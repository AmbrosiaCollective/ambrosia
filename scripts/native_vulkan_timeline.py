"""Real owned Vulkan timeline semaphores, without queue or shader submission."""
import ctypes
import importlib.util
from pathlib import Path


spec = importlib.util.spec_from_file_location('trusted_device_owner', Path(__file__).with_name('native_vulkan_device.py'))
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)


class SemaphoreTypeCreateInfo(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p),
                ('semaphoreType', ctypes.c_int), ('initialValue', ctypes.c_uint64)]


class SemaphoreCreateInfo(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p), ('flags', ctypes.c_uint32)]


class SemaphoreSignalInfo(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p),
                ('semaphore', ctypes.c_uint64), ('value', ctypes.c_uint64)]


def checked_value(value):
    if not isinstance(value, int) or not 0 <= value < 2**64 - 1:
        raise ValueError('native fence value is outside its bounded uint64 contract')
    return value


class TimelineDevice(base.NativeDevice):
    def __init__(self):
        self.fences = {}
        self.next_fence = 1
        self.destroy_semaphore = None
        super().__init__()
        try:
            self.create_semaphore = self.function('vkCreateSemaphore', ctypes.c_int,
                [ctypes.c_void_p, ctypes.POINTER(SemaphoreCreateInfo), ctypes.c_void_p,
                 ctypes.POINTER(ctypes.c_uint64)])
            self.destroy_semaphore = self.function('vkDestroySemaphore', None,
                [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_void_p])
            self.get_counter = self.function('vkGetSemaphoreCounterValue', ctypes.c_int,
                [ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64)])
            self.signal_semaphore = self.function('vkSignalSemaphore', ctypes.c_int,
                [ctypes.c_void_p, ctypes.POINTER(SemaphoreSignalInfo)])
        except BaseException:
            self.close()
            raise

    def create_fence(self, initial):
        initial = checked_value(initial)
        if len(self.fences) >= 32 or self.next_fence > 128:
            raise ValueError('native fence ownership budget exhausted')
        type_info = SemaphoreTypeCreateInfo(1000207002, None, 1, initial)
        description = SemaphoreCreateInfo(9, ctypes.addressof(type_info), 0)
        semaphore = ctypes.c_uint64()
        result = self.create_semaphore(self.device, ctypes.byref(description), None, ctypes.byref(semaphore))
        if result or not semaphore.value:
            raise RuntimeError('native timeline creation failed: ' + str(result))
        owner = self.next_fence
        try:
            self.fences[owner] = semaphore.value
        except BaseException:
            self.destroy_semaphore(self.device, semaphore, None)
            raise
        self.next_fence += 1
        return owner

    def fence(self, owner):
        semaphore = self.fences.get(owner)
        if semaphore is None:
            raise ValueError('native fence is not owned by this device')
        return semaphore

    def completed(self, owner):
        value = ctypes.c_uint64()
        result = self.get_counter(self.device, self.fence(owner), ctypes.byref(value))
        if result:
            raise RuntimeError('native timeline query failed: ' + str(result))
        return value.value

    def signal(self, owner, value):
        value = checked_value(value)
        current = self.completed(owner)
        if value < current:
            raise ValueError('native timeline cannot rewind a D3D12 fence')
        if value == current:
            return
        description = SemaphoreSignalInfo(1000207005, None, self.fence(owner), value)
        result = self.signal_semaphore(self.device, ctypes.byref(description))
        if result:
            raise RuntimeError('native host timeline signal failed: ' + str(result))

    def destroy_fence(self, owner):
        semaphore = self.fence(owner)
        self.destroy_semaphore(self.device, semaphore, None)
        del self.fences[owner]

    def close(self):
        if self.device.value and self.destroy_semaphore:
            for semaphore in self.fences.values():
                self.destroy_semaphore(self.device, semaphore, None)
            self.fences.clear()
        super().close()
