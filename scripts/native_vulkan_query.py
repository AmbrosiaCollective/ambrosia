"""Trusted, fixed native Vulkan capability query. Never load guest code or submit GPU work.

ABI fields are checked against pinned Khronos headers. Device names, vendor IDs,
UUIDs and driver identifiers are deliberately omitted from the returned metadata.
"""
import ctypes
import os


VULKAN12_FIELDS = (
    'samplerMirrorClampToEdge',
    'drawIndirectCount',
    'storageBuffer8BitAccess',
    'uniformAndStorageBuffer8BitAccess',
    'storagePushConstant8',
    'shaderBufferInt64Atomics',
    'shaderSharedInt64Atomics',
    'shaderFloat16',
    'shaderInt8',
    'descriptorIndexing',
    'shaderInputAttachmentArrayDynamicIndexing',
    'shaderUniformTexelBufferArrayDynamicIndexing',
    'shaderStorageTexelBufferArrayDynamicIndexing',
    'shaderUniformBufferArrayNonUniformIndexing',
    'shaderSampledImageArrayNonUniformIndexing',
    'shaderStorageBufferArrayNonUniformIndexing',
    'shaderStorageImageArrayNonUniformIndexing',
    'shaderInputAttachmentArrayNonUniformIndexing',
    'shaderUniformTexelBufferArrayNonUniformIndexing',
    'shaderStorageTexelBufferArrayNonUniformIndexing',
    'descriptorBindingUniformBufferUpdateAfterBind',
    'descriptorBindingSampledImageUpdateAfterBind',
    'descriptorBindingStorageImageUpdateAfterBind',
    'descriptorBindingStorageBufferUpdateAfterBind',
    'descriptorBindingUniformTexelBufferUpdateAfterBind',
    'descriptorBindingStorageTexelBufferUpdateAfterBind',
    'descriptorBindingUpdateUnusedWhilePending',
    'descriptorBindingPartiallyBound',
    'descriptorBindingVariableDescriptorCount',
    'runtimeDescriptorArray',
    'samplerFilterMinmax',
    'scalarBlockLayout',
    'imagelessFramebuffer',
    'uniformBufferStandardLayout',
    'shaderSubgroupExtendedTypes',
    'separateDepthStencilLayouts',
    'hostQueryReset',
    'timelineSemaphore',
    'bufferDeviceAddress',
    'bufferDeviceAddressCaptureReplay',
    'bufferDeviceAddressMultiDevice',
    'vulkanMemoryModel',
    'vulkanMemoryModelDeviceScope',
    'vulkanMemoryModelAvailabilityVisibilityChains',
    'shaderOutputViewportIndex',
    'shaderOutputLayer',
    'subgroupBroadcastDynamicId',
)

VULKAN13_FIELDS = (
    'robustImageAccess',
    'inlineUniformBlock',
    'descriptorBindingInlineUniformBlockUpdateAfterBind',
    'pipelineCreationCacheControl',
    'privateData',
    'shaderDemoteToHelperInvocation',
    'shaderTerminateInvocation',
    'subgroupSizeControl',
    'computeFullSubgroups',
    'synchronization2',
    'textureCompressionASTC_HDR',
    'shaderZeroInitializeWorkgroupMemory',
    'dynamicRendering',
    'shaderIntegerDotProduct',
    'maintenance4',
)


class ApplicationInfo(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p),
                ('pApplicationName', ctypes.c_char_p), ('applicationVersion', ctypes.c_uint32),
                ('pEngineName', ctypes.c_char_p), ('engineVersion', ctypes.c_uint32),
                ('apiVersion', ctypes.c_uint32)]


class InstanceCreateInfo(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p), ('flags', ctypes.c_uint32),
                ('pApplicationInfo', ctypes.POINTER(ApplicationInfo)),
                ('enabledLayerCount', ctypes.c_uint32), ('ppEnabledLayerNames', ctypes.c_void_p),
                ('enabledExtensionCount', ctypes.c_uint32), ('ppEnabledExtensionNames', ctypes.c_void_p)]


class Features12(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p)] + [
        (name, ctypes.c_uint32) for name in VULKAN12_FIELDS]


class Features13(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p)] + [
        (name, ctypes.c_uint32) for name in VULKAN13_FIELDS]


class Features2(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p),
                ('features', ctypes.c_uint32 * 55)]


class QueueFamilyProperties(ctypes.Structure):
    _fields_ = [('queueFlags', ctypes.c_uint32), ('queueCount', ctypes.c_uint32),
                ('timestampValidBits', ctypes.c_uint32),
                ('minimumTransferGranularity', ctypes.c_uint32 * 3)]


def version_parts(version):
    return [(version >> 22) & 0x7f, (version >> 12) & 0x3ff, version & 0xfff]


