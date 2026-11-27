"""Fixed host-only coherent buffer allocation and two bounded transfer copies."""
import importlib.util
from pathlib import Path


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def probe_buffers():
    device_api = module('buffer_probe_device', 'native_vulkan_device.py')
    buffers_api = module('buffer_probe_store', 'native_vulkan_buffers.py')

    class ProbeDevice(device_api.NativeDevice):
        def create_device(self, physical, family, required12, required13, extensions=()):
            super().create_device(physical, family, required12, required13, extensions)
            self.physical = physical

    device = ProbeDevice()
    store = None
    try:
        store = buffers_api.BufferStore(device)
        large_bytes = 192 * 1024**2
        large = store.create(large_bytes)
        small = store.create(65536)
        if store.read(large, large_bytes - 64, 64) != bytes(64) or store.read(small, 0, 64) != bytes(64):
            raise RuntimeError('new coherent buffers expose nonzero initial bytes')
        pattern = b'\x21\x32\x43\x54' * 8192
        store.write(large, 0, pattern)
        store.copy(large, 0, small, 0, len(pattern))
        if store.read(small, 0, len(pattern)) != pattern:
            raise RuntimeError('cross-buffer GPU copy did not preserve uploaded bytes')
        store.copy(small, 0, large, large_bytes - len(pattern), len(pattern))
        if store.read(large, large_bytes - len(pattern), len(pattern)) != pattern:
            raise RuntimeError('large coherent buffer tail copy did not preserve uploaded bytes')
        addresses_present = bool(store.owned(large).device_address and store.owned(small).device_address)
        store.close()
        if store.buffers or store.allocated_bytes:
            raise RuntimeError('coherent buffer store retained resources after close')
        return {'owned_coherent_buffers_verified': True, 'large_buffer_bytes': large_bytes,
                'upload_and_download_verified': True, 'GPU_copies_verified': 2,
                'copy_bytes_each': len(pattern), 'device_addresses_nonzero': addresses_present,
                'all_buffers_reclaimed': True, 'host_handles_returned': False,
                'guest_cpu_code_loaded': False, 'game_inputs': False}
    finally:
        if store is not None:
            store.close()
        device.close()
