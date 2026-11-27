# Native window classes

`RegisterClassExA` registers owned class metadata in the current runtime process.
The x64 adapter copies the 80-byte structure with packed-pointer support, validates
its size and extra-byte counts, and retains the callback's mapped module catalog.
Registration never invokes that callback or creates a host window.

Each class owns its name, optional menu name or numeric resource ID, zeroed class
extra bytes and the supplied attributes. Procedure/instance, icon, cursor, brush,
style and extra-window metadata remain available to later window creation.
Resource handles are guest metadata, not claims of native resource support.

ASCII class names compare without case, with a 256-byte limit. Non-ASCII identity
comparison stops explicitly until Windows-compatible Unicode atom handling is
provided. The process ANSI codepage remains UTF-8; no lossy conversion is used.
Each name has a class atom shared by its local/global registrations. Local classes
are scoped by instance; lookup tries that instance before an application global
class. Duplicate registration returns zero with `ERROR_CLASS_ALREADY_EXISTS`.
The registry allows 128 class records and up to 40 extra bytes per class/window.

Aliases include `user32.dll` and the measured runtime API set. Valid registration
preserves host errno, guest errno and LastError. Missing module context or an
unmapped procedure/instance stops explicitly. A callback catalog remains pinned
with its record; future dispatch must restore the correct guest ABI and context.
Unregistration, system classes and native window/message presentation are still
separate work. A registered atom is not proof of a rendered frame or game menu.
