"""CPU-only import ABI and memory selection controls. Never loads a driver."""
import ctypes
import importlib.util
import json
from pathlib import Path


def refuse_driver(*arguments, **keywords):
    raise AssertionError('CPU-only import checks must not load a driver')


ctypes.CDLL = refuse_driver
spec = importlib.util.spec_from_file_location('import_api', Path(__file__).resolve().parents[1]
                                               / 'scripts/native_vulkan_host_memory.py')
api = importlib.util.module_from_spec(spec)
spec.loader.exec_module(api)
checks = 0


def check(value):
    global checks
    assert value
    checks += 1


layouts = (
    ('VkBufferCreateInfo', api.BufferCreateInfo, 56,
     {'sType': 0, 'pNext': 8, 'flags': 16, 'size': 24, 'usage': 32, 'sharingMode': 36,
      'queueFamilyIndexCount': 40, 'pQueueFamilyIndices': 48}),
    ('VkExternalMemoryBufferCreateInfo', api.ExternalBufferCreateInfo, 24,
     {'sType': 0, 'pNext': 8, 'handleTypes': 16}),
    ('VkMemoryRequirements', api.MemoryRequirements, 24,
     {'size': 0, 'alignment': 8, 'memoryTypeBits': 16}),
    ('VkMemoryHostPointerPropertiesEXT', api.HostPointerProperties, 24,
     {'sType': 0, 'pNext': 8, 'memoryTypeBits': 16}),
    ('VkImportMemoryHostPointerInfoEXT', api.ImportHostPointerInfo, 32,
     {'sType': 0, 'pNext': 8, 'handleType': 16, 'pHostPointer': 24}),
    ('VkMemoryAllocateFlagsInfo', api.AllocateFlagsInfo, 24,
     {'sType': 0, 'pNext': 8, 'flags': 16, 'deviceMask': 20}),
    ('VkMemoryAllocateInfo', api.MemoryAllocateInfo, 32,
     {'sType': 0, 'pNext': 8, 'allocationSize': 16, 'memoryTypeIndex': 24}),
    ('VkCommandPoolCreateInfo', api.CommandPoolCreateInfo, 24,
     {'sType': 0, 'pNext': 8, 'flags': 16, 'queueFamilyIndex': 20}),
    ('VkCommandBufferAllocateInfo', api.CommandBufferAllocateInfo, 32,
     {'sType': 0, 'pNext': 8, 'commandPool': 16, 'level': 24, 'commandBufferCount': 28}),
    ('VkCommandBufferBeginInfo', api.CommandBufferBeginInfo, 32,
     {'sType': 0, 'pNext': 8, 'flags': 16, 'pInheritanceInfo': 24}),
    ('VkBufferCopy', api.BufferCopy, 24, {'srcOffset': 0, 'dstOffset': 8, 'size': 16}),
    ('VkMemoryBarrier', api.MemoryBarrier, 24,
     {'sType': 0, 'pNext': 8, 'srcAccessMask': 16, 'dstAccessMask': 20}),
    ('VkFenceCreateInfo', api.FenceCreateInfo, 24, {'sType': 0, 'pNext': 8, 'flags': 16}),
    ('VkSubmitInfo', api.SubmitInfo, 72,
     {'sType': 0, 'pNext': 8, 'waitSemaphoreCount': 16, 'pWaitSemaphores': 24,
      'pWaitDstStageMask': 32, 'commandBufferCount': 40, 'pCommandBuffers': 48,
      'signalSemaphoreCount': 56, 'pSignalSemaphores': 64}),
)
for name, binding, size, fields in layouts:
    check(ctypes.sizeof(binding) == size)
    check(all(getattr(binding, field).offset == offset for field, offset in fields.items()))

bindings = api.device_api().load_bindings()
types = (bindings.MemoryType * 3)()
for entry, flags in zip(types, (1, 6, 7)):
    entry.propertyFlags = flags
check(api.select_memory_type(7, 6, types) == 1)
check(api.select_memory_type(4, 7, types) == 2)
for masks in ((0, 7), (7, 0), (1, 7), (2, 4)):
    rejected = False
    try:
        api.select_memory_type(*masks, types)
    except RuntimeError:
        rejected = True
    check(rejected)
api.require_success(0, 'synthetic')
for status in (1, 2, -1, -4):
    rejected = False
    try:
        api.require_success(status, 'synthetic')
    except RuntimeError as error:
        rejected = str(error) == 'synthetic failed: ' + str(status)
    check(rejected)
print(json.dumps({'checks': checks, 'failures': 0, 'GPU_driver_loaded': False,
                  'game_inputs': False}))

