"""Owned coherent Vulkan buffers for explicit CPU/physical-page transfers.

No caller-supplied pointer or GPU handle is accepted. Callers serialize this
store with queue submission: CPU reads/writes occur only after preceding GPU
work completes. The initial copy operation waits synchronously before returning.
"""
import ctypes
import importlib.util
import os
from pathlib import Path


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


abi = module('buffer_transfer_abi', 'native_vulkan_host_memory.py')


class BufferAddressInfo(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p), ('buffer', ctypes.c_uint64)]


def checked_integer(value, minimum, maximum, description):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError(description + ' outside its fixed bound')
    return value


class Buffer:
    def __init__(self, size):
        self.size = size
        self.buffer = ctypes.c_uint64()
        self.memory = ctypes.c_uint64()
        self.pointer = ctypes.c_void_p()
        self.device_address = 0
        self.allocation_size = 0


class BufferStore:
    maximum_buffer_bytes = 256 * 1024**2
    maximum_total_bytes = 1024**3
    maximum_chunk_bytes = 1024**2

    def __init__(self, device):
        self.device = device
        self.buffers = {}
        self.next_id = 1
        self.allocated_bytes = 0
        pointer = ctypes.c_void_p
        handle = ctypes.c_uint64
        self.destroy_buffer = device.function('vkDestroyBuffer', None, [pointer, handle, pointer])
        self.free_memory = device.function('vkFreeMemory', None, [pointer, handle, pointer])
        self.unmap_memory = device.function('vkUnmapMemory', None, [pointer, handle])

    def create(self, size):
        checked_integer(size, 1, self.maximum_buffer_bytes, 'buffer size')
        if len(self.buffers) >= 32 or self.next_id > 128:
            raise ValueError('native buffer ownership budget exhausted')
        if size > self.maximum_total_bytes - self.allocated_bytes:
            raise ValueError('native buffer byte budget exhausted')
        record = Buffer(size)
        pointer = ctypes.c_void_p
        handle = ctypes.c_uint64
        device = self.device
        try:
            description = abi.BufferCreateInfo(12, None, 0, size, 0x201ff, 0, 0, None)
            create = device.function('vkCreateBuffer', ctypes.c_int,
                [pointer, ctypes.POINTER(abi.BufferCreateInfo), pointer, ctypes.POINTER(handle)])
            abi.require_success(create(device.device, ctypes.byref(description), None,
                                       ctypes.byref(record.buffer)), 'native buffer creation')
            if not record.buffer.value:
                raise RuntimeError('native buffer creation returned an empty handle')
            requirements = abi.MemoryRequirements()
            get_requirements = device.function('vkGetBufferMemoryRequirements', None,
                [pointer, handle, ctypes.POINTER(abi.MemoryRequirements)])
            get_requirements(device.device, record.buffer, ctypes.byref(requirements))
            if not size <= requirements.size <= self.maximum_buffer_bytes:
                raise RuntimeError('native buffer allocation exceeds the individual byte budget')
            if requirements.size > self.maximum_total_bytes - self.allocated_bytes:
                raise ValueError('native buffer allocation exceeds the total byte budget')
            properties = device.bindings.MemoryProperties()
            get_properties = device.function('vkGetPhysicalDeviceMemoryProperties', None,
                [pointer, ctypes.POINTER(device.bindings.MemoryProperties)])
            get_properties(device.physical, ctypes.byref(properties))
            if not 1 <= properties.memoryTypeCount <= 32:
                raise RuntimeError('native memory type count outside fixed bounds')
            index = abi.select_memory_type(requirements.memoryTypeBits, requirements.memoryTypeBits,
                                            properties.memoryTypes[:properties.memoryTypeCount])
            flags = abi.AllocateFlagsInfo(1000060000, None, 2, 0)
            allocation = abi.MemoryAllocateInfo(5, ctypes.addressof(flags), requirements.size, index)
            allocate = device.function('vkAllocateMemory', ctypes.c_int,
                [pointer, ctypes.POINTER(abi.MemoryAllocateInfo), pointer, ctypes.POINTER(handle)])
            abi.require_success(allocate(device.device, ctypes.byref(allocation), None,
                                         ctypes.byref(record.memory)), 'native buffer memory allocation')
            if not record.memory.value:
                raise RuntimeError('native buffer memory allocation returned an empty handle')
            bind = device.function('vkBindBufferMemory', ctypes.c_int, [pointer, handle, handle, ctypes.c_uint64])
            abi.require_success(bind(device.device, record.buffer, record.memory, 0), 'native buffer binding')
            map_memory = device.function('vkMapMemory', ctypes.c_int,
                [pointer, handle, ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint32, ctypes.POINTER(pointer)])
            abi.require_success(map_memory(device.device, record.memory, 0, requirements.size, 0,
                                           ctypes.byref(record.pointer)), 'native coherent memory mapping')
            if not record.pointer.value:
                raise RuntimeError('native coherent memory mapping returned an empty pointer')
            get_address = device.function('vkGetBufferDeviceAddress', ctypes.c_uint64,
                                           [pointer, ctypes.POINTER(BufferAddressInfo)])
            address_info = BufferAddressInfo(1000244001, None, record.buffer.value)
            record.device_address = get_address(device.device, ctypes.byref(address_info))
            if not record.device_address:
                raise RuntimeError('native buffer has no device address')
            # Do not expose uninitialized driver allocation bytes to clients.
            ctypes.memset(record.pointer, 0, requirements.size)
            record.allocation_size = requirements.size
            identity = self.next_id
            self.buffers[identity] = record
        except BaseException:
            self.release(record)
            raise
        self.allocated_bytes += record.allocation_size
        self.next_id += 1
        return identity

    def owned(self, identity):
        checked_integer(identity, 1, 128, 'buffer ID')
        if identity not in self.buffers:
            raise ValueError('buffer is not owned by this store')
        return self.buffers[identity]

    def range(self, identity, offset, size):
        record = self.owned(identity)
        checked_integer(offset, 0, record.size, 'buffer offset')
        checked_integer(size, 1, self.maximum_chunk_bytes, 'transfer size')
        if size > record.size - offset:
            raise ValueError('transfer exceeds owned buffer range')
        return record

    def write(self, identity, offset, data):
        if type(data) is not bytes:
            raise ValueError('native buffer upload requires immutable bytes')
        record = self.range(identity, offset, len(data))
        ctypes.memmove(record.pointer.value + offset, data, len(data))

    def read(self, identity, offset, size):
        record = self.range(identity, offset, size)
        return ctypes.string_at(record.pointer.value + offset, size)

    def release(self, record):
        # No submitted command can retain a record across this synchronous API.
        if record.buffer.value:
            self.destroy_buffer(self.device.device, record.buffer, None)
            record.buffer = ctypes.c_uint64()
        if record.pointer.value:
            self.unmap_memory(self.device.device, record.memory)
            record.pointer = ctypes.c_void_p()
        if record.memory.value:
            self.free_memory(self.device.device, record.memory, None)
            record.memory = ctypes.c_uint64()

    def destroy(self, identity):
        record = self.owned(identity)
        self.release(record)
        del self.buffers[identity]
        self.allocated_bytes -= record.allocation_size

    def close(self):
        for identity in list(self.buffers):
            self.destroy(identity)

    def copy(self, source, source_offset, destination, destination_offset, size):
        first = self.range(source, source_offset, size)
        second = self.range(destination, destination_offset, size)
        if source == destination and source_offset < destination_offset + size and destination_offset < source_offset + size:
            raise ValueError('native buffer copy ranges overlap')
        if source_offset % 4 or destination_offset % 4 or size % 4:
            raise ValueError('native Vulkan buffer copies require four-byte aligned ranges')
        self._copy_and_wait(first, source_offset, second, destination_offset, size)

    def _copy_and_wait(self, source, source_offset, destination, destination_offset, size):
        device = self.device
        pointer = ctypes.c_void_p
        handle = ctypes.c_uint64
        pool = handle()
        fence = handle()
        pending = False
        destroy_pool = device.function('vkDestroyCommandPool', None, [pointer, handle, pointer])
        destroy_fence = device.function('vkDestroyFence', None, [pointer, handle, pointer])
        wait_idle = device.function('vkDeviceWaitIdle', ctypes.c_int, [pointer])
        try:
            description = abi.CommandPoolCreateInfo(39, None, 0, device.family)
            create_pool = device.function('vkCreateCommandPool', ctypes.c_int,
                [pointer, ctypes.POINTER(abi.CommandPoolCreateInfo), pointer, ctypes.POINTER(handle)])
            abi.require_success(create_pool(device.device, ctypes.byref(description), None,
                                            ctypes.byref(pool)), 'native transfer pool creation')
            if not pool.value:
                raise RuntimeError('native transfer returned an empty command pool')
            allocation = abi.CommandBufferAllocateInfo(40, None, pool.value, 0, 1)
            commands = pointer()
            allocate = device.function('vkAllocateCommandBuffers', ctypes.c_int,
                [pointer, ctypes.POINTER(abi.CommandBufferAllocateInfo), ctypes.POINTER(pointer)])
            abi.require_success(allocate(device.device, ctypes.byref(allocation), ctypes.byref(commands)),
                                'native transfer command allocation')
            if not commands.value:
                raise RuntimeError('native transfer returned an empty command buffer')
            begin_info = abi.CommandBufferBeginInfo(42, None, 1, None)
            begin = device.function('vkBeginCommandBuffer', ctypes.c_int,
                                     [pointer, ctypes.POINTER(abi.CommandBufferBeginInfo)])
            abi.require_success(begin(commands, ctypes.byref(begin_info)), 'native transfer recording begin')
            barrier = device.function('vkCmdPipelineBarrier', None,
                [pointer, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
                 ctypes.c_uint32, ctypes.POINTER(abi.MemoryBarrier), ctypes.c_uint32, pointer,
                 ctypes.c_uint32, pointer])
            # Include prior device writes as well as CPU uploads. A later copy
            # may read a buffer written by an earlier completed GPU submission.
            before = abi.MemoryBarrier(46, None, 0x14000, 0x1800)
            barrier(commands, 0x14000, 0x1000, 0, 1, ctypes.byref(before), 0, None, 0, None)
            copy = device.function('vkCmdCopyBuffer', None,
                [pointer, handle, handle, ctypes.c_uint32, ctypes.POINTER(abi.BufferCopy)])
            region = abi.BufferCopy(source_offset, destination_offset, size)
            copy(commands, source.buffer, destination.buffer, 1, ctypes.byref(region))
            after = abi.MemoryBarrier(46, None, 0x1000, 0xa000)
            barrier(commands, 0x1000, 0x14000, 0, 1, ctypes.byref(after), 0, None, 0, None)
            end = device.function('vkEndCommandBuffer', ctypes.c_int, [pointer])
            abi.require_success(end(commands), 'native transfer recording end')
            fence_info = abi.FenceCreateInfo(8, None, 0)
            create_fence = device.function('vkCreateFence', ctypes.c_int,
                [pointer, ctypes.POINTER(abi.FenceCreateInfo), pointer, ctypes.POINTER(handle)])
            abi.require_success(create_fence(device.device, ctypes.byref(fence_info), None,
                                             ctypes.byref(fence)), 'native transfer fence creation')
            if not fence.value:
                raise RuntimeError('native transfer returned an empty fence')
            submit_info = abi.SubmitInfo(4, None, 0, None, None, 1, ctypes.addressof(commands), 0, None)
            submit = device.function('vkQueueSubmit', ctypes.c_int,
                [pointer, ctypes.c_uint32, ctypes.POINTER(abi.SubmitInfo), handle])
            pending = True
            abi.require_success(submit(device.queue, 1, ctypes.byref(submit_info), fence), 'native transfer submission')
            wait = device.function('vkWaitForFences', ctypes.c_int,
                [pointer, ctypes.c_uint32, ctypes.POINTER(handle), ctypes.c_uint32, ctypes.c_uint64])
            abi.require_success(wait(device.device, 1, ctypes.byref(fence), 1, 5_000_000_000),
                                'native transfer completion')
            pending = False
        finally:
            if pending and wait_idle(device.device) not in (0, -4):
                os._exit(79)
            if fence.value:
                destroy_fence(device.device, fence, None)
            if pool.value:
                destroy_pool(device.device, pool, None)
