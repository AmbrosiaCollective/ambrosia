"""Bounded resource observations and immutable topology copies for host services."""
import json
import os
from pathlib import Path

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
