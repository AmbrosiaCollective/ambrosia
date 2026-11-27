#!/usr/bin/python3
"""Fixed trusted synthetic host-memory import/copy service. No CPU guest payloads.

Only this reviewed host integration may access the host GPU. It imports bounded
synthetic scratch memory into one Vulkan buffer and executes
one fixed nonoverlapping transfer copy per backing (at most two). It reads no game inputs or guest code.
Game execution remains in the default outer sandbox with its existing profiles.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import resource
import signal
import stat
import subprocess
import tempfile
import time


def resource_helpers():
    path = Path(__file__).with_name('native_isolation_resources.py')
    spec = importlib.util.spec_from_file_location('reviewed_host_resources', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def device_metadata():
    candidates = [Path('/dev/nvidiactl'), Path('/dev/nvidia0'), Path('/dev/nvidia-uvm'),
                  Path('/dev/nvidia-uvm-tools')]
    render_nodes = sorted(Path('/dev/dri').glob('renderD*'))
    candidates.extend(render_nodes[:1])
    entries = []
    for path in candidates:
        if not path.exists():
            continue
        information = path.lstat()
        if not stat.S_ISCHR(information.st_mode):
            raise RuntimeError('graphics-host device must be a direct character node')
        if not os.access(path, os.R_OK | os.W_OK):
            if path.name in ('nvidia0', 'nvidiactl'):
                raise RuntimeError('required graphics-host device access unavailable')
            continue
        entries.append({'path': str(path), 'rdev': information.st_rdev})
    if not entries or not any(item['path'] == '/dev/nvidia0' for item in entries):
        raise RuntimeError('fixed NVIDIA capability query requires the installed host device')
    return {'devices': entries, 'scope': 'fixed synthetic host import/copy service; no CPU guest entrypoint'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--label', required=True)
    parser.add_argument('--self-check', action='store_true')
    parser.add_argument('--trace-syscalls', action='store_true',
                        help='trace only this fixed service into private evidence')
    arguments = parser.parse_args()
    if not arguments.label.replace('-', '').isalnum():
        parser.error('label must be alphanumeric with hyphens')
    scripts = Path(__file__).resolve().parent
    workspace = scripts.parent
    evidence = arguments.evidence.resolve()
    if not evidence.is_relative_to(workspace / 'reports'):
        parser.error('host-query evidence must remain under workspace reports')
    evidence.mkdir(parents=True, exist_ok=True)
    for suffix in ('.stdout.log', '.stderr.log', '.run.json', '.syscalls.log'):
        if (evidence / (arguments.label + suffix)).exists():
            parser.error('refusing to overwrite host-query evidence')
    profile = device_metadata()
    selected_cpus = sorted(os.sched_getaffinity(0))[:8]
    if not selected_cpus:
        raise RuntimeError('graphics-host CPU allowance is empty')
    os.sched_setaffinity(0, selected_cpus)
    helpers = resource_helpers()
    sources = [scripts / name for name in ('native_vulkan_query.py',
        'isolation_graphics_import_bootstrap.py', 'native_cpu_isolation.py',
        'native_isolation_resources.py', 'native_vulkan_device.py',
        'native_vulkan_host_memory.py')]
    hashes = {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in sources}
    with tempfile.TemporaryDirectory(prefix='ambrosia-graphics-host-', dir='/tmp') as directory:
        root = Path(directory)
        work = root / 'scratch'
        work.mkdir()
        for name in ('home', 'tmp', 'no-layers'):
            (work / name).mkdir()
        topology = root / 'topology'
        topology_record = helpers.snapshot_topology(topology)
        source = root / 'source'
        source.mkdir()
        for payload in (sources[0], sources[4], sources[5]):
            (source / payload.name).write_bytes(payload.read_bytes())
        metadata = root / 'graphics-profile.json'
        metadata.write_text(json.dumps(profile), encoding='utf-8', newline='\n')
        # The traced ICD checks this public state before using existing device
        # nodes. Copy only its bounded text, never mount live GPU/module sysfs.
        module_state = Path('/sys/module/nvidia/initstate').read_bytes()
        if len(module_state) > 32 or module_state.strip() != b'live':
            raise RuntimeError('required graphics driver module is not live')
        module_copy = root / 'module-state'
        module_copy.write_bytes(module_state)
        module_copy.chmod(0o444)
        command = ['/usr/bin/bwrap', '--unshare-user', '--unshare-pid', '--unshare-ipc', '--unshare-uts',
            '--die-with-parent', '--new-session', '--cap-drop', 'ALL', '--ro-bind', '/usr', '/usr',
            '--ro-bind', '/lib', '/lib', '--ro-bind', '/lib64', '/lib64', '--proc', '/proc',
            '--dev', '/dev', '--tmpfs', '/tmp', '--ro-bind', str(topology), '/sys/devices/system',
            '--ro-bind', str(source), '/src', '--bind', str(work), '/work',
            '--ro-bind', str(metadata), '/graphics-profile.json',
            '--ro-bind', str(module_copy), '/sys/module/nvidia/initstate',
            '--ro-bind', str(sources[1]), '/bootstrap.py',
            '--ro-bind', str(sources[2]), '/cpu_controls.py', '--clearenv']
        if any(item['path'].startswith('/dev/dri/') for item in profile['devices']):
            command += ['--dir', '/dev/dri']
        for device in profile['devices']:
            command += ['--dev-bind', device['path'], device['path']]
        for key, value in {'PATH': '/usr/bin', 'HOME': '/work/home', 'TMPDIR': '/work/tmp',
                           'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8', 'PYTHONHASHSEED': '0'}.items():
            command += ['--setenv', key, value]
        command += ['--chdir', '/src', '--', '/usr/bin/python3', '-I', '-S', '-B', '/bootstrap.py']
        if arguments.self_check:
            command.append('--self-check')
        if arguments.trace_syscalls:
            if not Path('/usr/bin/strace').is_file():
                raise RuntimeError('installed trusted syscall tracer unavailable')
            command = ['/usr/bin/strace', '-f', '-qq', '-s', '64',
                '-e', 'trace=%file,%memory,ioctl,write', '-o',
                str(evidence / (arguments.label + '.syscalls.log')), *command]
        started = time.monotonic()
        timed_out = exceeded = monitor_failed = False
        peak = 0
        with (evidence / (arguments.label + '.stdout.log')).open('wb') as stdout, (
                evidence / (arguments.label + '.stderr.log')).open('wb') as stderr:
            def trace_limits():
                resource.setrlimit(resource.RLIMIT_FSIZE, (8 * 1024**2,) * 2)
                resource.setrlimit(resource.RLIMIT_CPU, (40, 45))

            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
                env={'PATH': '/usr/bin'}, close_fds=True, start_new_session=True,
                preexec_fn=trace_limits if arguments.trace_syscalls else None)
            try:
                while process.poll() is None:
                    peak = max(peak, helpers.process_tree_rss(process.pid))
                    timed_out = time.monotonic() - started >= 30
                    exceeded = peak > 8 * 1024**3
                    if timed_out or exceeded:
                        break
                    try:
                        process.wait(timeout=0.01)
                    except subprocess.TimeoutExpired:
                        pass
            except (OSError, ValueError, RuntimeError):
                monitor_failed = True
            finally:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGKILL)
            code = process.wait()
    record = {'label': arguments.label, 'returncode': code, 'timed_out': timed_out,
        'rss_watchdog_exceeded': exceeded, 'rss_monitor_failed': monitor_failed,
        'sampled_peak_process_tree_rss_bytes': peak, 'wall_seconds': round(time.monotonic() - started, 3),
        'source_hashes': hashes, 'topology_metadata': topology_record,
        'self_check_only': arguments.self_check, 'guest_test_game_code_loaded': False,
        'trusted_service_syscall_trace': arguments.trace_syscalls,
        'gpu_work_scope': 'at most two fixed synthetic transfer copies; see payload results',
        'profile': 'trusted fixed host-only memory import/copy service',
        'limits': {'virtual_gib': 64, 'data_and_sampled_rss_gib': 8, 'hard_rss_cap': False,
                   'tasks': 32, 'cpus_at_most': 8, 'cpu_seconds_soft_hard': [40, 45], 'wall_seconds': 30,
                   'file_mib': 8, 'file_descriptors': 64},
        'capability_change': 'allowlisted GPU nodes and file-backed sharing within private/RO namespace only'}
    (evidence / (arguments.label + '.run.json')).write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps(record, indent=2))
    return 1 if timed_out or exceeded or monitor_failed else (code if code >= 0 else 128 - code)


if __name__ == '__main__':
    raise SystemExit(main())
