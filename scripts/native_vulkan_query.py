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


class ExtensionProperties(ctypes.Structure):
    _fields_ = [('extensionName', ctypes.c_char * 256), ('specVersion', ctypes.c_uint32)]


class MemoryType(ctypes.Structure):
    _fields_ = [('propertyFlags', ctypes.c_uint32), ('heapIndex', ctypes.c_uint32)]


class MemoryHeap(ctypes.Structure):
    _fields_ = [('size', ctypes.c_uint64), ('flags', ctypes.c_uint32)]


class MemoryProperties(ctypes.Structure):
    _fields_ = [('memoryTypeCount', ctypes.c_uint32), ('memoryTypes', MemoryType * 32),
                ('memoryHeapCount', ctypes.c_uint32), ('memoryHeaps', MemoryHeap * 16)]


class Properties2Storage(ctypes.Structure):
    # Core VkPhysicalDeviceProperties is written at offset 16. Overallocate its
    # aligned storage, as in the existing core query; never expose its identifiers.
    _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p),
                ('properties', ctypes.c_uint64 * 512)]


class ExternalHostProperties(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p),
                ('minImportedHostPointerAlignment', ctypes.c_uint64)]


class ExternalBufferInfo(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p),
                ('flags', ctypes.c_uint32), ('usage', ctypes.c_uint32),
                ('handleType', ctypes.c_uint32)]


class ExternalMemoryProperties(ctypes.Structure):
    _fields_ = [('externalMemoryFeatures', ctypes.c_uint32),
                ('exportFromImportedHandleTypes', ctypes.c_uint32),
                ('compatibleHandleTypes', ctypes.c_uint32)]


class ExternalBufferProperties(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p),
                ('externalMemoryProperties', ExternalMemoryProperties)]


def query_host_memory(function, instance, physical):
    """Report import candidates only; no logical device or host allocation exists."""
    enumerate_extensions = function(instance, 'vkEnumerateDeviceExtensionProperties', ctypes.c_int,
        [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint32),
         ctypes.POINTER(ExtensionProperties)])
    count = ctypes.c_uint32()
    if enumerate_extensions(physical, None, ctypes.byref(count), None) or count.value > 1024:
        raise RuntimeError('native device extension count failed or exceeds bounds')
    extensions = (ExtensionProperties * count.value)()
    if count.value:
        if enumerate_extensions(physical, None, ctypes.byref(count), extensions):
            raise RuntimeError('native device extension enumeration changed or failed')
        if count.value > len(extensions):
            raise RuntimeError('native device extension count exceeds its storage')
    host_extension = any(entry.extensionName == b'VK_EXT_external_memory_host'
                         for entry in extensions[:count.value])
    get_memory = function(instance, 'vkGetPhysicalDeviceMemoryProperties', None,
                          [ctypes.c_void_p, ctypes.POINTER(MemoryProperties)])
    memory = MemoryProperties()
    get_memory(physical, ctypes.byref(memory))
    if not 1 <= memory.memoryTypeCount <= 32 or not 1 <= memory.memoryHeapCount <= 16:
        raise RuntimeError('native memory counts outside fixed Vulkan bounds')
    types = memory.memoryTypes[:memory.memoryTypeCount]
    if any(entry.heapIndex >= memory.memoryHeapCount for entry in types):
        raise RuntimeError('native memory type refers to an invalid heap')
    result = {'extension_present': host_extension,
              'memory_type_flags': [int(entry.propertyFlags) for entry in types],
              'host_visible_coherent_available': any(entry.propertyFlags & 6 == 6 for entry in types),
              'minimum_import_alignment': None, 'buffer_candidates': []}
    if not host_extension:
        return result
    get_properties = function(instance, 'vkGetPhysicalDeviceProperties2', None,
                              [ctypes.c_void_p, ctypes.POINTER(Properties2Storage)])
    host = ExternalHostProperties(sType=1000178002)
    properties = Properties2Storage(sType=1000059001, pNext=ctypes.addressof(host))
    get_properties(physical, ctypes.byref(properties))
    ctypes.memset(ctypes.addressof(properties), 0, ctypes.sizeof(properties))
    alignment = host.minImportedHostPointerAlignment
    if not alignment or alignment & (alignment - 1):
        raise RuntimeError('native host import alignment is not a nonzero power of two')
    result['minimum_import_alignment'] = int(alignment)
    get_external = function(instance, 'vkGetPhysicalDeviceExternalBufferProperties', None,
        [ctypes.c_void_p, ctypes.POINTER(ExternalBufferInfo), ctypes.POINTER(ExternalBufferProperties)])
    # HOST_ALLOCATION and HOST_MAPPED_FOREIGN_MEMORY are distinct import classes.
    # Test ordinary renderer usage, then the same mask plus buffer device address.
    for handle in (0x80, 0x100):
        for usage in (0x1ff, 0x201ff):
            info = ExternalBufferInfo(1000071002, None, 0, usage, handle)
            output = ExternalBufferProperties(sType=1000071003)
            get_external(physical, ctypes.byref(info), ctypes.byref(output))
            flags = output.externalMemoryProperties
            result['buffer_candidates'].append({
                'handle_type': handle, 'usage': usage,
                'importable': bool(flags.externalMemoryFeatures & 4),
                'dedicated_only': bool(flags.externalMemoryFeatures & 1),
                'compatible_handle': bool(flags.compatibleHandleTypes & handle)})
    return result


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
                'host_memory_import': query_host_memory(function, instance, device),
            })
        return {'native_vulkan_host_query': True, 'public_capabilities': summaries,
                'logical_device_created': False, 'gpu_work_submitted': False,
                'guest_cpu_code_loaded': False, 'device_names_IDs_UUIDs_recorded': False}
    finally:
        destroy(instance, None)
