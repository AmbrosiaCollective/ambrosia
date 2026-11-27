"""CPU-only timeline ABI, values, ownership and lifetime checks. No driver."""
import ctypes
import importlib.util
import json
from pathlib import Path

saved_loader = ctypes.CDLL

def refuse_driver(*arguments, **keywords):
    raise AssertionError('CPU-only timeline check cannot load a driver')

ctypes.CDLL = refuse_driver
spec = importlib.util.spec_from_file_location('timeline', Path(__file__).resolve().parents[1] / 'scripts/native_vulkan_timeline.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
checks = 0

def check(value):
    global checks
    assert value
    checks += 1

check(ctypes.sizeof(module.SemaphoreTypeCreateInfo) == 32)
check(ctypes.sizeof(module.SemaphoreCreateInfo) == 24)
check(ctypes.sizeof(module.SemaphoreSignalInfo) == 32)
check(module.SemaphoreTypeCreateInfo.initialValue.offset == 24)
check(module.SemaphoreSignalInfo.semaphore.offset == 16)
check(module.SemaphoreSignalInfo.value.offset == 24)
for bad in (-1, 2**64 - 1, 2**64, 1.5, '1'):
    try:
        module.checked_value(bad)
    except ValueError:
        check(True)
    else:
        raise AssertionError('invalid fence value admitted')
check(module.checked_value(2**64 - 2) == 2**64 - 2)

object = module.TimelineDevice.__new__(module.TimelineDevice)
object.device = ctypes.c_void_p(11)
object.instance = ctypes.c_void_p(22)
object.queue = ctypes.c_void_p(33)
object.fences = {}
object.next_fence = 1
values = {}
released = []

def create(device, info_pointer, allocator, output):
    description = ctypes.cast(info_pointer, ctypes.POINTER(module.SemaphoreCreateInfo)).contents
    type_info = ctypes.cast(description.pNext, ctypes.POINTER(module.SemaphoreTypeCreateInfo)).contents
    check(device.value == 11 and description.sType == 9 and description.flags == 0)
    check(type_info.sType == 1000207002 and type_info.semaphoreType == 1)
    handle = len(values) + 100
    values[handle] = type_info.initialValue
    ctypes.cast(output, ctypes.POINTER(ctypes.c_uint64))[0] = handle
    return 0

def completed(device, semaphore, output):
    ctypes.cast(output, ctypes.POINTER(ctypes.c_uint64))[0] = values[semaphore]
    return 0

def signal(device, info_pointer):
    description = ctypes.cast(info_pointer, ctypes.POINTER(module.SemaphoreSignalInfo)).contents
    check(description.sType == 1000207005 and description.value > values[description.semaphore])
    values[description.semaphore] = description.value
    return 0

object.create_semaphore = create
object.get_counter = completed
object.signal_semaphore = signal
object.destroy_semaphore = lambda device, semaphore, allocator: released.append(('semaphore', semaphore))
object.destroy_device = lambda device, allocator: released.append(('device', device.value))
object.destroy_instance = lambda instance, allocator: released.append(('instance', instance.value))
owner = object.create_fence(7)
check(owner == 1 and object.completed(owner) == 7)
object.signal(owner, 9)
check(object.completed(owner) == 9)
object.signal(owner, 9)
check(object.completed(owner) == 9)
try:
    object.signal(owner, 8)
except ValueError:
    check(object.completed(owner) == 9)
else:
    raise AssertionError('timeline rewind admitted')
object.destroy_fence(owner)
try:
    object.completed(owner)
except ValueError:
    check(True)
else:
    raise AssertionError('released fence admitted')
second = object.create_fence(2**40)
check(second == 2 and object.completed(second) == 2**40)
object.close()
check(released[-3:][0][0] == 'semaphore' and released[-2:] == [('device', 11), ('instance', 22)])
check(not object.fences and not object.device.value)
object.close()
check(len(released) == 4)
original_create_device = module.base.NativeDevice.create_device
try:
    module.base.NativeDevice.create_device = lambda *arguments: None
    for device_class, expected in [(1, 1), (2, 2), (0, 0), (3, 0), (4, 0), (99, 0)]:
        selected = module.TimelineDevice.__new__(module.TimelineDevice)
        def physical_properties(physical, output):
            prefix = ctypes.cast(output, ctypes.POINTER(ctypes.c_uint32))
            prefix[4] = device_class
        def unsupported_external(physical, request, output):
            return None
        selected.function = lambda name, result, arguments: (
            physical_properties if name == 'vkGetPhysicalDeviceProperties' else unsupported_external)
        selected.create_device(1, 0, (), ())
        check(selected.heap_model == expected)
finally:
    module.base.NativeDevice.create_device = original_create_device
ctypes.CDLL = saved_loader
print(json.dumps({'CPU_timeline_checks': checks, 'failures': 0, 'driver_loaded': False}))
