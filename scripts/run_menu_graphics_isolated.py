#!/usr/bin/python3
"""Bounded combined presentation/graphics native-boot runner under standing goal authorization.

64GiB virtual space,8GiB data and a sampled8GiB RSS watchdog. No hard RSS
cgroup is available; the watchdog can overshoot between observations.

Use only in the default outer CLI sandbox, never with elevated tool permissions.
"""
import argparse
from contextlib import contextmanager
import fcntl
import json
import os
from pathlib import Path
import signal
import stat
import subprocess
import tempfile
import time


def process_tree_rss(root_pid):
    """Observe bubblewrap and its descendants; PID exits are ordinary races."""
    pending = [root_pid]
    seen = set()
    total = 0
    while pending:
        pid = pending.pop()
        if pid in seen:
            continue
        seen.add(pid)
        if len(seen) > 64:
            raise RuntimeError('Unexpected process-tree growth in sparse sandbox')
        try:
            status = Path('/proc', str(pid), 'status').read_text()
            children = Path('/proc', str(pid), 'task', str(pid), 'children').read_text()
        except (FileNotFoundError, ProcessLookupError):
            continue
        for line in status.splitlines():
            if line.startswith('VmRSS:'):
                total += int(line.split()[1]) * 1024
                break
        pending.extend(int(value) for value in children.split())
    return total


def snapshot_topology(destination):
    """Copy a bounded allowlist of public topology text, never device handles."""
    import hashlib
    import re

    root = Path('/sys/devices/system')
    total_bytes = 0
    entries = []

    def copy(relative):
        nonlocal total_bytes
        with (root / relative).open('rb') as stream:
            data = stream.read(4097)
        if not data or len(data) > 4096 or total_bytes + len(data) > 512 * 1024:
            raise RuntimeError('Topology metadata exceeds its bounded text allowance')
        data.decode('ascii')
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        target.chmod(0o444)
        total_bytes += len(data)
        entries.append((relative, hashlib.sha256(data).hexdigest()))
        return data.decode('ascii').strip()

    def ids(text):
        result = set()
        for item in text.split(','):
            if not re.fullmatch(r'[0-9]+(?:-[0-9]+)?', item):
                raise RuntimeError('Invalid topology ID list')
            ends = [int(part) for part in item.split('-')]
            first, last = ends[0], ends[-1]
            if first > last or last >= 4096:
                raise RuntimeError('Topology ID list exceeds its bounds')
            for value in range(first, last + 1):
                if value in result:
                    raise RuntimeError('Duplicate topology ID')
                result.add(value)
        return result

    online_text = copy('cpu/online')
    online = ids(online_text)
    selected = sorted(os.sched_getaffinity(0))
    if not selected or len(selected) > 64 or not set(selected) <= online:
        raise RuntimeError('Topology snapshot requires one supported processor group')
    for cpu_id in selected:
        prefix = 'cpu/cpu' + str(cpu_id)
        for field in ('core_id', 'physical_package_id'):
            copy(prefix + '/topology/' + field)
        cache_root = root / prefix / 'cache'
        if cache_root.exists():
            caches = sorted(path for path in cache_root.iterdir()
                            if re.fullmatch(r'index[0-9]+', path.name))
            if len(caches) > 32:
                raise RuntimeError('Too many topology cache descriptors')
            for cache in caches:
                for field in ('level', 'type', 'ways_of_associativity', 'number_of_sets',
                              'coherency_line_size', 'size', 'shared_cpu_list'):
                    copy(prefix + '/cache/' + cache.name + '/' + field)
    for node_id in sorted(ids(copy('node/online'))):
        copy('node/node' + str(node_id) + '/cpulist')
    if (root / 'cpu/online').read_text(encoding='ascii').strip() != online_text:
        raise RuntimeError('CPU online list changed during topology snapshot')
    if sorted(os.sched_getaffinity(0)) != selected:
        raise RuntimeError('CPU affinity changed during topology snapshot')
    digest = hashlib.sha256(json.dumps(sorted(entries), separators=(',', ':')).encode()).hexdigest()
    return {'files': len(entries), 'bytes': total_bytes, 'manifest_sha256': digest,
            'scope': 'allowlisted CPU/core/package/cache and NUMA text only; no unique hardware IDs'}


