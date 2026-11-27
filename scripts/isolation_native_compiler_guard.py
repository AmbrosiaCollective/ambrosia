"""Fail-closed checks before the fixed compiler commands; never run compiled output."""
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import resource
import socket
import stat


def denied_write(path, create=False):
    flags = os.O_WRONLY | os.O_NOFOLLOW
    if create:
        flags |= os.O_CREAT | os.O_EXCL
    try:
        descriptor = os.open(path, flags, 0o600)
    except OSError as error:
        return error.errno in (errno.EROFS, errno.EACCES, errno.EPERM)
    os.close(descriptor)
    if create:
        os.unlink(path)
    return False


def socket_denied(family):
    try:
        connection = socket.socket(family, socket.SOCK_STREAM)
    except OSError as error:
        return error.errno == errno.EPERM
    connection.close()
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--header-sha256', required=True)
    parser.add_argument('--host-pid-namespace', type=int, required=True)
    parser.add_argument('--file-mib', type=int, choices=(64, 256), required=True)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if len(args.header_sha256) != 64 or any(char not in '0123456789abcdef' for char in args.header_sha256):
        parser.error('invalid expected public-header hash')
    command = args.command
    if command and command[0] == '--':
        command = command[1:]
    if command and command[0] not in ('/usr/bin/g++', '/usr/bin/clang++-18', '/usr/bin/cmake'):
        parser.error('only fixed compiler tools may follow the guard')
    checks = {}
    for path in ('/home', '/root', '/run', '/etc', '/host', '/media', '/mnt', '/sys'):
        checks['hidden:' + path] = not Path(path).exists()
    checks['clean_environment'] = dict(os.environ) == {
        'PATH': '/usr/bin', 'HOME': '/work/home', 'TMPDIR': '/work/tmp', 'LANG': 'C.UTF-8', 'PWD': '/work'}
    checks['private_pid_namespace'] = os.stat('/proc/self/ns/pid').st_ino != args.host_pid_namespace
    checks['GPU_devices_hidden'] = not Path('/dev/dri').exists() and not list(Path('/dev').glob('nvidia*'))
    checks['internet_socket_denied'] = socket_denied(socket.AF_INET)
    checks['unix_socket_denied'] = socket_denied(socket.AF_UNIX)
    checks['source_read_only'] = denied_write('/src/.compiler-write-probe', create=True)
    header = Path('/portable/command_list.h')
    information = header.lstat()
    checks['portable_header_regular'] = stat.S_ISREG(information.st_mode) and information.st_nlink == 1
    checks['portable_header_hash'] = hashlib.sha256(header.read_bytes()).hexdigest() == args.header_sha256
    checks['portable_header_read_only'] = denied_write(header)
    checks['portable_directory_read_only'] = denied_write('/portable/.compiler-write-probe', create=True)
    placeholder = Path('/work/portable-root/command_list.h').stat()
    checks['no_scratch_writable_header_alias'] = (placeholder.st_dev, placeholder.st_ino) != (
        information.st_dev, information.st_ino)
    scratch = Path('/work/.compiler-scratch-probe')
    scratch.write_bytes(b'1')
    checks['scratch_writable'] = scratch.read_bytes() == b'1'
    scratch.unlink()
    expected_limits = {
        'address_space_2gib': (resource.RLIMIT_AS, (2 * 1024**3,) * 2),
        'cpu_45_50_seconds': (resource.RLIMIT_CPU, (45, 50)),
        'scratch_file_bound': (resource.RLIMIT_FSIZE, (args.file_mib * 1024**2,) * 2),
        'descriptor_bound_128': (resource.RLIMIT_NOFILE, (128, 128)),
        'core_dump_disabled': (resource.RLIMIT_CORE, (0, 0)),
    }
    for name, (limit, expected) in expected_limits.items():
        checks[name] = resource.getrlimit(limit) == expected
    print(json.dumps({'compiler_isolation_checks': checks, 'compiled_output_executed': False}), flush=True)
    if not all(checks.values()):
        raise RuntimeError('compiler isolation checks failed')
    if command:
        os.execv(command[0], command)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
