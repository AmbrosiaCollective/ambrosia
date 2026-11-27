"""Owned native Vulkan device for a fixed trusted host service.

No guest CPU code, shader or command submission is accepted here. Host pointers
stay private to this process; future transports must expose owned numeric IDs.
"""
import ctypes
import importlib.util
from pathlib import Path


def load_bindings():
    path = Path(__file__).with_name('native_vulkan_query.py')
    spec = importlib.util.spec_from_file_location('trusted_vulkan_bindings', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class DeviceQueueCreateInfo(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p),
                ('flags', ctypes.c_uint32), ('queueFamilyIndex', ctypes.c_uint32),
                ('queueCount', ctypes.c_uint32), ('pQueuePriorities', ctypes.POINTER(ctypes.c_float))]


class DeviceCreateInfo(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p), ('flags', ctypes.c_uint32),
                ('queueCreateInfoCount', ctypes.c_uint32),
                ('pQueueCreateInfos', ctypes.POINTER(DeviceQueueCreateInfo)),
                ('enabledLayerCount', ctypes.c_uint32), ('ppEnabledLayerNames', ctypes.c_void_p),
                ('enabledExtensionCount', ctypes.c_uint32), ('ppEnabledExtensionNames', ctypes.c_void_p),
                ('pEnabledFeatures', ctypes.c_void_p)]


