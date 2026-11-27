"""Trusted fixed host-memory import/copy bootstrap; no CPU guest entrypoint."""
import ctypes
import errno
import importlib.util
import json
import mmap
import os
from pathlib import Path
import stat
import sys


def module_from_file(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


base = module_from_file('reviewed_cpu_controls', '/cpu_controls.py')


def graphics_memory_controls(library, context):
    library.seccomp_rule_add_array.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
        ctypes.c_int, ctypes.c_uint, ctypes.POINTER(base.ArgumentComparison)]
    library.seccomp_rule_add_array.restype = ctypes.c_int

    def deny(name, argument, mask, value):
        number = library.seccomp_syscall_resolve_name(name.encode('ascii'))
        comparison = base.ArgumentComparison(argument, 7, mask, value)
        if number < 0 or library.seccomp_rule_add_array(context, 0x50000 | errno.EPERM,
            number, 1, ctypes.byref(comparison)):
            raise RuntimeError('cannot restrict graphics-host memory operation')

    # Only file-backed sharing is added for the explicitly mounted GPU devices
    # and private scratch. Anonymous shared memory and every WX mapping stay denied.
    deny('mmap', 3, 0x21, 0x21)
    for flag in (0x100, 0x2000, 0x8000, 0x40000):
        deny('mmap', 3, flag, flag)
    for name in ('mmap', 'mprotect'):
        deny(name, 2, 0x6, 0x6)
    for flag in (0x1000000, 0x2000000):
        deny('mprotect', 2, flag, flag)
    number = library.seccomp_syscall_resolve_name(b'madvise')
    comparison = base.ArgumentComparison(2, 1, 4, 0)
    if number < 0 or library.seccomp_rule_add_array(context, 0x50000 | errno.EPERM,
        number, 1, ctypes.byref(comparison)):
        raise RuntimeError('cannot restrict graphics-host memory advice')


def metadata_checks():
    profile = json.loads(Path('/graphics-profile.json').read_text())
    expected = {item['path']: item['rdev'] for item in profile['devices']}
    checks = {
        'bounded_gpu_device_count': 1 <= len(expected) <= 8,
        'cpu_topology_present': Path('/sys/devices/system/cpu/online').is_file(),
        'numa_topology_present': Path('/sys/devices/system/node/online').is_file(),
        'other_sysfs_hidden': not Path('/sys/class').exists(),
        'metadata_writable_alias_hidden': not Path('/work/topology').exists(),
        'game_inputs_not_mounted': not Path('/src/master').exists(),
        'kernel_module_state_snapshot': Path('/sys/module/nvidia/initstate').read_bytes().strip() == b'live',
    }
    actual = {str(path) for path in Path('/dev').glob('nvidia*')}
    actual.update(str(path) for path in Path('/dev/dri').glob('*'))
    checks['only_allowlisted_gpu_nodes'] = actual == set(expected)
    checks['gpu_nodes_are_expected_character_devices'] = all(
        stat.S_ISCHR(Path(path).lstat().st_mode) and Path(path).stat().st_rdev == device
        for path, device in expected.items())
    checks['gpu_node_access'] = all(os.access(path, os.R_OK | os.W_OK) for path in expected)
    for name, path in (('topology_read_only', '/sys/devices/system/cpu/online'),
                       ('gpu_metadata_read_only', '/graphics-profile.json'),
                       ('kernel_module_state_read_only', '/sys/module/nvidia/initstate')):
        try:
            Path(path).write_text('probe')
        except OSError as error:
            checks[name] = error.errno == errno.EROFS
        else:
            checks[name] = False
    # A regular file in private scratch may be shared; no other writable host
    # filesystem is mounted. This is not permission to create shared memfds/IPC.
    with Path('/work/tmp/shared-probe').open('w+b') as stream:
        stream.truncate(4096)
        view = mmap.mmap(stream.fileno(), 4096)
        view[0] = 17
        view.close()
        checks['private_file_backed_sharing_allowed'] = stream.read(1) == b'\x11'
    Path('/work/tmp/shared-probe').unlink()
    if not all(checks.values()):
        raise RuntimeError('graphics-host metadata checks failed: ' + json.dumps(checks))
    return checks


def main():
    if sys.argv[1:] not in ([], ['--self-check']):
        raise RuntimeError('host query accepts no guest/test entrypoint')
    blocked = base.restrict(graphics_memory_controls)
    checks = base.self_check()
    memory_checks = base.memory_self_check()
    memory_checks['anonymous_shared_mapping_denied'] = memory_checks.pop('shared_mapping_denied')
    checks.update(memory_checks)
    checks.update(metadata_checks())
    thread_checks, workers = base.thread_self_check()
    checks.update(thread_checks)
    print('HOST_GRAPHICS_ISOLATION ' + json.dumps({
        'checks': checks, 'blocked_syscalls': blocked, 'thread_cap_probe_workers': workers,
        'guest_cpu_code_loaded': False, 'gpu_work_submitted': False}), flush=True)
    if sys.argv[1:] == ['--self-check']:
        return
    api = module_from_file('trusted_native_host_memory', '/src/native_vulkan_host_memory.py')
    print(json.dumps(api.probe_host_import()), flush=True)


if __name__ == '__main__':
    main()
