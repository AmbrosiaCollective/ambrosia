#!/usr/bin/python3
"""Compile native sources with one read-only canonical portable IR header.

Unlike run_isolated.py, compilation must allow child processes. Sources remain
read-only; host home, credentials, game data, network and GPU devices are absent.
Never run this launcher with elevated tool permissions. It does not run output.
"""
import argparse
import ctypes
import errno
import hashlib
import json
import os
from pathlib import Path
import resource
import signal
import stat
import subprocess
import tempfile
import time


def filter_file(path):
    lib = ctypes.CDLL('libseccomp.so.2', use_errno=True)
    lib.seccomp_init.argtypes = [ctypes.c_uint32]
    lib.seccomp_init.restype = ctypes.c_void_p
    lib.seccomp_syscall_resolve_name.argtypes = [ctypes.c_char_p]
    lib.seccomp_syscall_resolve_name.restype = ctypes.c_int
    lib.seccomp_rule_add.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int, ctypes.c_uint]
    lib.seccomp_rule_add.restype = ctypes.c_int
    lib.seccomp_export_bpf.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.seccomp_export_bpf.restype = ctypes.c_int
    lib.seccomp_release.argtypes = [ctypes.c_void_p]
    context = lib.seccomp_init(0x7fff0000)
    if not context:
        raise RuntimeError('seccomp initialization failed')
    try:
        for name in '''socket socketpair connect bind listen accept accept4 sendto sendmsg sendmmsg
            recvfrom recvmsg recvmmsg ptrace process_vm_readv process_vm_writev pidfd_getfd
            mount umount2 unshare setns bpf perf_event_open keyctl add_key request_key
            kexec_load init_module finit_module delete_module reboot io_uring_setup
            io_uring_enter io_uring_register open_by_handle_at name_to_handle_at'''.split():
            number = lib.seccomp_syscall_resolve_name(name.encode())
            if number >= 0 and lib.seccomp_rule_add(context, 0x50000 | errno.EPERM, number, 0):
                raise RuntimeError('seccomp rule failed')
        with path.open('wb') as stream:
            if lib.seccomp_export_bpf(context, stream.fileno()):
                raise RuntimeError('seccomp export failed')
    finally:
        lib.seccomp_release(context)


