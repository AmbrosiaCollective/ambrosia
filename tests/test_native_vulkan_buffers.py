"""CPU-only buffer bounds, ownership and failure cleanup. Never loads a driver."""
import ctypes
import importlib.util
import json
from pathlib import Path


def refuse_driver(*arguments, **keywords):
    raise AssertionError('CPU buffer tests must not load a Vulkan driver')


ctypes.CDLL = refuse_driver
spec = importlib.util.spec_from_file_location('buffers', Path(__file__).resolve().parents[1]
                                              / 'scripts/native_vulkan_buffers.py')
api = importlib.util.module_from_spec(spec)
spec.loader.exec_module(api)
bindings = api.abi.device_api().load_bindings()
checks = 0


def check(value):
    global checks
    assert value
    checks += 1


def rejected(operation):
    try:
        operation()
    except (ValueError, RuntimeError):
        check(True)
    else:
        raise AssertionError('invalid buffer operation was accepted')


check(ctypes.sizeof(api.BufferAddressInfo) == 24)
check(api.BufferAddressInfo.buffer.offset == 16)


class FakeDevice:
    def __init__(self, failure=None):
        self.device = ctypes.c_void_p(1)
        self.queue = ctypes.c_void_p(2)
        self.physical = 3
        self.family = 0
        self.bindings = bindings
        self.failure = failure
        self.next_handle = 10
        self.buffers = {}
        self.memories = {}
        self.bound = {}
        self.mapped = set()
        self.transients = set()
        self.region = None
        self.calls = []

    def function(self, name, result, arguments):
        def call(*values):
            self.calls.append(name)
            if name == self.failure:
                return -4
            if name == 'vkCreateBuffer':
                description = values[1]._obj
                check(description.usage == 0x201ff and description.flags == 0)
                identity = self.next_handle
                self.next_handle += 1
                self.buffers[identity] = description.size
                values[-1]._obj.value = identity
            elif name == 'vkGetBufferMemoryRequirements':
                values[-1]._obj.size = self.buffers[values[1].value]
                values[-1]._obj.alignment = 16
                values[-1]._obj.memoryTypeBits = 1
            elif name == 'vkGetPhysicalDeviceMemoryProperties':
                values[-1]._obj.memoryTypeCount = 1
                values[-1]._obj.memoryTypes[0].propertyFlags = 6
            elif name == 'vkAllocateMemory':
                identity = self.next_handle
                self.next_handle += 1
                size = values[1]._obj.allocationSize
                self.memories[identity] = ctypes.create_string_buffer(b'\x7b' * size, size)
                values[-1]._obj.value = identity
            elif name == 'vkBindBufferMemory':
                self.bound[values[1].value] = values[2].value
            elif name == 'vkMapMemory':
                identity = values[1].value
                self.mapped.add(identity)
                values[-1]._obj.value = ctypes.addressof(self.memories[identity])
            elif name == 'vkGetBufferDeviceAddress':
                return 4096 * values[1]._obj.buffer
            elif name == 'vkDestroyBuffer':
                del self.buffers[values[1].value]
                self.bound.pop(values[1].value, None)
            elif name == 'vkUnmapMemory':
                self.mapped.remove(values[1].value)
            elif name == 'vkFreeMemory':
                identity = values[1].value
                check(identity not in self.mapped and identity not in self.bound.values())
                del self.memories[identity]
            elif name in ('vkCreateCommandPool', 'vkCreateFence', 'vkAllocateCommandBuffers'):
                identity = self.next_handle
                self.next_handle += 1
                if name != 'vkAllocateCommandBuffers':
                    self.transients.add(identity)
                values[-1]._obj.value = identity
            elif name in ('vkDestroyCommandPool', 'vkDestroyFence'):
                self.transients.remove(values[1].value)
            elif name == 'vkCmdCopyBuffer':
                region = values[-1]._obj
                self.region = (values[1].value, values[2].value, region.srcOffset, region.dstOffset, region.size)
            elif name == 'vkWaitForFences':
                source, destination, first, second, size = self.region
                source_pointer = ctypes.addressof(self.memories[self.bound[source]])
                destination_pointer = ctypes.addressof(self.memories[self.bound[destination]])
                ctypes.memmove(destination_pointer + second, source_pointer + first, size)
            return 0
        return call


device = FakeDevice()
store = api.BufferStore(device)
first = store.create(128)
second = store.create(64)
check((first, second) == (1, 2) and store.allocated_bytes == 192)
check(store.read(first, 0, 128) == bytes(128))
store.write(first, 3, b'bytewise')
check(store.read(first, 3, 8) == b'bytewise')
store.write(first, 0, b'0123456789abcdef')
store.copy(first, 0, second, 4, 16)
check(store.read(second, 4, 16) == b'0123456789abcdef')
store.copy(first, 0, first, 64, 16)
check(store.read(first, 64, 16) == b'0123456789abcdef')
for operation in (lambda: store.create(0), lambda: store.create(True), lambda: store.create(2**64),
                  lambda: store.read(first, -1, 1), lambda: store.read(first, 128, 1),
                  lambda: store.read(first, 0, 0), lambda: store.read(first, 0, 2**64),
                  lambda: store.read(99, 0, 1), lambda: store.read(True, 0, 1),
                  lambda: store.write(first, 0, bytearray(b'abcd')),
                  lambda: store.copy(first, 0, first, 4, 8),
                  lambda: store.copy(first, 1, second, 0, 4),
                  lambda: store.copy(first, 0, second, 0, 3)):
    before = len(device.calls)
    rejected(operation)
    check(len(device.calls) == before)
store.destroy(first)
rejected(lambda: store.read(first, 0, 4))
third = store.create(32)
check(third == 3)
store.close()
store.close()
check(not store.buffers and store.allocated_bytes == 0)
check(not device.buffers and not device.memories and not device.transients)
for failure in ('vkCreateBuffer', 'vkAllocateMemory', 'vkBindBufferMemory', 'vkMapMemory'):
    device = FakeDevice(failure)
    store = api.BufferStore(device)
    rejected(lambda: store.create(128))
    check(not store.buffers and store.next_id == 1 and store.allocated_bytes == 0)
    check(not device.buffers and not device.memories and not device.mapped)
for failure in ('vkCreateCommandPool', 'vkAllocateCommandBuffers', 'vkBeginCommandBuffer',
                'vkEndCommandBuffer', 'vkCreateFence', 'vkQueueSubmit', 'vkWaitForFences'):
    device = FakeDevice()
    store = api.BufferStore(device)
    first = store.create(128)
    device.failure = failure
    rejected(lambda: store.copy(first, 0, first, 64, 16))
    check(not device.transients)
    check(('vkDeviceWaitIdle' in device.calls) == (failure in ('vkQueueSubmit', 'vkWaitForFences')))
    store.close()
    check(not device.buffers and not device.memories)
for next_id, allocated in ((129, 0), (1, api.BufferStore.maximum_total_bytes)):
    device = FakeDevice()
    store = api.BufferStore(device)
    store.next_id = next_id
    store.allocated_bytes = allocated
    before = len(device.calls)
    rejected(lambda: store.create(128))
    check(len(device.calls) == before)
print(json.dumps({'checks': checks, 'failures': 0, 'GPU_driver_loaded': False,
                  'copy_execution': 'simulated only', 'game_inputs': False}))
