"""Fixed synthetic host-allocation import and transfer probes. No CPU guest entrypoint.

Try one bounded scratch-file mapping and a private anonymous control. Each GPU
command copies a known CPU pattern between nonoverlapping halves of one buffer.
No shaders, external handles, game paths or caller-supplied commands are accepted.
"""
import ctypes
import importlib.util
import json
import mmap
import os
from pathlib import Path


def device_api():
    path = Path(__file__).with_name('native_vulkan_device.py')
    spec = importlib.util.spec_from_file_location('host_memory_device', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class BufferCreateInfo(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p), ('flags', ctypes.c_uint32),
                ('size', ctypes.c_uint64), ('usage', ctypes.c_uint32), ('sharingMode', ctypes.c_int),
                ('queueFamilyIndexCount', ctypes.c_uint32), ('pQueueFamilyIndices', ctypes.c_void_p)]


class ExternalBufferCreateInfo(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p), ('handleTypes', ctypes.c_uint32)]


class MemoryRequirements(ctypes.Structure):
    _fields_ = [('size', ctypes.c_uint64), ('alignment', ctypes.c_uint64), ('memoryTypeBits', ctypes.c_uint32)]


class HostPointerProperties(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p), ('memoryTypeBits', ctypes.c_uint32)]


class ImportHostPointerInfo(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p),
                ('handleType', ctypes.c_uint32), ('pHostPointer', ctypes.c_void_p)]


class AllocateFlagsInfo(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p),
                ('flags', ctypes.c_uint32), ('deviceMask', ctypes.c_uint32)]


class MemoryAllocateInfo(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p),
                ('allocationSize', ctypes.c_uint64), ('memoryTypeIndex', ctypes.c_uint32)]


class CommandPoolCreateInfo(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p),
                ('flags', ctypes.c_uint32), ('queueFamilyIndex', ctypes.c_uint32)]


class CommandBufferAllocateInfo(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p), ('commandPool', ctypes.c_uint64),
                ('level', ctypes.c_int), ('commandBufferCount', ctypes.c_uint32)]


class CommandBufferBeginInfo(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p),
                ('flags', ctypes.c_uint32), ('pInheritanceInfo', ctypes.c_void_p)]


class BufferCopy(ctypes.Structure):
    _fields_ = [('srcOffset', ctypes.c_uint64), ('dstOffset', ctypes.c_uint64), ('size', ctypes.c_uint64)]


class MemoryBarrier(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p),
                ('srcAccessMask', ctypes.c_uint32), ('dstAccessMask', ctypes.c_uint32)]


class FenceCreateInfo(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p), ('flags', ctypes.c_uint32)]


class SubmitInfo(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p), ('waitSemaphoreCount', ctypes.c_uint32),
                ('pWaitSemaphores', ctypes.c_void_p), ('pWaitDstStageMask', ctypes.c_void_p),
                ('commandBufferCount', ctypes.c_uint32), ('pCommandBuffers', ctypes.c_void_p),
                ('signalSemaphoreCount', ctypes.c_uint32), ('pSignalSemaphores', ctypes.c_void_p)]


def require_success(result, operation):
    if result:
        raise RuntimeError(operation + ' failed: ' + str(result))


def select_memory_type(requirement_bits, pointer_bits, types):
    compatible = requirement_bits & pointer_bits
    for index, entry in enumerate(types):
        if compatible & (1 << index) and entry.propertyFlags & 6 == 6:
            return index
    raise RuntimeError('host import has no compatible host-visible coherent memory type')


