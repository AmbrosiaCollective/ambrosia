"""CPU-only ABI and failure/lifetime controls. Never load a GPU driver."""
import ctypes
import importlib.util
import json
from pathlib import Path

saved_loader = ctypes.CDLL

def refuse_driver(*arguments, **keywords):
    raise AssertionError('CPU-only device checks must not load a driver')

ctypes.CDLL = refuse_driver
spec = importlib.util.spec_from_file_location('device', Path(__file__).resolve().parents[1] / 'scripts/native_vulkan_device.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
api = module.load_bindings()
checks = 0

def check(value):
    global checks
    assert value
    checks += 1

check(ctypes.sizeof(module.DeviceQueueCreateInfo) == 40)
check(ctypes.sizeof(module.DeviceCreateInfo) == 72)
check(module.DeviceQueueCreateInfo.pQueuePriorities.offset == 32)
check(module.DeviceCreateInfo.pQueueCreateInfos.offset == 24)
check(module.DeviceCreateInfo.pEnabledFeatures.offset == 64)

released = []
class PartialFailure(module.NativeDevice):
    def open(self):
        self.instance = ctypes.c_void_p(11)
        self.device = ctypes.c_void_p(22)
        self.queue = ctypes.c_void_p(33)
        self.destroy_instance = lambda handle, allocator: released.append(('instance', handle.value))
        self.destroy_device = lambda handle, allocator: released.append(('device', handle.value))
        raise RuntimeError('controlled partial initialization failure')

try:
    PartialFailure()
except RuntimeError as error:
    check(str(error) == 'controlled partial initialization failure')
else:
    raise AssertionError('partial initialization must fail')
check(released == [('device', 22), ('instance', 11)])

class Fixture(module.NativeDevice):
    def open(self):
        self.instance = ctypes.c_void_p(11)
        self.destroy_instance = lambda handle, allocator: released.append(('instance', handle.value))
        self.creation_result = 0
        self.queue_present = True
        self.commands = []

    def function(self, name, result, arguments):
        self.commands.append(name)
        if name == 'vkCreateDevice':
            def create(physical, description_pointer, allocator, output):
                description = ctypes.cast(description_pointer, ctypes.POINTER(module.DeviceCreateInfo)).contents
                queue = description.pQueueCreateInfos.contents
                check(description.sType == 3 and description.queueCreateInfoCount == 1)
                check(queue.sType == 2 and queue.queueFamilyIndex == 2 and queue.queueCount == 1)
                check(queue.pQueuePriorities[0] == 1.0)
                enabled12 = ctypes.cast(description.pNext, ctypes.POINTER(api.Features12)).contents
                enabled13 = ctypes.cast(enabled12.pNext, ctypes.POINTER(api.Features13)).contents
                check(enabled12.timelineSemaphore == 1 and enabled13.synchronization2 == 1)
                check(enabled12.bufferDeviceAddress == 0 and enabled13.dynamicRendering == 0)
                if self.creation_result == 0:
                    ctypes.cast(output, ctypes.POINTER(ctypes.c_void_p))[0] = 22
                return self.creation_result
            return create
        if name == 'vkDestroyDevice':
            return lambda handle, allocator: released.append(('device', handle.value))
        if name == 'vkGetDeviceQueue':
            def get_queue(device, family, index, output):
                check(device.value == 22 and family == 2 and index == 0)
                if self.queue_present:
                    ctypes.cast(output, ctypes.POINTER(ctypes.c_void_p))[0] = 33
            return get_queue
        raise AssertionError('unexpected bootstrap entrypoint')

fixture = Fixture()
fixture.create_device(1, 2, ('timelineSemaphore',), ('synchronization2',))
check(fixture.device.value == 22 and fixture.queue.value == 33 and fixture.family == 2)
fixture.close()
check(not fixture.device.value and not fixture.queue.value and not fixture.instance.value)
count = len(released)
fixture.close()
check(len(released) == count)
fixture = Fixture()
fixture.creation_result = -3
try:
    fixture.create_device(1, 2, ('timelineSemaphore',), ('synchronization2',))
except RuntimeError as error:
    check(str(error).endswith('-3'))
else:
    raise AssertionError('failed creation must not return a device')
finally:
    fixture.close()
fixture = Fixture()
fixture.queue_present = False
try:
    fixture.create_device(1, 2, ('timelineSemaphore',), ('synchronization2',))
except RuntimeError as error:
    check('empty handle' in str(error))
else:
    raise AssertionError('empty queue must fail')
finally:
    fixture.close()
check(released[-2:] == [('device', 22), ('instance', 11)])
ctypes.CDLL = saved_loader
print(json.dumps({'CPU_device_checks': checks, 'failures': 0, 'driver_loaded': False}))
