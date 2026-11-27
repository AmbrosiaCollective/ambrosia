"""CPU-only wire-format and owned-buffer service controls, without Vulkan."""
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import struct
import tempfile
import threading
import time


def refuse_driver(*arguments, **keywords):
    raise AssertionError('CPU protocol tests must not load a driver')


ctypes.CDLL = refuse_driver
spec = importlib.util.spec_from_file_location('protocol', Path(__file__).resolve().parents[1]
                                              / 'scripts/native_graphics_buffer_broker.py')
api = importlib.util.module_from_spec(spec)
spec.loader.exec_module(api)
checks = 0


def check(value):
    global checks
    assert value
    checks += 1


def packet(sequence, operation, owner=0, value=0, count=0, destination=0, destination_offset=0):
    data = bytearray(64)
    struct.pack_into('<8sIIIIQ', data, 0, b'AMBGFX01', sequence, operation, owner, count, value)
    struct.pack_into('<I', data, 32, destination)
    struct.pack_into('<Q', data, 48, destination_offset)
    return bytes(data)


for operation in (1, 4):
    check(api.decode(packet(1, operation)) == (1, operation, 0, 0, 0, 0, 0))
for operation in (10, 11, 12, 13, 14):
    value = 65536 if operation == 10 else 0
    count = 4 if operation in (12, 13, 14) else 0
    destination = 2 if operation == 14 else 0
    check(api.decode(packet(1, operation, 1, value, count, destination)) ==
          (1, operation, 1, value, count, destination, 0))
invalid = [packet(0, 10, 1, 65536), packet(1, 10, 0, 65536), packet(1, 10, 1, 0),
           packet(1, 10, 1, 256 * 1024**2 + 1), packet(1, 11, 1, 1),
           packet(1, 12, 1, 0, 0), packet(1, 12, 1, 0, 1024**2 + 1),
           packet(1, 13, 1, 2**64 - 1, 1), packet(1, 14, 1, 0, 4, 0),
           packet(1, 14, 1, 1, 4, 2), packet(1, 14, 1, 0, 3, 2),
           packet(1, 14, 1, 0, 4, 2, 2**64 - 1), b'bad']
for index in (0, 36, 40, 44, 56, 63):
    data = bytearray(packet(1, 10, 1, 65536))
    data[index] = 1
    invalid.append(bytes(data))
for data in invalid:
    try:
        api.decode(data)
    except ValueError:
        check(True)
    else:
        raise AssertionError('invalid buffer packet was accepted')


class Record:
    device_address = 4096


class Store:
    def __init__(self):
        self.next_id = 1
        self.buffers = {}

    def create(self, size):
        identity = self.next_id
        self.next_id += 1
        self.buffers[identity] = bytearray(size)
        return identity

    def owned(self, identity):
        if identity not in self.buffers:
            raise ValueError('foreign buffer')
        return Record()

    def write(self, identity, offset, chunk):
        self.owned(identity)
        if offset + len(chunk) > len(self.buffers[identity]):
            raise ValueError('out of range')
        self.buffers[identity][offset:offset + len(chunk)] = chunk

    def read(self, identity, offset, size):
        self.owned(identity)
        if offset + size > len(self.buffers[identity]):
            raise ValueError('out of range')
        return bytes(self.buffers[identity][offset:offset + size])

    def copy(self, source, first, destination, second, size):
        self.write(destination, second, self.read(source, first, size))

    def destroy(self, identity):
        self.owned(identity)
        del self.buffers[identity]


class Device:
    api_version = [1, 3, 0]

    def __init__(self):
        self.buffers = Store()
        self.next_fence = 1
        self.closed = False

    def close(self):
        self.buffers.buffers.clear()
        self.closed = True


def service_check():
    with tempfile.TemporaryDirectory(dir='/work/tmp') as temporary:
        root = Path(temporary)
        descriptors = []
        for name, size in (('request', 64), ('response', 64), ('upload', 1024**2), ('download', 1024**2)):
            descriptor = os.open(root / name, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
            os.ftruncate(descriptor, size)
            descriptors.append(descriptor)
        devices = []
        errors = []

        def factory():
            device = Device()
            devices.append(device)
            return device

        def run():
            try:
                api.serve(*descriptors, factory)
            except BaseException as error:
                errors.append(error)

        worker = threading.Thread(target=run)
        worker.start()
        sequence = 0

        def exchange(operation, owner=0, value=0, count=0, destination=0, destination_offset=0):
            nonlocal sequence
            sequence += 1
            os.pwrite(descriptors[0], packet(sequence, operation, owner, value, count, destination,
                                            destination_offset), 0)
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                response = os.pread(descriptors[1], 64, 0)
                if struct.unpack_from('<I', response, 8)[0] == sequence:
                    check(struct.unpack_from('<I', response, 12)[0] == operation)
                    return (struct.unpack_from('<I', response, 16)[0],
                            struct.unpack_from('<I', response, 40)[0])
                if errors:
                    raise errors[0]
                time.sleep(0.001)
            raise AssertionError('protocol test response timed out')

        try:
            device, status = exchange(1)
            check(device == 1 and status == 0)
            check(exchange(10, 99, 65536)[1] == 1)
            first, status = exchange(10, device, 65536)
            second, status2 = exchange(10, device, 65536)
            check((first, second, status, status2) == (1, 2, 0, 0))
            pattern = b'known-buffer-pattern' * 40
            os.pwrite(descriptors[2], pattern, 0)
            check(exchange(12, first, 12, len(pattern))[1] == 0)
            check(exchange(14, first, 12, len(pattern), second, 24)[1] == 0)
            check(exchange(13, second, 24, len(pattern))[1] == 0)
            check(os.pread(descriptors[3], len(pattern), 0) == pattern)
            check(exchange(13, 99, 0, 4)[1] == 1)
            check(exchange(13, second, 65535, 4)[1] == 1)
            check(exchange(11, first)[1] == 0)
            check(exchange(13, first, 0, 4)[1] == 1)
            check(exchange(2, device)[1] == 0)
            check(devices[0].closed)
            new_device, status = exchange(1)
            new_buffer, status2 = exchange(10, new_device, 65536)
            check(new_device == 2 and new_buffer == 3 and status == status2 == 0)
            exchange(4)
            worker.join(2)
            check(not worker.is_alive() and not errors and devices[-1].closed)
        finally:
            if worker.is_alive():
                # Force the service's size guard to terminate before closing FDs.
                os.ftruncate(descriptors[0], 0)
                worker.join(2)
            for descriptor in descriptors:
                os.close(descriptor)


service_check()
print(json.dumps({'checks': checks, 'failures': 0, 'GPU_driver_loaded': False,
                  'transport_execution': 'stand-in device only', 'game_inputs': False}))
