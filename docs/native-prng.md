# Native CRT random state

`srand` and `rand` share one typed Windows x64 implementation across the CRT
aliases. Each owned guest thread context starts with seed 1. Seeding replaces
that context's unsigned 32-bit state. Each draw applies the Windows recurrence
`state = state * 214013 + 2531011`, wrapping modulo 2³², then returns bits 16
through 30, a value from 0 to 32767. The default sequence begins 41, 18467, 6334.

State follows guest context lifetime and activation. Nested contexts and new
native workers have independent state; reactivating a context resumes its
sequence. Existing callback adapters create fresh guest contexts, so their random
state follows the same lifetime as their TLS and error state. Access requires
the owning native thread, an active guest context and a scoped runtime.

These operations preserve host errno, guest errno and LastError. They do not
use host libc's generator. `rand_s` continues to use OS entropy and never advances
the deterministic sequence.

The contracts follow Microsoft's [srand documentation](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/srand)
and [rand documentation](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/rand).
Original fixtures check fixed vectors, isolation, errors and a two-import PE call.
Real game startup is verified separately; these tests do not establish a menu.
