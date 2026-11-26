# Native persistent-storage paths

The native GDK layer provides the persistent-local-storage class and its known
interface revisions with a counted IUnknown prefix. Path sizing and path retrieval
use the Windows x64 method layout. Space accounting, space prompts and package
mounts remain explicit unsupported calls.

Run modes accept `--storage-root DIR` once. Select an existing directory outside
the package and executable roots, including their ancestors and descendants.
The runtime validates this separation before testing writable access. It creates
no storage directory implicitly and never uses a developer-specific location.
Without a binding, path methods return a missing-path error with cleared counts.

A narrow Linux host adapter pins the directory, verifies exclusive empty-file
creation/removal without following links, and checks its identity and writable
permissions when queried. Root symlink aliases, replaced directories and lost
write access fail explicitly. The portable service returns the actual UTF-8
native path, including the NUL in size/used counts. It supports an omitted used
count, preserves error state and rejects time-sensitive or missing guest contexts.
An insufficient buffer returns the documented PC insufficient-buffer HRESULT.
The directory is also available through confined native absolute read paths;
DOS aliases continue to select assets. This read binding grants no write access.

Directory/file creation and write APIs require separate storage-confined host
operations. Returning a valid path does not claim those APIs, storage quotas,
space reservation, save-game support or crash durability are implemented.

Diagnostic boot currently selects a directory in the existing sandbox's writable
scratch. It is destroyed with that sandbox and is not a cross-launch persistence
test. The backend otherwise uses the explicitly selected existing filesystem
root. Original read-only inputs, credentials/network/exec hiding and resource
bounds remain in force; this feature does not authorize arbitrary host mounts.
