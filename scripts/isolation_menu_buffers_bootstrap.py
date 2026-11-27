"""Combined file-only presentation/graphics guest profile. Never escalate execution."""
import errno
import importlib.util
import json
import os
from pathlib import Path
import runpy
import stat
import sys


spec = importlib.util.spec_from_file_location('reviewed_presentation_controls', '/presentation_controls.py')
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)


def graphics_checks():
    expected = json.loads(Path('/graphics-endpoints.json').read_text())
    root = Path('/graphics')
    checks = {'graphics_endpoint_names_fixed': sorted(path.name for path in root.iterdir()) == ['download', 'request', 'response', 'upload']}
    for name in ('request', 'response', 'upload', 'download'):
        information = (root / name).lstat()
        checks['graphics_' + name + '_identity'] = (stat.S_ISREG(information.st_mode)
            and information.st_nlink == 1 and information.st_size == (1024**2 if name in ('upload', 'download') else 64)
            and information.st_dev == expected[name]['device']
            and information.st_ino == expected[name]['inode'])
    for name in ('request', 'upload'):
        with (root / name).open('r+b') as stream:
            saved = stream.read(1)
            stream.seek(0)
            checks['graphics_' + name + '_writable'] = len(saved) == 1 and stream.write(saved) == 1
    for name, path in (('graphics_response_read_only', root / 'response'),
                       ('graphics_download_read_only', root / 'download'),
                       ('graphics_endpoint_metadata_read_only', Path('/graphics-endpoints.json'))):
        try:
            with path.open('r+b') as stream:
                stream.write(b'probe')
        except OSError as error:
            checks[name] = error.errno == errno.EROFS
        else:
            checks[name] = False
    try:
        (root / 'extra').write_bytes(b'probe')
    except OSError as error:
        checks['graphics_directory_not_writable'] = error.errno == errno.EROFS
    else:
        checks['graphics_directory_not_writable'] = False
    checks['graphics_no_writable_directory_alias'] = not Path('/work/graphics').exists()
    if not all(checks.values()):
        raise RuntimeError('Graphics guest endpoint controls failed: ' + json.dumps(checks))
    return checks


def main():
    arguments = sys.argv[1:]
    if not arguments:
        raise RuntimeError('No reviewed entrypoint supplied')
    blocked = base.restrict()
    checks = base.self_check()
    checks.update(base.memory_self_check())
    checks.update(base.topology_self_check())
    checks.update(base.physical_self_check())
    checks.update(base.presentation_self_check())
    checks.update(graphics_checks())
    thread_checks, workers = base.thread_self_check()
    checks.update(thread_checks)
    print('ISOLATION ' + json.dumps({'checks': checks, 'blocked_syscalls': blocked,
                                   'thread_cap_probe_workers': workers}), flush=True)
    if arguments == ['--self-check']:
        return
    entry = Path(arguments[0]).resolve()
    if not (entry.is_relative_to('/src') or entry.is_relative_to('/review')) or not entry.is_file():
        raise RuntimeError('Entrypoint must be a reviewed file under /src or /review')
    sys.dont_write_bytecode = True
    sys.argv = arguments
    runpy.run_path(str(entry), run_name='__main__')


if __name__ == '__main__':
    main()
