# Native CRT string scanning

The native service layer resolves `__stdio_common_vsscanf` through both
`ucrtbase.dll` and the CRT stdio API contract. It adapts the six Windows x64
arguments directly. Guest variable arguments are consecutive eight-byte slots
containing destination pointers, not a native libc `va_list`.

The current portable parser supports narrow `%s`, `%c`, their optional `h`
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

Options zero and the scanner's legacy-wide-specifier flag (`2`) are accepted for
these narrow conversions. Explicit locales, secure-scanner flags, numeric,
scanset, wide and count conversions remain unsupported and stop explicitly when
reached. No success or assignment count is invented for an unsupported operation.
Null input/format/destinations and zero widths dispatch the guest invalid-parameter
handler and, if it returns, report EOF with guest `EINVAL`. Ordinary matches and
failures preserve host errno, guest errno and Windows LastError.

Original fixtures cover these contracts and an original PE verifies the Windows
register/stack calling positions. Real game boot is checked separately. Passing
these tests does not establish a rendered frame or interactive menu.