def probe_one_backing(backing_kind):
    if backing_kind not in ('shared_file', 'private_anonymous'):
        raise ValueError('unsupported fixed host import backing')
    device_module = device_api()

    class ImportDevice(device_module.NativeDevice):
        def create_device(self, physical, family, required12, required13, extensions=()):
            self.import_support = self.bindings.query_host_memory(
                lambda instance, name, result, arguments: self.function(name, result, arguments),
                self.instance, physical)
            supported = self.import_support
            candidate = any(item['handle_type'] == 128 and item['usage'] == 0x201ff
                            and item['importable'] and item['compatible_handle']
                            and not item['dedicated_only'] for item in supported['buffer_candidates'])
            if not candidate or not 1 <= supported['minimum_import_alignment'] <= 65536:
                raise RuntimeError('fixed host import candidate unavailable or exceeds alignment bound')
            super().create_device(physical, family, required12, required13, ('VK_EXT_external_memory_host',))
            self.physical = physical

    device = ImportDevice()
    buffer = ctypes.c_uint64()
    memory = ctypes.c_uint64()
    pool = ctypes.c_uint64()
    fence = ctypes.c_uint64()
    descriptor = None
    mapping = alias = None
    submitted = False
    path = Path('/work/tmp/host-import-backing')
    handle = ctypes.c_uint64
    pointer = ctypes.c_void_p

    def command(name, result, arguments):
        return device.function(name, result, arguments)

    # Resolve destruction before allocating anything it must release.
    try:
        destroy_buffer = command('vkDestroyBuffer', None, [pointer, handle, pointer])
        free_memory = command('vkFreeMemory', None, [pointer, handle, pointer])
        destroy_pool = command('vkDestroyCommandPool', None, [pointer, handle, pointer])
        destroy_fence = command('vkDestroyFence', None, [pointer, handle, pointer])
        wait_idle = command('vkDeviceWaitIdle', ctypes.c_int, [pointer])
    except BaseException:
        device.close()
        raise
    try:
        size = 256 * 1024
        external = ExternalBufferCreateInfo(1000072000, None, 128)
        description = BufferCreateInfo(12, ctypes.addressof(external), 0, size, 0x201ff, 0, 0, None)
        create = command('vkCreateBuffer', ctypes.c_int,
                         [pointer, ctypes.POINTER(BufferCreateInfo), pointer, ctypes.POINTER(handle)])
        require_success(create(device.device, ctypes.byref(description), None, ctypes.byref(buffer)),
                        'host import buffer creation')
        if not buffer.value:
            raise RuntimeError('host import returned an empty buffer')
        requirements = MemoryRequirements()
        get_requirements = command('vkGetBufferMemoryRequirements', None,
                                   [pointer, handle, ctypes.POINTER(MemoryRequirements)])
        get_requirements(device.device, buffer, ctypes.byref(requirements))
        alignment = max(device.import_support['minimum_import_alignment'], requirements.alignment)
        if not alignment or alignment & (alignment - 1) or alignment > 65536:
            raise RuntimeError('buffer alignment outside fixed import probe bounds')
        allocation_bytes = (requirements.size + alignment - 1) & ~(alignment - 1)
        if not size <= allocation_bytes <= 1024 * 1024:
            raise RuntimeError('buffer memory requirement outside fixed import probe bounds')
        if backing_kind == 'shared_file':
            descriptor = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR | os.O_NOFOLLOW, 0o600)
            os.ftruncate(descriptor, allocation_bytes + alignment)
            mapping = mmap.mmap(descriptor, allocation_bytes + alignment)
            alias = mmap.mmap(descriptor, allocation_bytes + alignment)
        else:
            mapping = mmap.mmap(-1, allocation_bytes + alignment,
                                flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS)
            alias = mapping
        address = ctypes.addressof(ctypes.c_char.from_buffer(mapping))
        prefix = (-address) % alignment
        imported_pointer = address + prefix
        # Materialize the complete imported extent before asking the driver to
        # pin it. The file initially consists of holes; this stays below 1 MiB.
        mapping[prefix:prefix + allocation_bytes] = bytes(allocation_bytes)
        host_properties = HostPointerProperties(sType=1000178001)
        get_pointer = command('vkGetMemoryHostPointerPropertiesEXT', ctypes.c_int,
                              [pointer, ctypes.c_int, pointer, ctypes.POINTER(HostPointerProperties)])
        require_success(get_pointer(device.device, 128, imported_pointer, ctypes.byref(host_properties)),
                        'host pointer memory properties')
        properties = device.bindings.MemoryProperties()
        get_properties = command('vkGetPhysicalDeviceMemoryProperties', None,
                                 [pointer, ctypes.POINTER(device.bindings.MemoryProperties)])
        get_properties(device.physical, ctypes.byref(properties))
        if not 1 <= properties.memoryTypeCount <= 32:
            raise RuntimeError('host import memory type count outside bounds')
        index = select_memory_type(requirements.memoryTypeBits, host_properties.memoryTypeBits,
                                   properties.memoryTypes[:properties.memoryTypeCount])
        print(json.dumps({'host_import_candidate': True, 'backing_kind': backing_kind,
                          'allocation_bytes': allocation_bytes,
                          'alignment': alignment, 'buffer_memory_type_bits': requirements.memoryTypeBits,
                          'pointer_memory_type_bits': host_properties.memoryTypeBits,
                          'selected_memory_type': index}), flush=True)
        imported = ImportHostPointerInfo(1000178000, None, 128, imported_pointer)
        flags = AllocateFlagsInfo(1000060000, ctypes.addressof(imported), 2, 0)
        allocation = MemoryAllocateInfo(5, ctypes.addressof(flags), allocation_bytes, index)
        allocate = command('vkAllocateMemory', ctypes.c_int,
                           [pointer, ctypes.POINTER(MemoryAllocateInfo), pointer, ctypes.POINTER(handle)])
        require_success(allocate(device.device, ctypes.byref(allocation), None, ctypes.byref(memory)),
                        'host memory import')
        if not memory.value:
            raise RuntimeError('host import returned empty memory')
        bind = command('vkBindBufferMemory', ctypes.c_int, [pointer, handle, handle, ctypes.c_uint64])
        require_success(bind(device.device, buffer, memory, 0), 'imported buffer binding')
        half = size // 2
        pattern = b'\x4c\x3d\x2e\x1f' * (half // 4)
        alias[prefix:prefix + half] = pattern
        alias[prefix + half:prefix + size] = bytes(half)
        if mapping[prefix:prefix + half] != pattern:
            raise RuntimeError('host import scratch aliases are not coherent')
        pool_info = CommandPoolCreateInfo(39, None, 0, device.family)
        create_pool = command('vkCreateCommandPool', ctypes.c_int,
                              [pointer, ctypes.POINTER(CommandPoolCreateInfo), pointer, ctypes.POINTER(handle)])
        require_success(create_pool(device.device, ctypes.byref(pool_info), None, ctypes.byref(pool)),
                        'fixed transfer pool creation')
        if not pool.value:
            raise RuntimeError('fixed transfer returned an empty command pool')
        allocate_info = CommandBufferAllocateInfo(40, None, pool.value, 0, 1)
        commands = pointer()
        allocate_commands = command('vkAllocateCommandBuffers', ctypes.c_int,
                                    [pointer, ctypes.POINTER(CommandBufferAllocateInfo), ctypes.POINTER(pointer)])
        require_success(allocate_commands(device.device, ctypes.byref(allocate_info), ctypes.byref(commands)),
                        'fixed transfer command allocation')
        if not commands.value:
            raise RuntimeError('fixed transfer returned an empty command buffer')
        begin_info = CommandBufferBeginInfo(42, None, 1, None)
        begin = command('vkBeginCommandBuffer', ctypes.c_int, [pointer, ctypes.POINTER(CommandBufferBeginInfo)])
        require_success(begin(commands, ctypes.byref(begin_info)), 'fixed transfer recording begin')
        barrier = command('vkCmdPipelineBarrier', None,
                          [pointer, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
                           ctypes.c_uint32, ctypes.POINTER(MemoryBarrier), ctypes.c_uint32, pointer,
                           ctypes.c_uint32, pointer])
        before = MemoryBarrier(46, None, 0x4000, 0x1800)
        barrier(commands, 0x4000, 0x1000, 0, 1, ctypes.byref(before), 0, None, 0, None)
        copy = command('vkCmdCopyBuffer', None,
                       [pointer, handle, handle, ctypes.c_uint32, ctypes.POINTER(BufferCopy)])
        region = BufferCopy(0, half, half)
        copy(commands, buffer, buffer, 1, ctypes.byref(region))
        after = MemoryBarrier(46, None, 0x1000, 0x2000)
        barrier(commands, 0x1000, 0x4000, 0, 1, ctypes.byref(after), 0, None, 0, None)
        end = command('vkEndCommandBuffer', ctypes.c_int, [pointer])
        require_success(end(commands), 'fixed transfer recording end')
        fence_info = FenceCreateInfo(8, None, 0)
        create_fence = command('vkCreateFence', ctypes.c_int,
                               [pointer, ctypes.POINTER(FenceCreateInfo), pointer, ctypes.POINTER(handle)])
        require_success(create_fence(device.device, ctypes.byref(fence_info), None, ctypes.byref(fence)),
                        'fixed transfer fence creation')
        if not fence.value:
            raise RuntimeError('fixed transfer returned an empty fence')
        submit_info = SubmitInfo(4, None, 0, None, None, 1, ctypes.addressof(commands), 0, None)
        submit = command('vkQueueSubmit', ctypes.c_int, [pointer, ctypes.c_uint32, ctypes.POINTER(SubmitInfo), handle])
        # A failed submit can also lose the device; cleanup drains before freeing.
        submitted = True
        require_success(submit(device.queue, 1, ctypes.byref(submit_info), fence), 'fixed transfer submission')
        wait = command('vkWaitForFences', ctypes.c_int,
                       [pointer, ctypes.c_uint32, ctypes.POINTER(handle), ctypes.c_uint32, ctypes.c_uint64])
        require_success(wait(device.device, 1, ctypes.byref(fence), 1, 5_000_000_000), 'fixed transfer completion')
        submitted = False
        if alias[prefix + half:prefix + size] != pattern or mapping[prefix + half:prefix + size] != pattern:
            raise RuntimeError('GPU transfer did not preserve the imported CPU pattern')
        return {'host_allocation_imported': True, 'buffer_bound': True, 'gpu_work_submitted': True,
                'fixed_nonoverlapping_copy_bytes': half, 'cpu_to_gpu_to_cpu_bytes_verified': True,
                'shared_file_aliases_verified': backing_kind == 'shared_file',
                'host_visible_coherent_type': True,
                'host_handles_returned': False, 'guest_cpu_code_loaded': False, 'game_inputs': False}
    finally:
        if submitted:
            # The parent wall watchdog remains authoritative if the driver hangs.
            # Device loss also permits object teardown; other failures fail closed.
            drained = wait_idle(device.device)
            if drained not in (0, -4):
                os._exit(79)
        if fence.value:
            destroy_fence(device.device, fence, None)
        if pool.value:
            destroy_pool(device.device, pool, None)
        if buffer.value:
            destroy_buffer(device.device, buffer, None)
        if memory.value:
            free_memory(device.device, memory, None)
        if alias is not None and alias is not mapping:
            alias.close()
        if mapping is not None:
            mapping.close()
        if descriptor is not None:
            os.close(descriptor)
            path.unlink()
        device.close()


def probe_host_import():
    results = []
    for kind in ('shared_file', 'private_anonymous'):
        try:
            result = probe_one_backing(kind)
        except RuntimeError as error:
            result = {'verified': False, 'error': str(error)}
        else:
            result['verified'] = True
        results.append({'backing_kind': kind, **result})
    return {'host_memory_import_probes': results,
            'shared_backing_verified': results[0]['verified'],
            'guest_cpu_code_loaded': False, 'game_inputs': False}
