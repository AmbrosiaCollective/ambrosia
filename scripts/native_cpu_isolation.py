"""Audited CPU restriction building blocks for fixed trusted host services."""
import ctypes
import errno
import json
import os
from pathlib import Path
import resource
import socket
import subprocess
import sys
import threading

def restrict(memory_policy=None):
    if sys.platform != 'linux' or os.uname().machine != 'x86_64':
        raise RuntimeError('Sparse memory profile requires native x86-64 Linux')
    resource.setrlimit(resource.RLIMIT_AS, (64 * 1024**3,) * 2)
    resource.setrlimit(resource.RLIMIT_DATA, (8 * 1024**3,) * 2)
    resource.setrlimit(resource.RLIMIT_STACK, (8 * 1024**2,) * 2)
    resource.setrlimit(resource.RLIMIT_MEMLOCK, (0, 0))
    resource.setrlimit(resource.RLIMIT_CPU, (40, 45))
    resource.setrlimit(resource.RLIMIT_FSIZE, (8 * 1024 * 1024,) * 2)
    resource.setrlimit(resource.RLIMIT_NOFILE, (64, 64))
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    resource.setrlimit(resource.RLIMIT_NPROC, (32, 32))
    libc = ctypes.CDLL(None, use_errno=True)
    libc.prctl.argtypes = [ctypes.c_int, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong]
    if libc.prctl(38, 1, 0, 0, 0):  # PR_SET_NO_NEW_PRIVS
        raise RuntimeError('Could not enable no_new_privs')
    lib = ctypes.CDLL('libseccomp.so.2', use_errno=True)
    lib.seccomp_init.argtypes = [ctypes.c_uint32]
    lib.seccomp_init.restype = ctypes.c_void_p
    lib.seccomp_syscall_resolve_name.argtypes = [ctypes.c_char_p]
    lib.seccomp_syscall_resolve_name.restype = ctypes.c_int
    lib.seccomp_rule_add.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int, ctypes.c_uint]
    lib.seccomp_rule_add.restype = ctypes.c_int
    lib.seccomp_load.argtypes = [ctypes.c_void_p]
    lib.seccomp_load.restype = ctypes.c_int
    lib.seccomp_release.argtypes = [ctypes.c_void_p]
    ctx = lib.seccomp_init(0x7FFF0000)  # SCMP_ACT_ALLOW
    if not ctx:
        raise RuntimeError('Could not create seccomp filter')
    blocked = '''socket socketpair connect bind listen accept accept4 sendto sendmsg sendmmsg
    recvfrom recvmsg recvmmsg execve execveat fork vfork ptrace process_vm_readv
    process_vm_writev pidfd_getfd mount umount2 unshare setns bpf perf_event_open keyctl
    add_key request_key kexec_load init_module finit_module delete_module reboot
    io_uring_setup io_uring_enter io_uring_register open_by_handle_at name_to_handle_at
    shmget shmat shmdt shmctl memfd_create userfaultfd mremap remap_file_pages
    mlock mlock2 mlockall pkey_mprotect pkey_alloc pkey_free'''.split()
    try:
        for name in blocked:
            num = lib.seccomp_syscall_resolve_name(name.encode())
            if num >= 0 and lib.seccomp_rule_add(ctx, 0x00050000 | errno.EPERM, num, 0):
                raise RuntimeError('Could not restrict syscall: ' + name)
        restrict_thread_calls(lib, ctx)
        if memory_policy is None:
            memory_policy = restrict_memory_calls
        memory_policy(lib, ctx)
        if lib.seccomp_load(ctx):
            raise RuntimeError('Could not activate seccomp filter')
    finally:
        lib.seccomp_release(ctx)
    return blocked

class ArgumentComparison(ctypes.Structure):
    _fields_ = [('argument', ctypes.c_uint), ('operation', ctypes.c_uint),
                ('mask', ctypes.c_uint64), ('value', ctypes.c_uint64)]