def limits(file_mib):
    resource.setrlimit(resource.RLIMIT_AS, (2 * 1024**3,) * 2)
    resource.setrlimit(resource.RLIMIT_CPU, (45, 50))
    resource.setrlimit(resource.RLIMIT_FSIZE, (file_mib * 1024**2,) * 2)
    resource.setrlimit(resource.RLIMIT_NOFILE, (128, 128))
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compiler', choices=('gcc', 'clang'), default='gcc')
    parser.add_argument('--cmake', action='store_true', help='verify the standalone offline CMake build instead')
    parser.add_argument('--shared-only', action='store_true',
                        help='build only the shared test/probe library for an iteration')
    parser.add_argument('--syntax-unit', action='append', default=[],
                        help='syntax-check only these reviewed units; repeat for changed files and tests.cpp')
    parser.add_argument('--self-check', action='store_true', help='check this profile without compiling')
    parser.add_argument('--probe-portable', action='store_true', help='syntax-check the fixed portable IR probe')
    args = parser.parse_args()
    if args.cmake and args.shared_only:
        parser.error('--cmake and --shared-only select different build modes')
    if args.syntax_unit and (args.cmake or args.shared_only):
        parser.error('--syntax-unit selects its own source-only mode')
    if args.self_check or args.probe_portable:
        if args.cmake or args.shared_only or args.syntax_unit or (args.self_check and args.probe_portable):
            parser.error('self-check and portable probe select exclusive build modes')
    source = args.source.resolve(strict=True)
    output = args.output.resolve()
    if output.exists():
        parser.error('choose a new output directory; existing evidence is preserved')
    units = ('host.cpp', 'host_shared_backing.cpp', 'host_presentation.cpp', 'host_graphics.cpp', 'host_topology.cpp', 'host_priority.cpp', 'host_affinity.cpp', 'host_readonly_files.cpp', 'host_local_storage.cpp', 'processor_information.cpp', 'sysinfo_services.cpp', 'exception_services.cpp', 'math_services.cpp', 'string_services.cpp', 'text_conversion.cpp', 'nls_services.cpp', 'readonly_files.cpp', 'file_services.cpp', 'format_services.cpp', 'scan_services.cpp', 'sort_services.cpp', 'random_services.cpp', 'host_tls.cpp', 'guest_thread.cpp', 'pe_image.cpp', 'pe_exports.cpp',
             'module_execution.cpp', 'module_graph.cpp', 'package_modules.cpp', 'native_command.cpp',
             'import_registry.cpp', 'service_module_image.cpp', 'native_modules.cpp', 'library_references.cpp', 'gdk_bootstrap.cpp', 'gdk_bootstrap_services.cpp', 'runtime_interface.cpp', 'gdk_threading.cpp', 'gdk_display.cpp', 'gdk_networking.cpp', 'gdk_users.cpp', 'gdk_packages.cpp', 'persistent_storage.cpp', 'package_identity.cpp', 'package_installation.cpp', 'game_input.cpp', 'storage_factory.cpp', 'winsock_lifecycle.cpp', 'native_threads.cpp', 'thread_services.cpp', 'thread_affinity.cpp', 'thread_affinity_services.cpp', 'invalid_parameters.cpp', 'worker_pool.cpp', 'task_queues.cpp', 'app_lifecycle.cpp', 'app_lifecycle_services.cpp', 'event_signal.cpp', 'semaphore_signal.cpp', 'kernel_objects.cpp', 'kernel_services.cpp', 'registry_keys.cpp', 'registry_services.cpp', 'graphics_memory_policy.cpp', 'graphics_services.cpp', 'graphics_devices.cpp', 'xmem_services.cpp', 'memory_services.cpp', 'physical_pages.cpp', 'window_classes.cpp', 'window_class_services.cpp', 'native_windows.cpp', 'window_services.cpp', 'service_runtime.cpp', 'unwind_table.cpp',
             'unwind_program.cpp', 'unwind_epilogue.cpp', 'startup_trace.cpp', 'host_fault_trace.cpp',
             'host_time.cpp', 'host_random.cpp', 'clock_services.cpp', 'critical_sections.cpp', 'critical_services.cpp', 'slim_locks.cpp', 'slim_lock_services.cpp',
             'fls_slots.cpp', 'fls_services.cpp', 'host_stack.cpp', 'crt_lifecycle.cpp', 'crt_services.cpp', 'crt_exit.cpp', 'crt_exit_services.cpp', 'crt_streams.cpp', 'crt_startup.cpp', 'crt_startup_services.cpp', 'crt_heap.cpp', 'crt_heap_services.cpp', 'slist_services.cpp', 'loader_services.cpp', 'locale_services.cpp', 'user_locale.cpp', 'crt_locales.cpp', 'pointer_services.cpp')
    if any(name not in (*units, 'main.cpp', 'tests.cpp') for name in args.syntax_unit):
        parser.error('syntax-unit must name a reviewed translation unit')
    if len(args.syntax_unit) != len(set(args.syntax_unit)):
        parser.error('choose each syntax unit once')
    for name in (*units, 'main.cpp', 'tests.cpp'):
        if not (source / name).is_file() or (source / name).is_symlink():
            parser.error('missing reviewed translation unit: ' + name)
    compiler = '/usr/bin/g++' if args.compiler == 'gcc' else '/usr/bin/clang++-18'
    if not Path(compiler).is_file():
        parser.error('installed compiler unavailable')
    portable_header = source.parent / 'src' / 'command_list.h'
    if not portable_header.is_file() or portable_header.is_symlink() or portable_header.stat().st_size > 1024**2:
        parser.error('a direct, bounded canonical command_list.h is required beside the native directory')
    portable_bytes = portable_header.read_bytes()
    portable_bytes.decode('utf-8')
    portable_hash = hashlib.sha256(portable_bytes).hexdigest()
    source_hashes_before = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                            for path in sorted(source.iterdir()) if path.is_file()}
    guard = Path(__file__).with_name('isolation_native_compiler_guard.py')
    if not guard.is_file() or guard.is_symlink():
        parser.error('fixed compiler guard unavailable')
    guard_hash = hashlib.sha256(guard.read_bytes()).hexdigest()
    host_pid_namespace = os.stat('/proc/self/ns/pid').st_ino
    file_mib = 256 if args.cmake else 64
    output.mkdir(parents=True)
    common = [compiler, '-std=c++20', '-pthread', '-O1', '-g0', '-Wall', '-Wextra', '-Werror',
              '-Wpedantic', '-fno-omit-frame-pointer', '-iquote', '/portable', *('/src/' + name for name in units), '-licuuc', '-lexpat']
    commands = [common + ['-fPIC', '-shared', '/src/tests.cpp', '-o', '/work/native-tests.so'],
                common + ['/src/main.cpp', '-o', '/work/ambrosia-host']]
    artifact_paths = [('native-tests.so', 'native-tests.so'), ('ambrosia-host', 'ambrosia-host')]
    if args.shared_only:
        commands = commands[:1]
        artifact_paths = artifact_paths[:1]
    if args.cmake:
        if not (source / 'CMakeLists.txt').is_file():
            parser.error('reviewed CMake configuration missing')
        commands = [['/usr/bin/cmake', '-S', '/src', '-B', '/work/cmake',
                     '-DCMAKE_CXX_COMPILER=' + compiler, '-DBUILD_TESTING=ON',
                     '-DCMAKE_BUILD_TYPE=Debug', '-DAMBROSIA_PORTABLE_INCLUDE_DIR=/portable'],
                    ['/usr/bin/cmake', '--build', '/work/cmake', '--parallel', '2',
                     '--target', 'ambrosia-host', 'ambrosia_native_tests']]
        artifact_paths = [('cmake/ambrosia-host', 'ambrosia-host'),
                          ('cmake/ambrosia_native_tests', 'ambrosia_native_tests')]
    if args.syntax_unit:
        commands = [[compiler, '-std=c++20', '-pthread', '-O1', '-g0', '-Wall', '-Wextra',
                     '-Werror', '-Wpedantic', '-fno-omit-frame-pointer', '-fPIC', '-fsyntax-only', '-iquote', '/portable',
                     *('/src/' + name for name in args.syntax_unit)]]
        artifact_paths = []
    if args.self_check:
        commands = [[]]
        artifact_paths = []
    if args.probe_portable:
        commands = [[compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wpedantic',
                     '-iquote', '/portable', '-fsyntax-only', '/work/portable-probe.cpp']]
        artifact_paths = []
    results = []
    with tempfile.TemporaryDirectory(prefix='ambrosia-compiler-', dir='/tmp') as temp:
        scratch = Path(temp)
        (scratch / 'tmp').mkdir()
        (scratch / 'home').mkdir()
        (scratch / 'portable-root').mkdir()
        (scratch / 'portable-root/command_list.h').write_bytes(b'')
        (scratch / 'portable-probe.cpp').write_text(
            '#include "command_list.h"\n'
            'void verify_shared_recording_compiles() {\n'
            '    ambrosia::CommandList recording;\n'
            '    (void)recording.close();\n'
            '    (void)recording.reset();\n'
            '}\n', encoding='utf-8', newline='\n')
        filter_file(scratch / 'filter.bpf')
        for index, command in enumerate(commands):
            with (scratch / 'filter.bpf').open('rb') as seccomp:
                # No network namespace setup: bubblewrap needs a netlink socket
                # to initialize loopback, which our deny-all-sockets filter
                # deliberately forbids. The filter blocks all networking, as in
                # the existing Python launcher; other namespaces stay private.
                wrapped = ['/usr/bin/bwrap', '--unshare-user', '--unshare-pid',
                    '--unshare-ipc', '--unshare-uts', '--die-with-parent', '--new-session',
                    '--cap-drop', 'ALL', '--ro-bind', '/usr', '/usr', '--ro-bind', '/lib', '/lib',
                    '--ro-bind', '/lib64', '/lib64', '--symlink', 'usr/bin', '/bin',
                    '--proc', '/proc', '--dev', '/dev',
                    '--tmpfs', '/tmp', '--ro-bind', str(source), '/src',
                    '--ro-bind', str(scratch / 'portable-root'), '/portable',
                    '--ro-bind', str(portable_header), '/portable/command_list.h',
                    '--ro-bind', str(guard), '/compiler-guard.py',
                    '--bind', str(scratch), '/work', '--clearenv',
                    '--setenv', 'PATH', '/usr/bin', '--setenv', 'HOME', '/work/home',
                    '--setenv', 'TMPDIR', '/work/tmp', '--setenv', 'LANG', 'C.UTF-8',
                    '--seccomp', str(seccomp.fileno()), '--chdir', '/work', '--',
                    '/usr/bin/python3', '-I', '-S', '-B', '/compiler-guard.py',
                    '--header-sha256', portable_hash, '--host-pid-namespace', str(host_pid_namespace),
                    '--file-mib', str(file_mib), '--', *command]
                start = time.monotonic()
                with (output / f'build-{index}.log').open('wb') as log:
                    child = subprocess.Popen(wrapped, stdin=subprocess.DEVNULL, stdout=log,
                        stderr=subprocess.STDOUT, env={'PATH': '/usr/bin'}, close_fds=True,
                        pass_fds=(seccomp.fileno(),), start_new_session=True, preexec_fn=lambda: limits(file_mib))
                    try:
                        code = child.wait(timeout=180)
                    except subprocess.TimeoutExpired:
                        os.killpg(child.pid, signal.SIGKILL)
                        child.wait()
                        code = 124
                results.append({'command': command, 'returncode': code,
                                'seconds': round(time.monotonic() - start, 3)})
                if code:
                    break
        artifacts = []
        source_hashes = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                         for path in sorted(source.iterdir()) if path.is_file()}
        inputs_unchanged = source_hashes == source_hashes_before and hashlib.sha256(
            portable_header.read_bytes()).hexdigest() == portable_hash and hashlib.sha256(
            guard.read_bytes()).hexdigest() == guard_hash
        if inputs_unchanged and all(r['returncode'] == 0 for r in results) and len(results) == len(commands):
            for source_name, name in artifact_paths:
                artifact = scratch / source_name
                descriptor = os.open(artifact, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
                try:
                    information = os.fstat(descriptor)
                    if (not stat.S_ISREG(information.st_mode) or information.st_nlink != 1 or
                            information.st_size > 64 * 1024**2):
                        raise RuntimeError('unexpected build artifact type or size')
                    with os.fdopen(descriptor, 'rb', closefd=False) as stream:
                        data = stream.read(64 * 1024**2 + 1)
                finally:
                    os.close(descriptor)
                if len(data) != information.st_size or not data.startswith(b'\x7fELF\x02'):
                    raise RuntimeError('unexpected build artifact contents')
                (output / name).write_bytes(data)
                artifacts.append({'name': name, 'bytes': len(data),
                                  'sha256': hashlib.sha256(data).hexdigest()})
    record = {'compiler': args.compiler, 'cmake': args.cmake, 'shared_only': args.shared_only,
              'syntax_units': args.syntax_unit,
              'results': results, 'artifacts': artifacts,
              'source_sha256': source_hashes,
              'portable_source_sha256': {'command_list.h': portable_hash},
              'compiler_guard_sha256': guard_hash,
              'inputs_unchanged': inputs_unchanged,
              'self_check_only': args.self_check, 'portable_probe_only': args.probe_portable,
              'scratch_file_mib': file_mib, 'returned_artifact_mib': 64,
              'outputs_executed': False, 'source_read_only': True,
              'wall_seconds_per_command': 180,
              'network_home_credentials_gpu_game_data': 'not exposed',
              'child_processes': 'allowed for compiler; this is not the test profile'}
    (output / 'build.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps(record, indent=2))
    succeeded = inputs_unchanged and len(results) == len(commands) and all(result['returncode'] == 0 for result in results)
    return 0 if succeeded and len(artifacts) == len(artifact_paths) else 1


if __name__ == '__main__':
    raise SystemExit(main())
