"""CPU-only host-memory query layouts and rejection controls; no driver loading."""
import ctypes
import importlib.util
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('memory_api', root / 'scripts/native_vulkan_query.py')
api = importlib.util.module_from_spec(spec)
spec.loader.exec_module(api)
def refuse_driver(*arguments, **keywords):
    raise AssertionError('CPU memory query checks must not load a driver')

ctypes.CDLL = refuse_driver
checks = 0

def check(value):
    global checks
    assert value
    checks += 1

# Independent x86-64 C ABI expectations, checked against the pinned declarations.
layouts = (
    ('VkExtensionProperties', api.ExtensionProperties, 260, {'extensionName': 0, 'specVersion': 256}),
    ('VkMemoryType', api.MemoryType, 8, {'propertyFlags': 0, 'heapIndex': 4}),
    ('VkMemoryHeap', api.MemoryHeap, 16, {'size': 0, 'flags': 8}),
    ('VkPhysicalDeviceMemoryProperties', api.MemoryProperties, 520,
     {'memoryTypeCount': 0, 'memoryTypes': 4, 'memoryHeapCount': 260, 'memoryHeaps': 264}),
    ('VkPhysicalDeviceExternalMemoryHostPropertiesEXT', api.ExternalHostProperties, 24,
     {'sType': 0, 'pNext': 8, 'minImportedHostPointerAlignment': 16}),
    ('VkPhysicalDeviceExternalBufferInfo', api.ExternalBufferInfo, 32,
     {'sType': 0, 'pNext': 8, 'flags': 16, 'usage': 20, 'handleType': 24}),
    ('VkExternalMemoryProperties', api.ExternalMemoryProperties, 12,
     {'externalMemoryFeatures': 0, 'exportFromImportedHandleTypes': 4, 'compatibleHandleTypes': 8}),
    ('VkExternalBufferProperties', api.ExternalBufferProperties, 32,
     {'sType': 0, 'pNext': 8, 'externalMemoryProperties': 16}),
)
for name, binding, size, fields in layouts:
    check(ctypes.sizeof(binding) == size)
    check(all(getattr(binding, field).offset == offset for field, offset in fields.items()))
check(api.Properties2Storage.properties.offset == 16 and ctypes.sizeof(api.Properties2Storage) >= 840)
class FakeDriver:
    def __init__(self, **overrides):
        self.options = {'count': 1, 'returned_count': 1, 'status': 0, 'extension': True,
                        'types': 1, 'heaps': 1, 'heap_index': 0, 'alignment': 4096}
        self.options.update(overrides)
        self.lookups = []
        self.buffer_calls = []

    def function(self, instance, name, result, arguments):
        self.lookups.append(name)
        return getattr(self, name)

    def vkEnumerateDeviceExtensionProperties(self, physical, layer, count, entries):
        check(layer is None)
        if entries is None:
            count._obj.value = self.options['count']
            return 0
        if len(entries):
            entries[0].extensionName = (b'VK_EXT_external_memory_host' if self.options['extension']
                                        else b'VK_KHR_example')
        count._obj.value = self.options['returned_count']
        return self.options['status']

    def vkGetPhysicalDeviceMemoryProperties(self, physical, output):
        memory = output._obj
        memory.memoryTypeCount = self.options['types']
        memory.memoryHeapCount = self.options['heaps']
        memory.memoryTypes[0].propertyFlags = 6
        memory.memoryTypes[0].heapIndex = self.options['heap_index']

    def vkGetPhysicalDeviceProperties2(self, physical, output):
        check(output._obj.sType == 1000059001)
        host = ctypes.cast(output._obj.pNext, ctypes.POINTER(api.ExternalHostProperties)).contents
        check(host.sType == 1000178002)
        host.minImportedHostPointerAlignment = self.options['alignment']

    def vkGetPhysicalDeviceExternalBufferProperties(self, physical, info, output):
        check(info._obj.sType == 1000071002 and output._obj.sType == 1000071003)
        self.buffer_calls.append((info._obj.handleType, info._obj.usage))
        flags = output._obj.externalMemoryProperties
        flags.externalMemoryFeatures = 5
        flags.compatibleHandleTypes = info._obj.handleType

driver = FakeDriver()
result = api.query_host_memory(driver.function, None, 1)
check(result['extension_present'] and result['minimum_import_alignment'] == 4096)
check(result['memory_type_flags'] == [6] and result['host_visible_coherent_available'])
check(driver.buffer_calls == [(128, 511), (128, 131583), (256, 511), (256, 131583)])
check(all(item['importable'] and item['dedicated_only'] and item['compatible_handle']
          for item in result['buffer_candidates']))
for overrides in ({'extension': False}, {'count': 0}):
    driver = FakeDriver(**overrides)
    result = api.query_host_memory(driver.function, None, 1)
    check(not result['extension_present'] and result['minimum_import_alignment'] is None)
    check(result['buffer_candidates'] == [] and not driver.buffer_calls)
    check('vkGetPhysicalDeviceProperties2' not in driver.lookups)
for overrides in ({'count': 1025}, {'status': 5}, {'returned_count': 2}, {'types': 0},
                  {'types': 33}, {'heaps': 0}, {'heaps': 17}, {'heap_index': 1},
                  {'alignment': 0}, {'alignment': 3}):
    driver = FakeDriver(**overrides)
    rejected = False
    try:
        api.query_host_memory(driver.function, None, 1)
    except RuntimeError:
        rejected = True
    check(rejected)
print(json.dumps({'checks': checks, 'failures': 0, 'GPU_driver_loaded': False,
                  'game_inputs': False, 'layout_checks': True}))