def restrict_thread_calls(lib, context):
    lib.seccomp_rule_add_array.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                         ctypes.c_int, ctypes.c_uint,
                                         ctypes.POINTER(ArgumentComparison)]
    lib.seccomp_rule_add_array.restype = ctypes.c_int
    number = lib.seccomp_syscall_resolve_name(b'clone')
    # Exact glibc pthread clone flags: VM, FS, FILES, SIGHAND, THREAD,
    # SYSVSEM, SETTLS, PARENT_SETTID and CHILD_CLEARTID. No child process,
    # exit signal, namespace, vfork, ptrace or alternate clone mode is admitted.
    comparison = ArgumentComparison(0, 1, 0x3d0f00, 0)  # SCMP_CMP_NE.
    if number < 0 or lib.seccomp_rule_add_array(context, 0x50000 | errno.EPERM,
                                               number, 1, ctypes.byref(comparison)):
        raise RuntimeError('Could not restrict pthread clone flags')
    number = lib.seccomp_syscall_resolve_name(b'clone3')
    # Force libc's known clone fallback; never inspect an untrusted pointed-to
    # clone_args structure with seccomp's scalar argument comparisons.
    if number < 0 or lib.seccomp_rule_add(context, 0x50000 | errno.ENOSYS, number, 0):
        raise RuntimeError('Could not reject clone3')

def restrict_memory_calls(lib, context):
    lib.seccomp_rule_add_array.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                         ctypes.c_int, ctypes.c_uint,
                                         ctypes.POINTER(ArgumentComparison)]
    lib.seccomp_rule_add_array.restype = ctypes.c_int

    def deny_mask(name, argument, mask, value):
        number = lib.seccomp_syscall_resolve_name(name.encode())
        if number < 0:
            raise RuntimeError('Required native memory syscall is unavailable: ' + name)
        comparison = ArgumentComparison(argument, 7, mask, value)  # SCMP_CMP_MASKED_EQ.
        if lib.seccomp_rule_add_array(context, 0x50000 | errno.EPERM, number, 1,
                                      ctypes.byref(comparison)):
            raise RuntimeError('Could not restrict memory syscall: ' + name)

    # Pthread MAP_STACK is allowed; shared/growsdown/hugepage mappings, eager
    # population and locked memory remain blocked. GPU mappings remain absent.
    for flag in (0x1, 0x100, 0x2000, 0x8000, 0x40000):
        deny_mask('mmap', 3, flag, flag)
    for name in ('mmap', 'mprotect'):
        deny_mask(name, 2, 0x6, 0x6)  # No direct writable/executable mappings.
    for flag in (0x1000000, 0x2000000):
        deny_mask('mprotect', 2, flag, flag)
    # Only discard advice is needed by the reviewed host memory adapter.
    number = lib.seccomp_syscall_resolve_name(b'madvise')
    comparison = ArgumentComparison(2, 1, 4, 0)  # SCMP_CMP_NE, MADV_DONTNEED=4.
    if number < 0 or lib.seccomp_rule_add_array(context, 0x50000 | errno.EPERM,
                                               number, 1, ctypes.byref(comparison)):
        raise RuntimeError('Could not restrict memory advice')

def memory_self_check():
    libc = ctypes.CDLL(None, use_errno=True)
    libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
                         ctypes.c_int, ctypes.c_int, ctypes.c_long]
    libc.mmap.restype = ctypes.c_void_p
    libc.munmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    libc.mprotect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
    failed = ctypes.c_void_p(-1).value
    checks = {
        'address_limit_64gib': resource.getrlimit(resource.RLIMIT_AS) == (64 * 1024**3,) * 2,
        'data_limit_8gib': resource.getrlimit(resource.RLIMIT_DATA) == (8 * 1024**3,) * 2,
        'locked_memory_disabled': resource.getrlimit(resource.RLIMIT_MEMLOCK) == (0, 0),
    }
    # These probes reserve virtual space only. They never touch/fill a huge range.
    size = 9 * 1024**3
    ctypes.set_errno(0)
    address = libc.mmap(None, size, 3, 0x22, -1, 0)
    checks['oversized_private_write_denied'] = address == failed and ctypes.get_errno() == errno.ENOMEM
    if address != failed:
        libc.munmap(address, size)
    address = libc.mmap(None, size, 0, 0x22, -1, 0)
    checks['sparse_noaccess_allowed'] = address != failed
    if address != failed:
        ctypes.set_errno(0)
        result = libc.mprotect(address, size, 3)
        checks['oversized_write_upgrade_denied'] = result != 0 and ctypes.get_errno() == errno.ENOMEM
        libc.munmap(address, size)
    else:
        checks['oversized_write_upgrade_denied'] = False
    for name, protection, flags in (('shared', 3, 0x21), ('write_execute', 7, 0x22),
                                     ('populate', 1, 0x8022), ('stack', 3, 0x122)):
        ctypes.set_errno(0)
        address = libc.mmap(None, 4096, protection, flags, -1, 0)
        checks[name + '_mapping_denied'] = address == failed and ctypes.get_errno() == errno.EPERM
        if address != failed:
            libc.munmap(address, 4096)
    if not all(checks.values()):
        raise RuntimeError('Sparse memory self-check failed: ' + json.dumps(checks))
    return checks

