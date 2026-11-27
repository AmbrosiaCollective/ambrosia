#!/usr/bin/python3
"""Trusted, bounded SDL presentation service. Never load guest code or game files.

The CLI sandbox retains guest execution. This service alone opens the host
display and exchanges fixed, validated requests through three pinned files.
"""
import argparse
import ctypes
import errno
import json
import os
from pathlib import Path
import resource
import signal
import stat
import struct
import time


MAGIC = b'AMBWIN01'
HEADER = struct.Struct('<8sIIIIiiiiIIII8x')
EVENT = struct.Struct('<IIIIii')
MAX_PIXELS = 3840 * 2160
MAX_PAYLOAD = 4096
PROJECT_TITLE = b'Ambrosia'


def project_icon():
    path = Path(__file__).resolve().parents[1] / 'assets' / 'window-icon.rgba'
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        information = os.fstat(descriptor)
        if not stat.S_ISREG(information.st_mode) or information.st_nlink != 1 or information.st_size > 262160:
            raise RuntimeError('invalid project icon asset')
        data = os.pread(descriptor, information.st_size, 0)
    finally:
        os.close(descriptor)
    if len(data) < 16:
        raise RuntimeError('truncated project icon asset')
    magic, width, height = struct.unpack_from('<8sII', data)
    if magic != b'AMBICON1' or not 1 <= width <= 256 or not 1 <= height <= 256:
        raise RuntimeError('invalid project icon dimensions')
    if len(data) != 16 + width * height * 4:
        raise RuntimeError('invalid project icon pixel count')
    return width, height, data[16:]


class SdlVersion(ctypes.Structure):
    _fields_ = [('major', ctypes.c_ubyte), ('minor', ctypes.c_ubyte), ('patch', ctypes.c_ubyte)]


class WindowManagerInfo(ctypes.Structure):
    _fields_ = [('version', SdlVersion), ('subsystem', ctypes.c_int), ('information', ctypes.c_uint64 * 8)]


class OwnedBranding:
    """Read back three fixed properties from this service's own X11 window only."""
    def __init__(self, sdl):
        self.sdl = sdl
        self.x11 = ctypes.CDLL('libX11.so.6')
        self.x11.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
        self.x11.XInternAtom.restype = ctypes.c_ulong
        self.x11.XGetWindowProperty.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_long, ctypes.c_long,
            ctypes.c_int, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_void_p)]
        self.x11.XGetWindowProperty.restype = ctypes.c_int
        self.x11.XFree.argtypes = [ctypes.c_void_p]
        self.x11.XFree.restype = ctypes.c_int

    def property(self, display, window, name, expected_type, expected_format, items):
        atom = self.x11.XInternAtom(display, name, 1)
        actual_type, count, remaining = ctypes.c_ulong(), ctypes.c_ulong(), ctypes.c_ulong()
        actual_format = ctypes.c_int()
        pointer = ctypes.c_void_p()
        length = items if expected_format == 32 else (items + 3) // 4
        status = self.x11.XGetWindowProperty(display, window, atom, 0, length, 0, expected_type,
            ctypes.byref(actual_type), ctypes.byref(actual_format), ctypes.byref(count),
            ctypes.byref(remaining), ctypes.byref(pointer))
        try:
            if (not atom or status or actual_type.value != expected_type or
                    actual_format.value != expected_format or count.value != items or
                    remaining.value or not pointer.value):
                raise RuntimeError('owned window branding property failed readback')
            if expected_format == 8:
                return ctypes.string_at(pointer, items)
            values = ctypes.cast(pointer, ctypes.POINTER(ctypes.c_ulong))
            return [values[index] & 0xffffffff for index in range(items)]
        finally:
            if pointer.value:
                self.x11.XFree(pointer)

    def verify(self, window, width, height, pixels):
        information = WindowManagerInfo()
        self.sdl.SDL_GetVersion(ctypes.byref(information.version))
        if not self.sdl.SDL_GetWindowWMInfo(window, ctypes.byref(information)) or information.subsystem != 2:
            raise RuntimeError('owned X11 window identity unavailable')
        display, native_window = information.information[:2]
        utf8 = self.x11.XInternAtom(display, b'UTF8_STRING', 1)
        title = self.property(display, native_window, b'_NET_WM_NAME', utf8, 8, len(PROJECT_TITLE))
        window_class = self.property(display, native_window, b'WM_CLASS', 31, 8, 2 * (len(PROJECT_TITLE) + 1))
        icon = self.property(display, native_window, b'_NET_WM_ICON', 6, 32, 2 + width * height)
        expected = [width, height]
        for offset in range(0, len(pixels), 4):
            red, green, blue, alpha = pixels[offset:offset + 4]
            expected.append((alpha << 24) | (red << 16) | (green << 8) | blue)
        if title != PROJECT_TITLE or window_class != (PROJECT_TITLE + b'\0') * 2 or icon != expected:
            raise RuntimeError('owned window branding disagrees with project identity')