class NativeDevice:
    """Own one instance/device/queue; release in reverse order on every path."""

    def __init__(self):
        self.instance = ctypes.c_void_p()
        self.device = ctypes.c_void_p()
        self.queue = ctypes.c_void_p()
        self.destroy_instance = None
        self.destroy_device = None
        self.bindings = load_bindings()
        try:
            self.open()
        except BaseException:
            self.close()
            raise

    def function(self, name, result, arguments):
        address = self.library.vkGetInstanceProcAddr(self.instance, name.encode('ascii'))
        if not address:
            raise RuntimeError('required native Vulkan entry unavailable: ' + name)
        return ctypes.CFUNCTYPE(result, *arguments)(address)

    def open(self):
        import os
        os.environ['VK_ICD_FILENAMES'] = '/usr/share/vulkan/icd.d/nvidia_icd.json'
        os.environ['VK_LAYER_PATH'] = '/work/no-layers'
        os.environ['VK_LOADER_DEBUG'] = 'error,warn'
        self.library = ctypes.CDLL('libvulkan.so.1')
        self.library.vkGetInstanceProcAddr.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        self.library.vkGetInstanceProcAddr.restype = ctypes.c_void_p
        api = self.bindings
        create_instance = self.function('vkCreateInstance', ctypes.c_int,
            [ctypes.POINTER(api.InstanceCreateInfo), ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)])
        application = api.ApplicationInfo(0, None, b'Ambrosia', 1, b'Ambrosia', 1, (1 << 22) | (3 << 12))
        description = api.InstanceCreateInfo(1, None, 0, ctypes.pointer(application), 0, None, 0, None)
        result = create_instance(ctypes.byref(description), None, ctypes.byref(self.instance))
        if result or not self.instance.value:
            raise RuntimeError('native Vulkan instance creation failed: ' + str(result))
        self.destroy_instance = self.function('vkDestroyInstance', None, [ctypes.c_void_p, ctypes.c_void_p])
        enumerate_devices = self.function('vkEnumeratePhysicalDevices', ctypes.c_int,
            [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_void_p)])
        count = ctypes.c_uint32()
        if enumerate_devices(self.instance, ctypes.byref(count), None) or not 1 <= count.value <= 8:
            raise RuntimeError('native physical device count outside bounds')
        devices = (ctypes.c_void_p * count.value)()
        if enumerate_devices(self.instance, ctypes.byref(count), devices) or not 1 <= count.value <= len(devices):
            raise RuntimeError('native physical device enumeration failed or changed')
        properties = self.function('vkGetPhysicalDeviceProperties', None, [ctypes.c_void_p, ctypes.c_void_p])
        features = self.function('vkGetPhysicalDeviceFeatures2', None,
            [ctypes.c_void_p, ctypes.POINTER(api.Features2)])
        queue_properties = self.function('vkGetPhysicalDeviceQueueFamilyProperties', None,
            [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(api.QueueFamilyProperties)])
        for physical in devices[:count.value]:
            if not physical:
                raise RuntimeError('native device enumeration returned an empty handle')
            storage = (ctypes.c_uint64 * 512)()
            properties(physical, ctypes.byref(storage))
            version = ctypes.c_uint32.from_buffer(storage).value
            ctypes.memset(ctypes.addressof(storage), 0, ctypes.sizeof(storage))
            if version < ((1 << 22) | (3 << 12)):
                continue
            supported13 = api.Features13(sType=53)
            supported12 = api.Features12(sType=51, pNext=ctypes.addressof(supported13))
            supported = api.Features2(sType=1000059000, pNext=ctypes.addressof(supported12))
            features(physical, ctypes.byref(supported))
            required12 = ('timelineSemaphore', 'bufferDeviceAddress', 'runtimeDescriptorArray',
                          'descriptorBindingPartiallyBound', 'scalarBlockLayout', 'shaderFloat16',
                          'separateDepthStencilLayouts', 'drawIndirectCount')
            required13 = ('dynamicRendering', 'synchronization2', 'subgroupSizeControl',
                          'computeFullSubgroups', 'maintenance4')
            if not all(getattr(supported12, name) for name in required12):
                continue
            if not all(getattr(supported13, name) for name in required13):
                continue
            queue_count = ctypes.c_uint32()
            queue_properties(physical, ctypes.byref(queue_count), None)
            if not 1 <= queue_count.value <= 64:
                raise RuntimeError('native queue count outside bounds')
            families = (api.QueueFamilyProperties * queue_count.value)()
            queue_properties(physical, ctypes.byref(queue_count), families)
            if not 1 <= queue_count.value <= len(families):
                raise RuntimeError('native queue enumeration changed')
            for family_index, family in enumerate(families[:queue_count.value]):
                if family.queueCount and family.queueFlags & 3 == 3:
                    self.create_device(physical, family_index, required12, required13)
                    self.api_version = api.version_parts(version)
                    return
        raise RuntimeError('no native device meets the renderer bootstrap requirements')

    def create_device(self, physical, family, required12, required13, extensions=()):
        api = self.bindings
        enabled13 = api.Features13(sType=53, **dict.fromkeys(required13, 1))
        enabled12 = api.Features12(sType=51, pNext=ctypes.addressof(enabled13),
                                   **dict.fromkeys(required12, 1))
        priority = ctypes.c_float(1.0)
        queue_info = DeviceQueueCreateInfo(2, None, 0, family, 1, ctypes.pointer(priority))
        if extensions not in ((), ('VK_KHR_external_semaphore_fd',),
                              ('VK_EXT_external_memory_host',)):
            raise ValueError('unsupported fixed native device extension selection')
        extension_names = (ctypes.c_char_p * len(extensions))(
            *(name.encode('ascii') for name in extensions))
        extension_pointer = ctypes.cast(extension_names, ctypes.c_void_p) if extensions else None
        description = DeviceCreateInfo(3, ctypes.addressof(enabled12), 0, 1,
                                      ctypes.pointer(queue_info), 0, None,
                                      len(extensions), extension_pointer, None)
        create = self.function('vkCreateDevice', ctypes.c_int,
            [ctypes.c_void_p, ctypes.POINTER(DeviceCreateInfo), ctypes.c_void_p,
             ctypes.POINTER(ctypes.c_void_p)])
        self.destroy_device = self.function('vkDestroyDevice', None, [ctypes.c_void_p, ctypes.c_void_p])
        get_queue = self.function('vkGetDeviceQueue', None,
            [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.POINTER(ctypes.c_void_p)])
        result = create(physical, ctypes.byref(description), None, ctypes.byref(self.device))
        if result or not self.device.value:
            raise RuntimeError('native Vulkan device creation failed: ' + str(result))
        # Resolve lifetime/queue commands before allocation so lookup failure
        # cannot strand an owned device. No queue work is submitted here.
        get_queue(self.device, family, 0, ctypes.byref(self.queue))
        if not self.queue.value:
            raise RuntimeError('native Vulkan queue lookup returned an empty handle')
        self.family = family

    def close(self):
        # No work is submitted, so there is no queue to drain before destruction.
        self.queue = ctypes.c_void_p()
        if self.device.value and self.destroy_device:
            self.destroy_device(self.device, None)
            self.device = ctypes.c_void_p()
        if self.instance.value and self.destroy_instance:
            self.destroy_instance(self.instance, None)
            self.instance = ctypes.c_void_p()


def bootstrap_device():
    device = NativeDevice()
    try:
        return {'native_vulkan_device_bootstrap': True, 'api': device.api_version,
                'logical_device_created': bool(device.device.value),
                'graphics_compute_queue_obtained': bool(device.queue.value),
                'gpu_work_submitted': False, 'guest_cpu_code_loaded': False,
                'host_handles_returned': False}
    finally:
        device.close()
