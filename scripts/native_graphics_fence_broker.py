"""Fixed native ownership protocol with bounded real timeline-fence operations."""
import importlib.util
import json
import os
from pathlib import Path
import struct
import time


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


base = module('trusted_owned_device_protocol', 'native_graphics_broker.py')


def decode(data):
    if len(data) != 64:
        raise ValueError('invalid graphics packet size')
    fields = base.PACKET.unpack(data)
    if fields[2] <= 4:
        sequence, operation, owner = base.decode_request(data)
        return sequence, operation, owner, 0
    if fields[0] != base.MAGIC or not fields[1] or fields[2] not in (5, 6, 7, 8, 9) or not fields[3]:
        raise ValueError('invalid native fence command')
    if any(fields[index] for index in (4, 7, 8, 9, 10, 11, 12)) or any(data[-8:]):
        raise ValueError('unsupported native fence command fields')
    value = struct.unpack_from('<Q', data, 24)[0]
    if value == 2**64 - 1 or fields[2] in (6, 8, 9) and value:
        raise ValueError('invalid native fence command value')
    return fields[1], fields[2], fields[3], value


def response(sequence, operation, owner, status=0, value=0, api=0):
    data = bytearray(base.response(sequence, operation, owner, status, api))
    struct.pack_into('<Q', data, 24, value)
    return data


def serve(request, reply, device_factory):
    device = None
    owner = 0
    next_owner = 1
    next_fence_id = 1
    last_sequence = 0
    requests = 0
    deadline = time.monotonic() + 20
    print(json.dumps({'native_graphics_service_ready': True, 'timeline_fences': True,
                      'GPU_work_submitted': False}), flush=True)
    try:
        while time.monotonic() < deadline:
            if os.fstat(request).st_size != 64 or os.fstat(reply).st_size != 64:
                raise RuntimeError('graphics endpoint size changed')
            data = os.pread(request, 64, 0)
            if data == bytes(64):
                time.sleep(0.001)
                continue
            sequence, operation, requested_owner, value = decode(data)
            if sequence == last_sequence:
                time.sleep(0.001)
                continue
            if sequence != last_sequence + 1 or requests >= 1024:
                raise RuntimeError('graphics request sequence or lifetime budget exhausted')
            if os.pread(request, 64, 0) != data:
                continue
            status = 0
            result_owner = requested_owner
            result_value = 0
            api = 0
            if operation == 1:
                if device is not None or next_owner > 64:
                    status = 1
                else:
                    device = device_factory()
                    device.next_fence = next_fence_id
                    owner = next_owner
                    next_owner += 1
                    result_owner = owner
                    api = (device.api_version[0] << 22) | (device.api_version[1] << 12) | device.api_version[2]
            elif operation in (2, 3):
                if device is None or requested_owner != owner:
                    status = 1
                elif operation == 2:
                    device.close()
                    device = None
                    owner = 0
                else:
                    api = (device.api_version[0] << 22) | (device.api_version[1] << 12) | device.api_version[2]
            elif operation == 4:
                if device is not None:
                    device.close()
                    device = None
                    owner = 0
            else:
                try:
                    if device is None:
                        raise ValueError('no device owns this fence request')
                    if operation == 5:
                        if requested_owner != owner:
                            raise ValueError('foreign device fence creation')
                        result_owner = device.create_fence(value)
                        next_fence_id = device.next_fence
                    elif operation == 6:
                        result_value = device.completed(requested_owner)
                    elif operation == 7:
                        device.signal(requested_owner, value)
                    elif operation == 8:
                        device.destroy_fence(requested_owner)
                    elif operation == 9:
                        if requested_owner != owner or device.heap_model not in (1, 2):
                            raise ValueError('native device heap model is unavailable')
                        result_value = device.heap_model
                except ValueError:
                    status = 1
            encoded = response(sequence, operation, result_owner, status, result_value, api)
            if os.pwrite(reply, encoded, 0) != 64:
                raise RuntimeError('graphics response write was truncated')
            last_sequence = sequence
            requests += 1
            print(json.dumps({'native_graphics_operation': operation, 'owner': result_owner,
                              'status': status, 'GPU_work_submitted': False}), flush=True)
            if operation == 4:
                return
    finally:
        if device is not None:
            device.close()


def run_broker():
    timeline = module('trusted_timeline_device', 'native_vulkan_timeline.py')
    request = base.open_endpoint('request', os.O_RDONLY)
    try:
        reply = base.open_endpoint('response', os.O_WRONLY)
        try:
            serve(request, reply, timeline.TimelineDevice)
        finally:
            os.close(reply)
    finally:
        os.close(request)