def limit_lifetime(seconds):
    # Default signal termination still works if a native display call blocks.
    signal.signal(signal.SIGALRM, signal.SIG_DFL)
    signal.alarm(seconds + 5)


def restrict_after_display_start():
    """The connected SDL display remains; no new socket/process capability."""
    libc = ctypes.CDLL(None, use_errno=True)
    libc.prctl.argtypes = [ctypes.c_int, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong]
    if libc.prctl(38, 1, 0, 0, 0):
        raise RuntimeError('cannot set broker no_new_privs')
    library = ctypes.CDLL('libseccomp.so.2', use_errno=True)
    library.seccomp_init.argtypes = [ctypes.c_uint32]
    library.seccomp_init.restype = ctypes.c_void_p
    library.seccomp_syscall_resolve_name.argtypes = [ctypes.c_char_p]
    library.seccomp_syscall_resolve_name.restype = ctypes.c_int
    library.seccomp_rule_add.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int, ctypes.c_uint]
    library.seccomp_rule_add.restype = ctypes.c_int
    library.seccomp_load.argtypes = [ctypes.c_void_p]
    library.seccomp_load.restype = ctypes.c_int
    library.seccomp_release.argtypes = [ctypes.c_void_p]
    library.seccomp_release.restype = None
    context = library.seccomp_init(0x7fff0000)
    if not context:
        raise RuntimeError('cannot create broker filter')
    try:
        names = '''socket socketpair connect bind listen accept accept4 execve execveat
        fork vfork clone clone3 ptrace process_vm_readv process_vm_writev pidfd_getfd
        mount umount2 unshare setns bpf perf_event_open keyctl add_key request_key
        io_uring_setup io_uring_enter io_uring_register open_by_handle_at'''.split()
        for name in names:
            number = library.seccomp_syscall_resolve_name(name.encode('ascii'))
            if number >= 0 and library.seccomp_rule_add(context, 0x50000 | errno.EPERM, number, 0):
                raise RuntimeError('cannot restrict broker syscall')
        if library.seccomp_load(context):
            raise RuntimeError('cannot load broker filter')
    finally:
        library.seccomp_release(context)
    # These checks create no connection and execute no child if controls hold.
    import socket
    import subprocess
    try:
        unexpected = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    except PermissionError:
        pass
    else:
        unexpected.close()
        raise RuntimeError('broker socket creation restriction failed')
    try:
        subprocess.run(['/usr/bin/true'], check=True)
    except PermissionError:
        pass
    else:
        raise RuntimeError('broker execution restriction failed')


