"""CPU-only external timeline ABI/capability/creation checks; no driver."""
import ctypes
import importlib.util
import json
from pathlib import Path

spec = importlib.util.spec_from_file_location('timeline', Path(__file__).resolve().parents[1] / 'scripts/native_vulkan_timeline.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
checks = 0

def check(value):
    global checks
    assert value
    checks += 1

check(ctypes.sizeof(module.ExternalSemaphoreInfo) == 24)
check(ctypes.sizeof(module.ExternalSemaphoreProperties) == 32)
check(ctypes.sizeof(module.ExportSemaphoreInfo) == 24)
check(module.ExternalSemaphoreProperties.externalSemaphoreFeatures.offset == 24)
original = module.base.NativeDevice.create_device
try:
    extension_requests = []
    module.base.NativeDevice.create_device = lambda *arguments: extension_requests.append(arguments[-1])
    for features, compatible, expected in [(3, 1, True), (1, 1, False), (2, 1, False), (3, 0, False), (0, 1, False)]:
        device = module.TimelineDevice.__new__(module.TimelineDevice)
        def query(physical, request_pointer, output):
            request = ctypes.cast(request_pointer, ctypes.POINTER(module.ExternalSemaphoreInfo)).contents
            timeline = ctypes.cast(request.pNext, ctypes.POINTER(module.SemaphoreTypeCreateInfo)).contents
            check(request.sType == 1000076000 and request.handleType == 1 and timeline.semaphoreType == 1)
            properties = ctypes.cast(output, ctypes.POINTER(module.ExternalSemaphoreProperties)).contents
            properties.externalSemaphoreFeatures = features
            properties.compatibleHandleTypes = compatible
        def physical(physical, output):
            ctypes.cast(output, ctypes.POINTER(ctypes.c_uint32))[4] = 2
        device.function = lambda name, result, arguments: physical if name == 'vkGetPhysicalDeviceProperties' else query
        device.create_device(1, 0, (), ())
        check(device.shared_supported == expected)
        check(extension_requests[-1] == (('VK_KHR_external_semaphore_fd',) if expected else ()))
finally:
    module.base.NativeDevice.create_device = original

owner = module.TimelineDevice.__new__(module.TimelineDevice)
owner.device = ctypes.c_void_p(1)
owner.fences = {}
owner.next_fence = 1
owner.shared_supported = True

def create(device, description_pointer, allocator, output):
    description = ctypes.cast(description_pointer, ctypes.POINTER(module.SemaphoreCreateInfo)).contents
    timeline = ctypes.cast(description.pNext, ctypes.POINTER(module.SemaphoreTypeCreateInfo)).contents
    export = ctypes.cast(timeline.pNext, ctypes.POINTER(module.ExportSemaphoreInfo)).contents
    check(timeline.initialValue == 7 and export.sType == 1000077000 and export.handleTypes == 1 and not export.pNext)
    ctypes.cast(output, ctypes.POINTER(ctypes.c_uint64))[0] = 44
    return 0

owner.create_semaphore = create
released = []
owner.destroy_semaphore = lambda device, semaphore, allocator: released.append(semaphore)
fence = owner.create_fence(7, shared=True)
check(fence == 1 and owner.fences[fence] == 44)
owner.destroy_fence(fence)
check(released == [44])
owner.shared_supported = False
try:
    owner.create_fence(0, shared=True)
except ValueError:
    check(not owner.fences)
else:
    raise AssertionError('unsupported sharing was admitted')
print(json.dumps({'CPU_shared_timeline_checks': checks, 'failures': 0, 'driver_loaded': False}))
