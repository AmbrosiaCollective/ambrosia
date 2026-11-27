"""Typed owned-buffer bridge. No CPU guest entrypoint, shaders or host handles."""
import importlib.util
import json
import os
from pathlib import Path
import stat
import struct
import time


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


fences = module('buffer_bridge_fences', 'native_graphics_fence_broker.py')
base = fences.base
CHUNK_BYTES = 1024**2


def decode(data):
    if len(data) != 64:
        raise ValueError('invalid graphics packet size')
    operation = struct.unpack_from('<I', data, 12)[0]
    if operation <= 9:
        sequence, operation, owner, value = fences.decode(data)
        return sequence, operation, owner, value, 0, 0, 0
    magic, sequence, operation, owner, count = struct.unpack_from('<8sIIII', data)
    value = struct.unpack_from('<Q', data, 24)[0]
    destination, reserved, flags, api = struct.unpack_from('<IIII', data, 32)
    destination_offset, padding = struct.unpack_from('<QQ', data, 48)
    if magic != base.MAGIC or not sequence or not owner or operation not in (10, 11, 12, 13, 14):
        raise ValueError('invalid native buffer command')
    if reserved or flags or api or padding:
        raise ValueError('unsupported native buffer command fields')
    if operation == 10:
        if count or destination or destination_offset or not 1 <= value <= 256 * 1024**2:
            raise ValueError('invalid native buffer size or creation fields')
    elif operation == 11:
        if any((count, value, destination, destination_offset)):
            raise ValueError('unsupported native buffer destruction fields')
    else:
        if not 1 <= count <= CHUNK_BYTES or value > 256 * 1024**2 - count:
            raise ValueError('native transfer exceeds bounded range')
        if operation == 14:
            if not destination or destination_offset > 256 * 1024**2 - count:
                raise ValueError('invalid native buffer copy destination')
            if value % 4 or destination_offset % 4 or count % 4:
                raise ValueError('native buffer copy range is unaligned')
        elif destination or destination_offset:
            raise ValueError('unsupported native transfer destination fields')
    return sequence, operation, owner, value, count, destination, destination_offset


def open_payload(name, access):
    descriptor = os.open('/graphics/' + name, access | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        information = os.fstat(descriptor)
        if not stat.S_ISREG(information.st_mode) or information.st_nlink != 1 or information.st_size != CHUNK_BYTES:
            raise RuntimeError('invalid native buffer payload file')
    except BaseException:
        os.close(descriptor)
        raise
    return descriptor


def serve(request, reply, upload, download, device_factory):
    device = None
    owner = 0
    next_owner = next_fence = next_buffer = 1
    last_sequence = requests = 0
    deadline = time.monotonic() + 20
    print(json.dumps({'native_graphics_service_ready': True, 'owned_buffer_transfers': True}), flush=True)
    try:
        while time.monotonic() < deadline:
            for descriptor, expected in ((request, 64), (reply, 64), (upload, CHUNK_BYTES), (download, CHUNK_BYTES)):
                if os.fstat(descriptor).st_size != expected:
                    raise RuntimeError('native graphics endpoint size changed')
            data = os.pread(request, 64, 0)
            if data == bytes(64):
                time.sleep(0.001)
                continue
            sequence, operation, requested_owner, value, count, destination, destination_offset = decode(data)
            if sequence == last_sequence:
                time.sleep(0.001)
                continue
            if sequence != last_sequence + 1 or requests >= 1024:
                raise RuntimeError('graphics sequence or lifetime budget exhausted')
            if os.pread(request, 64, 0) != data:
                continue
            status = result_value = api = 0
            result_owner = requested_owner
            if operation == 1:
                if device is not None or next_owner > 64:
                    status = 1
                else:
                    device = device_factory()
                    device.next_fence = next_fence
                    device.buffers.next_id = next_buffer
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
                        raise ValueError('no device owns this request')
                    if operation == 5:
                        if requested_owner != owner:
                            raise ValueError('foreign device fence creation')
                        result_owner = device.create_fence(value, shared=bool(base.PACKET.unpack(data)[9]))
                        next_fence = device.next_fence
                    elif operation == 6:
                        result_value = device.completed(requested_owner)
                    elif operation == 7:
                        device.signal(requested_owner, value)
                    elif operation == 8:
                        device.destroy_fence(requested_owner)
                    elif operation == 9:
                        if requested_owner != owner or device.heap_model not in (1, 2):
                            raise ValueError('native heap model unavailable')
                        result_value = device.heap_model
                    elif operation == 10:
                        if requested_owner != owner:
                            raise ValueError('foreign device buffer creation')
                        result_owner = device.buffers.create(value)
                        next_buffer = device.buffers.next_id
                        result_value = device.buffers.owned(result_owner).device_address
                    elif operation == 11:
                        device.buffers.destroy(requested_owner)
                    elif operation == 12:
                        chunk = os.pread(upload, count, 0)
                        if len(chunk) != count:
                            raise RuntimeError('native upload payload was truncated')
                        device.buffers.write(requested_owner, value, chunk)
                    elif operation == 13:
                        chunk = device.buffers.read(requested_owner, value, count)
                        if os.pwrite(download, chunk, 0) != count:
                            raise RuntimeError('native download payload write was truncated')
                    elif operation == 14:
                        device.buffers.copy(requested_owner, value, destination, destination_offset, count)
                except ValueError:
                    status = 1
            encoded = fences.response(sequence, operation, result_owner, status, result_value, api)
            if os.pwrite(reply, encoded, 0) != 64:
                raise RuntimeError('graphics response write was truncated')
            last_sequence = sequence
            requests += 1
            # Do not log payload bytes, addresses or caller offsets.
            print(json.dumps({'native_graphics_operation': operation, 'owner': result_owner,
                              'status': status}), flush=True)
            if operation == 4:
                return
    finally:
        if device is not None:
            device.close()


def run_broker():
    timeline = module('buffer_bridge_timeline', 'native_vulkan_timeline.py')
    buffers = module('buffer_bridge_store', 'native_vulkan_buffers.py')

    class BufferedDevice(timeline.TimelineDevice):
        def __init__(self):
            self.buffers = None
            super().__init__()
            try:
                self.buffers = buffers.BufferStore(self)
            except BaseException:
                self.close()
                raise

        def create_device(self, physical, family, required12, required13):
            super().create_device(physical, family, required12, required13)
            self.physical = physical

        def close(self):
            if self.buffers is not None:
                self.buffers.close()
            super().close()

    descriptors = []
    try:
        descriptors.append(base.open_endpoint('request', os.O_RDONLY))
        descriptors.append(base.open_endpoint('response', os.O_WRONLY))
        descriptors.append(open_payload('upload', os.O_RDONLY))
        descriptors.append(open_payload('download', os.O_WRONLY))
        serve(*descriptors, BufferedDevice)
    finally:
        for descriptor in reversed(descriptors):
            os.close(descriptor)
