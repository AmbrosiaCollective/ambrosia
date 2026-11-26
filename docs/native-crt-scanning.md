# Native CRT string scanning

The native service layer resolves `__stdio_common_vsscanf` through both
`ucrtbase.dll` and the CRT stdio API contract. It adapts the six Windows x64
arguments directly. Guest variable arguments are consecutive eight-byte slots
containing destination pointers, not a native libc `va_list`.

The portable parser supports narrow `%s`, `%c`, their optional `h`
modifier, positive field widths, assignment suppression, ordinary literals,
`%%` and C-locale whitespace. `%s` skips leading whitespace, writes actual input
bytes and appends a NUL. `%c` reads its exact width, defaults to one byte, retains
whitespace and adds no terminator. Suppression consumes input but no argument or
assignment. Matching failure returns the assignments so far; input exhaustion
before any completed conversion returns EOF. Completed suppressed conversions
count for that distinction. This is a non-secure API: callers must provide enough
storage, including the extra terminator for `%s`.

Input is copied before parsing, bounded by the supplied count or the first NUL.
The runtime allows at most 32,768 input bytes, a bounded format string, widths
up to 32,768 and 128 conversions/destination slots. Reading an unbounded string
past that budget stops explicitly. A finite count needs no input terminator.
Unaligned Windows argument slots are decoded with `memcpy`. Byte classification
is explicit and independent of the native process locale.

Integer conversions `%d`, `%i`, `%u`, `%o`, `%x` and `%X` use standard C++
`from_chars` after adapting optional signs and the CRT base prefixes. Auto-base
`%i` accepts decimal, leading-zero octal and `0x` hexadecimal. Width counts the
sign and prefix. The parser copies exactly 1, 2, 4 or 8 bytes for `hh`, `h`,
default/`l`/`I32` or `ll`/`I64`/`I`/`j`/`z`/`t`, respectively. Windows `long`
remains 32-bit on Linux. Unsigned negation uses the destination width; unaligned
destinations are supported. Values outside the selected signed/unsigned range
stop explicitly before storage rather than relying on libc overflow behavior.
Suppressed integer fields still consume no argument or assignment.

Options zero and the scanner's legacy-wide-specifier flag (`2`) are accepted for
these narrow conversions. Explicit locales, secure-scanner flags, floating-point, pointer,
scanset, wide and count conversions remain unsupported and stop explicitly when
reached. No success or assignment count is invented for an unsupported operation.
Null input/format/destinations and zero widths dispatch the guest invalid-parameter
handler and, if it returns, report EOF with guest `EINVAL`. Ordinary matches and
failures preserve host errno, guest errno and Windows LastError.

Original fixtures cover these contracts and an original PE verifies the Windows
register/stack calling positions. Real game boot is checked separately. Passing
these tests does not establish a rendered frame or interactive menu.