class Video:
    def __init__(self):
        self.library = ctypes.CDLL('libSDL2-2.0.so.0')
        signatures = {
            'SDL_Init': ([ctypes.c_uint], ctypes.c_int),
            'SDL_Quit': ([], None),
            'SDL_SetHint': ([ctypes.c_char_p, ctypes.c_char_p], ctypes.c_int),
            'SDL_CreateWindow': ([ctypes.c_char_p, ctypes.c_int, ctypes.c_int,
                                  ctypes.c_int, ctypes.c_int, ctypes.c_uint], ctypes.c_void_p),
            'SDL_DestroyWindow': ([ctypes.c_void_p], None),
            'SDL_ShowWindow': ([ctypes.c_void_p], None),
            'SDL_HideWindow': ([ctypes.c_void_p], None),
            'SDL_SetWindowTitle': ([ctypes.c_void_p, ctypes.c_char_p], None),
            'SDL_GetWindowTitle': ([ctypes.c_void_p], ctypes.c_char_p),
            'SDL_SetWindowIcon': ([ctypes.c_void_p, ctypes.c_void_p], None),
            'SDL_GetVersion': ([ctypes.POINTER(SdlVersion)], None),
            'SDL_GetWindowWMInfo': ([ctypes.c_void_p, ctypes.POINTER(WindowManagerInfo)], ctypes.c_int),
            'SDL_GetWindowID': ([ctypes.c_void_p], ctypes.c_uint),
            'SDL_GetWindowFlags': ([ctypes.c_void_p], ctypes.c_uint),
            'SDL_RaiseWindow': ([ctypes.c_void_p], None),
            'SDL_GetWindowSize': ([ctypes.c_void_p, ctypes.POINTER(ctypes.c_int),
                                    ctypes.POINTER(ctypes.c_int)], None),
            'SDL_GetWindowPosition': ([ctypes.c_void_p, ctypes.POINTER(ctypes.c_int),
                                        ctypes.POINTER(ctypes.c_int)], None),
            'SDL_GetWindowBordersSize': ([ctypes.c_void_p, ctypes.POINTER(ctypes.c_int),
                ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
                ctypes.POINTER(ctypes.c_int)], ctypes.c_int),
            'SDL_PollEvent': ([ctypes.c_void_p], ctypes.c_int),
            'SDL_GetWindowSurface': ([ctypes.c_void_p], ctypes.c_void_p),
            'SDL_CreateRGBSurfaceFrom': ([ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                                           ctypes.c_int, ctypes.c_int, ctypes.c_uint,
                                           ctypes.c_uint, ctypes.c_uint, ctypes.c_uint], ctypes.c_void_p),
            'SDL_UpperBlitScaled': ([ctypes.c_void_p, ctypes.c_void_p,
                                      ctypes.c_void_p, ctypes.c_void_p], ctypes.c_int),
            'SDL_UpdateWindowSurface': ([ctypes.c_void_p], ctypes.c_int),
            'SDL_FreeSurface': ([ctypes.c_void_p], None),
        }
        for name, (arguments, result) in signatures.items():
            function = getattr(self.library, name)
            function.argtypes = arguments
            function.restype = result
        self.windows = {}
        self.next_id = 1
        self.icon_width, self.icon_height, self.icon_data = project_icon()
        self.icon_pixels = ctypes.create_string_buffer(self.icon_data)
        self.icon_surface = None
        self.branding = OwnedBranding(self.library)
        self.library.SDL_SetHint(b'SDL_APP_NAME', PROJECT_TITLE)
        self.library.SDL_SetHint(b'SDL_FRAMEBUFFER_ACCELERATION', b'0')
        self.library.SDL_SetHint(b'SDL_RENDER_DRIVER', b'software')
        if self.library.SDL_Init(0x20):
            raise RuntimeError('cannot initialize native software presentation')
        self.icon_surface = self.library.SDL_CreateRGBSurfaceFrom(
            self.icon_pixels, self.icon_width, self.icon_height, 32, self.icon_width * 4,
            0xff, 0xff00, 0xff0000, 0xff000000)
        if not self.icon_surface:
            raise RuntimeError('cannot initialize project icon surface')

    def close(self):
        for pointer in self.windows.values():
            self.library.SDL_DestroyWindow(pointer)
        self.windows.clear()
        if self.icon_surface:
            self.library.SDL_FreeSurface(self.icon_surface)
            self.icon_surface = None
        self.library.SDL_Quit()

    def geometry(self, pointer):
        x, y, width, height = (ctypes.c_int() for _ in range(4))
        self.library.SDL_GetWindowPosition(pointer, ctypes.byref(x), ctypes.byref(y))
        self.library.SDL_GetWindowSize(pointer, ctypes.byref(width), ctypes.byref(height))
        return x.value, y.value, width.value, height.value

    def events(self):
        result = []
        buffer = ctypes.create_string_buffer(56)
        identifiers = {self.library.SDL_GetWindowID(pointer): key for key, pointer in self.windows.items()}
        for _ in range(64):
            if not self.library.SDL_PollEvent(buffer):
                break
            data = buffer.raw
            event_type, _, native_id = struct.unpack_from('<III', data)
            window = identifiers.get(native_id)
            if not window:
                continue
            if event_type == 0x200:
                subtype = data[12]
                first, second = struct.unpack_from('<ii', data, 16)
                kind = {14: 1, 5: 7, 6: 7, 12: 8, 13: 9}.get(subtype)
                if kind:
                    result.append(EVENT.pack(kind, window, 0, 0, first, second))
            elif event_type in (0x300, 0x301):
                scancode, keycode = struct.unpack_from('<ii', data, 16)
                modifiers = struct.unpack_from('<H', data, 24)[0]
                result.append(EVENT.pack(2 if event_type == 0x300 else 3,
                                         window, modifiers, data[13], scancode, keycode))
            elif event_type == 0x400:
                state, x, y = struct.unpack_from('<Iii', data, 16)
                result.append(EVENT.pack(4, window, state, 0, x, y))
            elif event_type in (0x401, 0x402):
                x, y = struct.unpack_from('<ii', data, 20)
                result.append(EVENT.pack(5 if event_type == 0x401 else 6,
                                         window, data[16], data[18], x, y))
        return b''.join(result)

    def execute(self, fields, payload, frame_fd):
        _, _, operation, identifier, _, x, y, width, height, flags, argument, _, _ = fields
        if operation == 1:
            if identifier or len(self.windows) == 4 or self.next_id > 64:
                return 4, 0, (0, 0, 0, 0), b'', 0
            if (width <= 0 or width > 4096 or height <= 0 or height > 2160 or
                    width * height > MAX_PIXELS or flags & ~7 or argument):
                return 1, 0, (0, 0, 0, 0), b'', 0
            if any(value != -2147483648 and not -32768 <= value <= 32767 for value in (x, y)):
                return 1, 0, (0, 0, 0, 0), b'', 0
            try:
                payload.decode('utf-8', errors='strict')
            except UnicodeDecodeError:
                return 1, 0, (0, 0, 0, 0), b'', 0
            if b'\0' in payload:
                return 1, 0, (0, 0, 0, 0), b'', 0
            native_flags = (4 if flags & 1 else 8) | (32 if flags & 2 else 0) | (16 if flags & 4 else 0)
            pointer = self.library.SDL_CreateWindow(PROJECT_TITLE,
                0x1fff0000 if x == -2147483648 else x,
                0x1fff0000 if y == -2147483648 else y, width, height, native_flags)
            if not pointer:
                return 3, 0, (0, 0, 0, 0), b'', 0
            self.library.SDL_SetWindowIcon(pointer, self.icon_surface)
            try:
                self.branding.verify(pointer, self.icon_width, self.icon_height, self.icon_data)
            except RuntimeError:
                self.library.SDL_DestroyWindow(pointer)
                return 3, 0, (0, 0, 0, 0), b'', 0
            print(json.dumps({'project_branding_verified': True, 'title': 'Ambrosia',
                              'desktop_class': 'Ambrosia', 'icon': [self.icon_width, self.icon_height]}), flush=True)
            identifier = self.next_id
            self.next_id += 1
            self.windows[identifier] = pointer
            return 0, identifier, self.geometry(pointer), b'', 0
        if operation == 4 and not identifier:
            events = self.events()
            return 0, 0, (0, 0, 0, 0), events, len(events) // EVENT.size
        pointer = self.windows.get(identifier)
        if not pointer:
            return 2, identifier, (0, 0, 0, 0), b'', 0
        if operation == 2:
            self.library.SDL_DestroyWindow(pointer)
            del self.windows[identifier]
            return 0, identifier, (0, 0, 0, 0), b'', 0
        if operation == 3 and flags in (0, 1):
            (self.library.SDL_ShowWindow if flags else self.library.SDL_HideWindow)(pointer)
            return 0, identifier, self.geometry(pointer), b'', 0
        if operation == 6:
            try:
                payload.decode('utf-8', errors='strict')
            except UnicodeDecodeError:
                return 1, identifier, (0, 0, 0, 0), b'', 0
            if b'\0' in payload:
                return 1, identifier, (0, 0, 0, 0), b'', 0
            self.library.SDL_SetWindowTitle(pointer, PROJECT_TITLE)
            if self.library.SDL_GetWindowTitle(pointer) != PROJECT_TITLE:
                return 3, identifier, (0, 0, 0, 0), b'', 0
            return 0, identifier, self.geometry(pointer), b'', 0
        if operation == 7:
            if payload or flags or argument or any((x, y, width, height)):
                return 1, identifier, (0, 0, 0, 0), b'', 0
            top, left, bottom, right = (ctypes.c_int() for _ in range(4))
            result = self.library.SDL_GetWindowBordersSize(
                pointer, ctypes.byref(top), ctypes.byref(left),
                ctypes.byref(bottom), ctypes.byref(right))
            borders = left.value, top.value, right.value, bottom.value
            if result or any(value < 0 or value > 4096 for value in borders):
                return 3, identifier, (0, 0, 0, 0), b'', 0
            return 0, identifier, borders, b'', 0
        if operation == 8:
            if payload or flags not in (0, 1, 2, 3) or argument or any((x, y, width, height)):
                return 1, identifier, (0, 0, 0, 0), b'', 0
            # Normal-mode creation is supported. Restoring an existing native
            # minimized/maximized window needs its resize/message integration.
            if flags == 3 and self.library.SDL_GetWindowFlags(pointer) & (64 | 128):
                return 3, identifier, (0, 0, 0, 0), b'', 0
            previous = bool(self.library.SDL_GetWindowFlags(pointer) & 4)
            if flags != 2:
                (self.library.SDL_ShowWindow if flags else self.library.SDL_HideWindow)(pointer)
                if flags:
                    self.library.SDL_RaiseWindow(pointer)
            visible = bool(self.library.SDL_GetWindowFlags(pointer) & 4)
            if flags != 2 and visible != bool(flags):
                return 3, identifier, (0, 0, 0, 0), b'', 0
            state = int(previous) | (int(visible) << 1)
            return 0, identifier, self.geometry(pointer), b'', state
        if operation == 5:
            if width <= 0 or height <= 0 or width * height > MAX_PIXELS or width > 4096 or height > 2160:
                return 1, identifier, (0, 0, 0, 0), b'', 0
            data = os.pread(frame_fd, width * height * 4, 0)
            if len(data) != width * height * 4:
                return 1, identifier, (0, 0, 0, 0), b'', 0
            pixels = ctypes.create_string_buffer(data)
            source = self.library.SDL_CreateRGBSurfaceFrom(pixels, width, height, 32, width * 4,
                                                           0xff, 0xff00, 0xff0000, 0xff000000)
            target = self.library.SDL_GetWindowSurface(pointer)
            if not source or not target:
                if source:
                    self.library.SDL_FreeSurface(source)
                return 3, identifier, (0, 0, 0, 0), b'', 0
            try:
                result = self.library.SDL_UpperBlitScaled(source, None, target, None)
                if not result:
                    result = self.library.SDL_UpdateWindowSurface(pointer)
            finally:
                self.library.SDL_FreeSurface(source)
            return (0 if result == 0 else 3), identifier, self.geometry(pointer), b'', 0
        return 1, identifier, (0, 0, 0, 0), b'', 0


