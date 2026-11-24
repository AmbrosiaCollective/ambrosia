# Native runtime source provenance

The inherited repository license and credits continue to apply. New native code
must retain applicable notices when adapting source. Pin origins and keep tests
and host-specific changes clear.

| Files | Origin and adaptation |
|---|---|
| `pe_exports.cpp`, `module_graph.cpp` | Adapted from the inherited `runtime/src/pe_loader.cpp` export parsing, forwarder resolution and dependency-ordering logic at Ambrosia commit `17176bd3a29b5851d573e768a0e365ac707489b9`. Project GPL-3.0-or-later terms continue. Windows host-DLL fallbacks and TLS-helper DLLs are excluded from the native adapters. |
| `forwarder_cases.h` | The seven-case `TARGETS` matrix from [Xbox2Windows](https://github.com/R0Y00/Xbox2Windows/blob/939e5142458007405bbc7a2a6d14bad292fcae62/tests/intake/forwarders_check.py), adapted from Python inventory assertions to C++ native module checks. MIT license reproduced in `LICENSE.Xbox2Windows`. |

No upstream Python script, pefile dependency, setup/build helper, binary or game
data is imported. Original native fixtures supply the PE bytes and execute the
cases in isolation. Reused cases keep their original technical meaning; native
execution coverage does not claim retail-game validation.