def query_vulkan():
    # Select the installed hardware ICD explicitly. No home/layer search path is
    # inherited, and the reviewed namespace exposes no game or credential files.
    os.environ['VK_ICD_FILENAMES'] = '/usr/share/vulkan/icd.d/nvidia_icd.json'
    os.environ['VK_LAYER_PATH'] = '/work/no-layers'
    os.environ['VK_LOADER_DEBUG'] = 'error,warn'
    library = ctypes.CDLL('libvulkan.so.1')
    library.vkGetInstanceProcAddr.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    library.vkGetInstanceProcAddr.restype = ctypes.c_void_p

    def function(instance, name, result, arguments):
        address = library.vkGetInstanceProcAddr(instance, name.encode('ascii'))
        if not address:
            raise RuntimeError('required native Vulkan entry unavailable: ' + name)
        return ctypes.CFUNCTYPE(result, *arguments)(address)

    create = function(None, 'vkCreateInstance', ctypes.c_int,
                      [ctypes.POINTER(InstanceCreateInfo), ctypes.c_void_p,
                       ctypes.POINTER(ctypes.c_void_p)])
    application = ApplicationInfo(0, None, b'Ambrosia', 1, b'Ambrosia', 1, (1 << 22) | (3 << 12))
    description = InstanceCreateInfo(1, None, 0, ctypes.pointer(application), 0, None, 0, None)
    instance = ctypes.c_void_p()
    result = create(ctypes.byref(description), None, ctypes.byref(instance))
    if result or not instance.value:
        raise RuntimeError('native Vulkan instance creation failed: ' + str(result))
    destroy = function(instance, 'vkDestroyInstance', None, [ctypes.c_void_p, ctypes.c_void_p])
    try:
        enumerate_devices = function(instance, 'vkEnumeratePhysicalDevices', ctypes.c_int,
            [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_void_p)])
        count = ctypes.c_uint32()
        if enumerate_devices(instance, ctypes.byref(count), None) or not 1 <= count.value <= 8:
            raise RuntimeError('native Vulkan device count unavailable or outside bounds')
        devices = (ctypes.c_void_p * count.value)()
        if enumerate_devices(instance, ctypes.byref(count), devices):
            raise RuntimeError('native Vulkan device enumeration changed or failed')
        if not 1 <= count.value <= len(devices):
            raise RuntimeError('native Vulkan enumeration returned an invalid count')
        properties = function(instance, 'vkGetPhysicalDeviceProperties', None,
                              [ctypes.c_void_p, ctypes.c_void_p])
        features = function(instance, 'vkGetPhysicalDeviceFeatures2', None,
                            [ctypes.c_void_p, ctypes.POINTER(Features2)])
        queue_properties = function(instance, 'vkGetPhysicalDeviceQueueFamilyProperties', None,
            [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(QueueFamilyProperties)])
        summaries = []
        for index in range(count.value):
            device = devices[index]
            if not device:
                raise RuntimeError('native Vulkan enumeration returned an empty device')
            # Overallocate aligned storage for the fixed core properties ABI.
            # Read only version/type; erase names, IDs and UUIDs without logging.
            storage = (ctypes.c_uint64 * 512)()
            properties(device, ctypes.byref(storage))
            prefix = (ctypes.c_uint32 * 5).from_buffer(storage)
            api_version, device_type = int(prefix[0]), int(prefix[4])
            ctypes.memset(ctypes.addressof(storage), 0, ctypes.sizeof(storage))
            feature13 = Features13(sType=53)
            feature12 = Features12(sType=51, pNext=ctypes.addressof(feature13))
            feature2 = Features2(sType=1000059000, pNext=ctypes.addressof(feature12))
            features(device, ctypes.byref(feature2))
            queue_count = ctypes.c_uint32()
            queue_properties(device, ctypes.byref(queue_count), None)
            if not 1 <= queue_count.value <= 64:
                raise RuntimeError('native queue family count outside bounds')
            families = (QueueFamilyProperties * queue_count.value)()
            queue_properties(device, ctypes.byref(queue_count), families)
            if not 1 <= queue_count.value <= len(families):
                raise RuntimeError('native queue enumeration returned an invalid count')
            available_families = families[:queue_count.value]
            selected12 = ('timelineSemaphore', 'bufferDeviceAddress', 'runtimeDescriptorArray',
                          'descriptorBindingPartiallyBound', 'scalarBlockLayout', 'shaderFloat16',
                          'separateDepthStencilLayouts', 'drawIndirectCount')
            selected13 = ('dynamicRendering', 'synchronization2', 'subgroupSizeControl',
                          'computeFullSubgroups', 'maintenance4')
            summaries.append({
                'api': version_parts(api_version),
                'device_class': {0: 'other', 1: 'integrated', 2: 'discrete', 3: 'virtual', 4: 'cpu'}.get(device_type, 'unknown'),
                'graphics_queue': any(family.queueCount and family.queueFlags & 1 for family in available_families),
                'compute_queue': any(family.queueCount and family.queueFlags & 2 for family in available_families),
                'features12': {name: bool(getattr(feature12, name)) for name in selected12},
                'features13': {name: bool(getattr(feature13, name)) for name in selected13},
            })
        return {'native_vulkan_host_query': True, 'public_capabilities': summaries,
                'logical_device_created': False, 'gpu_work_submitted': False,
                'guest_cpu_code_loaded': False, 'device_names_IDs_UUIDs_recorded': False}
    finally:
        destroy(instance, None)