# Exercise the fixed service's actual lifetime path with a driver stand-in. A
# simulated copy proves test plumbing only; hardware execution is a separate gate.
class FakeDevice:
    def __init__(self, failure=None, missing=None, empty=None):
        self.device = ctypes.c_void_p(101)
        self.queue = ctypes.c_void_p(102)
        self.physical = 103
        self.family = 2
        self.bindings = bindings
        self.import_support = {'minimum_import_alignment': 4096}
        self.failure = failure
        self.missing = missing
        self.empty = empty
        self.calls = []
        self.imported_pointer = None
        self.copy_region = None
        self.live = []

    def close(self):
        check(not self.live)
        self.calls.append('close_device')

    def function(self, name, result, arguments):
        if name == self.missing:
            raise RuntimeError('controlled missing command')

        def call(*values):
            self.calls.append(name)
            if name == self.failure:
                return -4
            outputs = {'vkCreateBuffer': (201, 'buffer'), 'vkAllocateMemory': (202, 'memory'),
                       'vkCreateCommandPool': (203, 'pool'), 'vkAllocateCommandBuffers': (204, None),
                       'vkCreateFence': (205, 'fence')}
            if name in outputs:
                identity, kind = outputs[name]
                if name != self.empty:
                    values[-1]._obj.value = identity
                    if kind:
                        self.live.append(kind)
            elif name == 'vkGetBufferMemoryRequirements':
                values[-1]._obj.size = 256 * 1024
                values[-1]._obj.alignment = 4096
                values[-1]._obj.memoryTypeBits = 1
            elif name == 'vkGetMemoryHostPointerPropertiesEXT':
                self.imported_pointer = values[2]
                values[-1]._obj.memoryTypeBits = 1
            elif name == 'vkGetPhysicalDeviceMemoryProperties':
                values[-1]._obj.memoryTypeCount = 1
                values[-1]._obj.memoryTypes[0].propertyFlags = 6
            elif name == 'vkCmdCopyBuffer':
                region = values[-1]._obj
                self.copy_region = (region.srcOffset, region.dstOffset, region.size)
            elif name == 'vkWaitForFences':
                source, destination, size = self.copy_region
                ctypes.memmove(self.imported_pointer + destination, self.imported_pointer + source, size)
            elif name.startswith('vkDestroy') or name == 'vkFreeMemory':
                kind = {'vkDestroyFence': 'fence', 'vkDestroyCommandPool': 'pool',
                        'vkDestroyBuffer': 'buffer', 'vkFreeMemory': 'memory'}[name]
                check(kind in self.live)
                if kind == 'memory':
                    check('buffer' not in self.live)
                self.live.remove(kind)
            return 0
        return call


class FakeModule:
    # probe_host_import defines its adapter subclass, then constructs this class.
    # Returning the already-created stand-in avoids invoking a real device path.
    class NativeDevice:
        def __new__(cls):
            return selected


real_device_api = api.device_api
api.device_api = lambda: FakeModule
try:
    selected = FakeDevice()
    result = api.probe_one_backing('shared_file')
    check(result['cpu_to_gpu_to_cpu_bytes_verified'])
    check(selected.calls[-5:] == ['vkDestroyFence', 'vkDestroyCommandPool',
                                 'vkDestroyBuffer', 'vkFreeMemory', 'close_device'])
    check(selected.calls.count('vkQueueSubmit') == 1)
    check(selected.copy_region == (0, 131072, 131072))
    check(not Path('/work/tmp/host-import-backing').exists())
    selected = FakeDevice()
    result = api.probe_one_backing('private_anonymous')
    check(result['cpu_to_gpu_to_cpu_bytes_verified'] and not result['shared_file_aliases_verified'])
    check(not Path('/work/tmp/host-import-backing').exists())
    for missing in ('vkDestroyBuffer', 'vkFreeMemory', 'vkDestroyCommandPool', 'vkDestroyFence',
                    'vkDeviceWaitIdle'):
        selected = FakeDevice(missing=missing)
        rejected = False
        try:
            api.probe_one_backing('shared_file')
        except RuntimeError as error:
            rejected = str(error) == 'controlled missing command'
        check(rejected and selected.calls == ['close_device'])
    for failure in ('vkCreateBuffer', 'vkGetMemoryHostPointerPropertiesEXT', 'vkAllocateMemory',
                    'vkBindBufferMemory', 'vkCreateCommandPool', 'vkAllocateCommandBuffers',
                    'vkBeginCommandBuffer', 'vkEndCommandBuffer', 'vkCreateFence',
                    'vkQueueSubmit', 'vkWaitForFences'):
        selected = FakeDevice(failure=failure)
        rejected = False
        try:
            api.probe_one_backing('shared_file')
        except RuntimeError as error:
            rejected = str(error).endswith('-4')
        check(rejected and selected.calls[-1] == 'close_device')
        check(not Path('/work/tmp/host-import-backing').exists())
        check(('vkDeviceWaitIdle' in selected.calls) ==
              (failure in ('vkQueueSubmit', 'vkWaitForFences')))
    for empty in ('vkCreateBuffer', 'vkAllocateMemory', 'vkCreateCommandPool',
                  'vkAllocateCommandBuffers', 'vkCreateFence'):
        selected = FakeDevice(empty=empty)
        rejected = False
        try:
            api.probe_one_backing('shared_file')
        except RuntimeError as error:
            rejected = 'empty' in str(error)
        check(rejected and selected.calls[-1] == 'close_device')
        check(not Path('/work/tmp/host-import-backing').exists())
finally:
    api.device_api = real_device_api
print(json.dumps({'total_checks': checks, 'failures': 0, 'GPU_driver_loaded': False,
                  'copy_execution': 'simulated only', 'game_inputs': False}))