@contextmanager
def prepared_physical_backing():
    """One sealed sparse pool; always close the parent copy after the child."""
    descriptor = os.memfd_create('ambrosia-physical-pages', os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING)
    try:
        if descriptor != 3:
            raise RuntimeError('Physical sandbox requires dedicated inherited descriptor 3')
        os.fchmod(descriptor, 0o600)
        os.ftruncate(descriptor, 1024**3)
        fcntl.fcntl(descriptor, fcntl.F_ADD_SEALS,
                    fcntl.F_SEAL_GROW | fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_SEAL)
        yield descriptor
    finally:
        os.close(descriptor)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, required=True)
    ap.add_argument('--review', type=Path)
    ap.add_argument('--presentation-root', type=Path, required=True)
    ap.add_argument('--graphics-root', type=Path, required=True)
    ap.add_argument('--evidence', type=Path, required=True)
    ap.add_argument('--label', required=True)
    ap.add_argument('--timeout', type=int, default=60)
    ap.add_argument('entry', nargs=argparse.REMAINDER)
    a = ap.parse_args()
    if not a.label.replace('-', '').replace('_', '').isalnum():
        ap.error('label must be an alphanumeric filename')
    if not 1 <= a.timeout <= 60:
        ap.error('timeout must be 1..60 seconds')
    if a.entry and a.entry[0] == '--':
        a.entry.pop(0)
    if not a.entry:
        ap.error('entrypoint is required')
    source = a.source.resolve(strict=True)
    presentation = a.presentation_root.resolve(strict=True)
    if not presentation.is_dir() or presentation.is_symlink() or stat.S_IMODE(presentation.stat().st_mode) != 0o700:
        ap.error('presentation root must be an existing private directory')
    hidden_roots = [source]
    if a.review:
        hidden_roots.append(a.review.resolve(strict=True))
    if any(presentation.is_relative_to(root) or root.is_relative_to(presentation) for root in hidden_roots):
        ap.error('presentation IPC must be separate from read-only source and review')
    presentation_files = {}
    for name in ('request', 'response', 'frame'):
        path = presentation / name
        information = path.lstat()
        if not stat.S_ISREG(information.st_mode) or information.st_nlink != 1 or information.st_size > 32 * 1024**2:
            ap.error('presentation endpoints must be bounded single-link regular files')
        if name != 'frame' and not 64 <= information.st_size <= 64 + 4096:
            ap.error('presentation protocol endpoint has an invalid size')
        presentation_files[name] = {'device': information.st_dev, 'inode': information.st_ino}
    graphics = a.graphics_root.resolve(strict=True)
    if not graphics.is_dir() or stat.S_IMODE(graphics.stat().st_mode) != 0o700:
        ap.error('graphics root must be an existing private directory')
    if any(graphics.is_relative_to(root) or root.is_relative_to(graphics)
           for root in [*hidden_roots, presentation]):
        ap.error('graphics IPC must be separate from source, review and presentation')
    graphics_files = {}
    for name in ('request', 'response'):
        information = (graphics / name).lstat()
        if not stat.S_ISREG(information.st_mode) or information.st_nlink != 1 or information.st_size != 64:
            ap.error('graphics endpoints must be exact bounded single-link regular files')
        graphics_files[name] = {'device': information.st_dev, 'inode': information.st_ino}
    evidence = a.evidence.resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    for suffix in ('.stdout.log', '.stderr.log', '.run.json'):
        if (evidence / (a.label + suffix)).exists():
            ap.error('refusing to overwrite existing evidence; choose a fresh label')
    bootstrap = Path(__file__).with_name('isolation_menu_graphics_bootstrap.py').resolve(strict=True)
    presentation_controls = Path(__file__).with_name('isolation_presentation_bootstrap.py').resolve(strict=True)
    with prepared_physical_backing() as backing_fd, tempfile.TemporaryDirectory(
            prefix='ambrosia-physical-', dir='/tmp') as td:
        root = Path(td)
        work = root / 'scratch'
        work.mkdir()
        metadata = root / 'topology'
        metadata_record = snapshot_topology(metadata)
        presentation_metadata = root / 'presentation-profile.json'
        presentation_metadata.write_text(json.dumps(presentation_files) + '\n', encoding='utf-8', newline='\n')
        graphics_metadata = root / 'graphics-endpoints.json'
        graphics_metadata.write_text(json.dumps(graphics_files) + '\n', encoding='utf-8', newline='\n')
        (work / 'home').mkdir()
        (work / 'tmp').mkdir()
        cmd = ['/usr/bin/bwrap', '--unshare-user', '--unshare-pid', '--unshare-ipc', '--unshare-uts',
               '--die-with-parent', '--new-session', '--cap-drop', 'ALL',
               '--ro-bind', '/usr', '/usr', '--ro-bind', '/lib', '/lib', '--ro-bind', '/lib64', '/lib64',
               '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp',
               '--ro-bind', str(metadata), '/sys/devices/system',
               '--ro-bind', str(source), '/src', '--bind', str(work), '/work',
               '--ro-bind', str(bootstrap), '/bootstrap.py',
               '--ro-bind', str(presentation_controls), '/presentation_controls.py',
               '--tmpfs', '/graphics',
               '--bind', str(graphics / 'request'), '/graphics/request',
               '--ro-bind', str(graphics / 'response'), '/graphics/response',
               '--ro-bind', str(graphics_metadata), '/graphics-endpoints.json',
               '--remount-ro', '/graphics',
               '--tmpfs', '/presentation',
               '--bind', str(presentation / 'request'), '/presentation/request',
               '--ro-bind', str(presentation / 'response'), '/presentation/response',
               '--bind', str(presentation / 'frame'), '/presentation/frame',
               '--ro-bind', str(presentation_metadata), '/presentation-profile.json',
               '--remount-ro', '/presentation', '--clearenv']
        if a.review:
            cmd += ['--ro-bind', str(a.review.resolve(strict=True)), '/review']
        for k, v in {'PATH': '/usr/bin', 'HOME': '/work/home', 'TMPDIR': '/work/tmp', 'LANG': 'C.UTF-8',
                     'LC_ALL': 'C.UTF-8', 'PYTHONHASHSEED': '0'}.items():
            cmd += ['--setenv', k, v]
        cmd += ['--chdir', '/src', '--', '/usr/bin/python3', '-I', '-S', '-B', '/bootstrap.py', *a.entry]
        t0 = time.monotonic()
        timed_out = False
        rss_exceeded = False
        peak_rss_bytes = 0
        monitor_failed = False
        with ((evidence / (a.label + '.stdout.log')).open('wb') as stdout,
              (evidence / (a.label + '.stderr.log')).open('wb') as stderr):
            proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
                                    env={'PATH': '/usr/bin'}, close_fds=True, pass_fds=(backing_fd,),
                                    start_new_session=True)
            deadline = time.monotonic() + a.timeout
            try:
                while proc.poll() is None:
                    observed = process_tree_rss(proc.pid)
                    peak_rss_bytes = max(peak_rss_bytes, observed)
                    rss_exceeded = observed > 8 * 1024**3
                    timed_out = time.monotonic() >= deadline
                    if rss_exceeded or timed_out:
                        break
                    try:
                        proc.wait(timeout=0.01)
                    except subprocess.TimeoutExpired:
                        pass
            except (OSError, ValueError, RuntimeError):
                monitor_failed = True
            finally:
                if proc.poll() is None:
                    try:
                        os.killpg(proc.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
            code = proc.wait()
        workspace = Path(__file__).resolve().parents[1]
        source_name = str(source.relative_to(workspace)) if source.is_relative_to(workspace) else 'external-source'
        doc = {'label': a.label, 'source_snapshot': source_name, 'entry': a.entry, 'returncode': code,
               'timed_out': timed_out, 'rss_watchdog_exceeded': rss_exceeded,
               'rss_monitor_failed': monitor_failed,
               'observed_peak_process_tree_rss_bytes': peak_rss_bytes, 'wall_seconds': round(time.monotonic() - t0, 3),
               'limits': {'address_space_gib': 64, 'data_mib': 8192, 'sampled_rss_mib': 8192,
                          'hard_rss_cap': False, 'kernel_tasks': 32, 'cpu_seconds_soft': 40, 'cpu_seconds_hard': 45,
                          'single_file_mib': 32, 'open_files': 64, 'wall_seconds': a.timeout},
               'topology_metadata': metadata_record,
               'graphics': {'transport': 'two fixed regular-file endpoints', 'response_read_only': True,
                            'direct_GPU_access': False},
               'presentation': {'transport': 'three fixed regular-file endpoints',
                                'response_read_only': True, 'network_and_display_credentials': 'not exposed'},
               'physical_backing': {'descriptor': 3, 'bytes': 1024**3, 'fixed_size_sealed': True,
                                    'other_shared_mappings': 'denied',
                                    'guest_file_limit_mib': 32},
               'isolation': 'read-only allowlisted CPU/NUMA snapshot + bubblewrap user/PID/IPC/UTS namespaces + read-only sources + empty HOME/environment + seccomp network/exec/process/WX denial + shared-map denial except dedicated backing FD + exact pthread clone flags and hard RLIMIT_NPROC=32 +8GiB data limit +designated sealed 1GiB backing FD +sampled RSS watchdog; no hard RSS cap; outer tool sandbox retained'}
        (evidence / (a.label + '.run.json')).write_text(json.dumps(doc, indent=2) + '\n', encoding='utf-8', newline='\n')
        print(json.dumps(doc, indent=2))
        for stream in ('stdout', 'stderr'):
            p = evidence / (a.label + '.' + stream + '.log')
            text = p.read_text(encoding='utf-8', errors='replace')
            print(stream.upper() + '\n' + (text if len(text) <= 20000 else text[:20000] + '\n[display truncated; log retained]'))
        if monitor_failed or rss_exceeded or timed_out:
            return 1
        return code if code >= 0 else 128 - code


if __name__ == '__main__':
    raise SystemExit(main())