def serve(root, seconds):
    information = root.lstat()
    if not stat.S_ISDIR(information.st_mode) or stat.S_IMODE(information.st_mode) != 0o700:
        raise RuntimeError('presentation root must be a private existing directory')
    directory = os.open(root, os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    files = []
    video = None
    try:
        for name in ('request', 'response', 'frame'):
            files.append(os.open(name, os.O_CREAT | os.O_EXCL | os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC,
                                 0o600, dir_fd=directory))
        request_fd, response_fd, frame_fd = files
        os.pwrite(request_fd, bytes(HEADER.size), 0)
        os.pwrite(response_fd, bytes(HEADER.size), 0)
        video = Video()
        restrict_after_display_start()
        os.environ.clear()
        print(json.dumps({'presentation_broker_ready': True, 'protocol': 1,
                          'guest_execution': False, 'maximum_pixels': MAX_PIXELS}), flush=True)
        deadline = time.monotonic() + seconds
        previous = 0
        requests = 0
        while time.monotonic() < deadline and requests < 8192:
            raw = os.pread(request_fd, HEADER.size, 0)
            if len(raw) != HEADER.size:
                raise RuntimeError('invalid presentation request header size')
            fields = HEADER.unpack(raw)
            magic, sequence, operation, identifier, size, x, y, width, height, flags, argument, reserved1, reserved2 = fields
            if sequence <= previous:
                time.sleep(0.002)
                continue
            requests += 1
            if magic != MAGIC or size > MAX_PAYLOAD or reserved1 or reserved2 or operation not in range(1, 9):
                status, identifier, geometry, output, argument = 1, 0, (0, 0, 0, 0), b'', 0
            else:
                payload = os.pread(request_fd, size, HEADER.size)
                if len(payload) != size or raw != os.pread(request_fd, HEADER.size, 0):
                    time.sleep(0.002)
                    continue
                status, identifier, geometry, output, argument = video.execute(fields, payload, frame_fd)
            previous = sequence
            # Payload is committed before the sequence-bearing response header.
            if output:
                os.pwrite(response_fd, output, HEADER.size)
            response = HEADER.pack(MAGIC, sequence, operation, identifier, len(output),
                                   *geometry, status, argument, 0, 0)
            os.pwrite(response_fd, response, 0)
        print(json.dumps({'presentation_broker_stopped': True, 'requests': requests}), flush=True)
    finally:
        if video is not None:
            video.close()
        for descriptor in files:
            os.close(descriptor)
        os.close(directory)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=60)
    arguments = parser.parse_args()
    if not 1 <= arguments.seconds <= 120:
        parser.error('seconds must be from1 to120')
    limit_lifetime(arguments.seconds)
    environment = {key: os.environ[key] for key in ('DISPLAY', 'XAUTHORITY') if key in os.environ}
    environment.update(PATH='/usr/bin', SDL_VIDEODRIVER='x11', SDL_RENDER_DRIVER='software',
                       SDL_VIDEO_X11_WMCLASS='Ambrosia')
    os.environ.clear()
    os.environ.update(environment)
    resource.setrlimit(resource.RLIMIT_AS, (2 * 1024**3,) * 2)
    resource.setrlimit(resource.RLIMIT_DATA, (512 * 1024**2,) * 2)
    resource.setrlimit(resource.RLIMIT_CPU, (40, 45))
    resource.setrlimit(resource.RLIMIT_FSIZE, (32 * 1024**2,) * 2)
    resource.setrlimit(resource.RLIMIT_NOFILE, (128, 128))
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    serve(arguments.root, arguments.seconds)


if __name__ == '__main__':
    main()