def self_check():
    checks = {}
    for path in ('/home', '/root', '/run', '/etc', '/host'):
        checks['hidden:' + path] = not Path(path).exists()
    checks['clean_environment'] = set(os.environ) <= {'PATH', 'HOME', 'TMPDIR', 'LANG', 'LC_ALL', 'PYTHONHASHSEED', 'PWD'}
    checks['private_pid_namespace'] = os.getpid() <= 4
    try:
        sock = socket.socket()
    except PermissionError:
        checks['socket_denied'] = True
    else:
        sock.close()
        checks['socket_denied'] = False
    try:
        subprocess.run(['/usr/bin/true'], check=True)
    except PermissionError:
        checks['process_execution_denied'] = True
    else:
        checks['process_execution_denied'] = False
    try:
        Path('/src/.sandbox-write-probe').write_text('probe', encoding='utf-8', newline='\n')
    except OSError as e:
        checks['source_read_only'] = e.errno == errno.EROFS
    else:
        checks['source_read_only'] = False
    dest = Path('/work/tmp/self-check')
    dest.write_text('synthetic', encoding='utf-8', newline='\n')
    checks['scratch_writable'] = dest.read_text() == 'synthetic'
    dest.unlink()
    if not all(checks.values()):
        raise RuntimeError('Sandbox self-check failed: ' + json.dumps({'checks': checks, 'environment_keys': sorted(os.environ)}))
    return checks

def thread_self_check():
    checks = {'hard_task_limit_32': resource.getrlimit(resource.RLIMIT_NPROC) == (32, 32)}
    status = Path('/proc/self/status').read_text()
    capabilities = next(line.split()[1] for line in status.splitlines() if line.startswith('CapEff:'))
    checks['all_capabilities_dropped'] = int(capabilities, 16) == 0
    libc = ctypes.CDLL(None, use_errno=True)
    libc.syscall.restype = ctypes.c_long
    ctypes.set_errno(0)
    result = libc.syscall(56, 17, 0, 0, 0, 0)  # clone(SIGCHLD), not a pthread.
    checks['process_clone_denied'] = result == -1 and ctypes.get_errno() == errno.EPERM
    if result == 0:
        os._exit(98)
    if result > 0:
        os.waitpid(result, 0)
        raise RuntimeError('Unsafe process clone was admitted')
    ctypes.set_errno(0)
    result = libc.syscall(435, 0, 0)  # clone3 is always unavailable.
    checks['clone3_unavailable'] = result == -1 and ctypes.get_errno() == errno.ENOSYS
    release = threading.Event()
    workers = []
    denied = False
    try:
        # Bounded failure probe. At most 40 idle trusted threads are attempted;
        # untrusted entrypoints are never reached unless the hard cap is proven.
        for _ in range(40):
            worker = threading.Thread(target=release.wait)
            try:
                worker.start()
            except RuntimeError:
                denied = True
                break
            workers.append(worker)
        checks['pthread_creation_allowed'] = bool(workers)
        checks['kernel_task_cap_enforced'] = denied and 1 <= len(workers) <= 31
    finally:
        release.set()
        for worker in workers:
            worker.join(timeout=5)
        checks['probe_threads_joined'] = all(not worker.is_alive() for worker in workers)
    status = Path('/proc/self/status').read_text()
    threads = int(next(line.split()[1] for line in status.splitlines() if line.startswith('Threads:')))
    checks['one_thread_after_self_check'] = threads == 1
    if not all(checks.values()):
        raise RuntimeError('Thread isolation self-check failed: ' + json.dumps(checks))
    return checks, len(workers)
