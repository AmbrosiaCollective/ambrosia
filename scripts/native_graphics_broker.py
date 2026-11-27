"""Fixed owned-device protocol for the trusted native Vulkan host service."""
import importlib.util
import json
import os
from pathlib import Path
import stat
import struct
import time


PACKET = struct.Struct('<8sIIIIiiiiIIII8x')
MAGIC = b'AMBGFX01'


def decode_request(data):
    if len(data) != PACKET.size:
        raise ValueError('invalid graphics packet size')
    magic, sequence, operation, owner, payload, x, y, width, height, flags, argument, reserved1, reserved2 = PACKET.unpack(data)
    if magic != MAGIC or not sequence or operation not in (1, 2, 3, 4):
        raise ValueError('invalid graphics command header')
    if any((payload, x, y, width, height, flags, argument, reserved1, reserved2)) or any(data[-8:]):
        raise ValueError('graphics command contains unsupported fields')
    if operation in (1, 4) and owner or operation in (2, 3) and not owner:
        raise ValueError('invalid graphics ownership request')
    return sequence, operation, owner


def response(sequence, operation, owner=0, status=0, api=0):
    return PACKET.pack(MAGIC, sequence, operation, owner, 0, 0, 0, 0, 0, status, api, 0, 0)


def open_endpoint(name, access):
    descriptor = os.open('/graphics/' + name, access | os.O_CLOEXEC | os.O_NOFOLLOW)
    information = os.fstat(descriptor)
    if not stat.S_ISREG(information.st_mode) or information.st_nlink != 1 or information.st_size != PACKET.size:
        os.close(descriptor)
        raise RuntimeError('invalid fixed graphics endpoint')
    return descriptor


def run_broker():
    path = Path(__file__).with_name('native_vulkan_device.py')
    spec = importlib.util.spec_from_file_location('trusted_owned_vulkan_device', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    request = open_endpoint('request', os.O_RDONLY)
    try:
        reply = open_endpoint('response', os.O_WRONLY)
        try:
            serve(request, reply, module.NativeDevice)
        finally:
            os.close(reply)
    finally:
        os.close(request)


def serve(request, reply, device_factory):
    """The factory is fixed by run_broker, never supplied over the protocol."""
    device = None
    owner = 0
    next_owner = 1
    last_sequence = 0
    requests = 0
    deadline = time.monotonic() + 20
    print(json.dumps({'native_graphics_service_ready': True, 'GPU_work_submitted': False}), flush=True)
    try:
        while time.monotonic() < deadline:
            if os.fstat(request).st_size != PACKET.size or os.fstat(reply).st_size != PACKET.size:
                raise RuntimeError('graphics endpoint size changed')
            data = os.pread(request, PACKET.size, 0)
            if data == bytes(PACKET.size):
                time.sleep(0.001)
                continue
            sequence, operation, requested_owner = decode_request(data)
            if sequence == last_sequence:
                time.sleep(0.001)
                continue
            if sequence != last_sequence + 1 or requests >= 1024:
                raise RuntimeError('graphics request sequence or lifetime budget exhausted')
            if os.pread(request, PACKET.size, 0) != data:
                continue
            status = 0
            result_owner = requested_owner
            api = 0
            if operation == 1:
                if device is not None or next_owner > 64:
                    status = 1
                else:
                    device = device_factory()
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
            # Commit only after the real operation completes. Never acknowledge a
            # failed driver call, publish a host pointer or reuse a released ID.
            encoded = response(sequence, operation, result_owner, status, api)
            if os.pwrite(reply, encoded, 0) != PACKET.size:
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
