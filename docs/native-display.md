# Native display output policy

The native GDK display object provides its known interface and counted IUnknown
prefix. `XDisplayTryEnableHdrMode` uses the documented Windows ABI, preferences
and result values. Interface slot 3 remains an explicit unsupported call.

Run modes explicitly select the current SDR-only software output policy before
guest initialization. A valid HDR preference then returns Disabled. Without a
selected policy, or with an invalid preference, the result is Unknown. Selection
occurs once per runtime; policies and interface identities are independent.

This is a software output choice. It creates no window, changes no monitor mode
and queries no host HDR, resolution or refresh-rate data. The implementation has
no HDR-enable path. The optional luminance buffer remains untouched because the
API only supplies luminance information when HDR is Enabled. No nits or display
measurements are invented.

Negotiation requires an ordinary active guest thread and an object owned by its
current runtime. Host errno and guest error state are preserved. The original
strict CPU/thread sandbox profiles and their device restrictions remain intact.
A future HDR backend needs real presentation support and valid enabled-mode
metadata before that result can be offered.

Original fixtures cover policy binding, result values, optional/packed output
preservation, independent identities and the actual native-command-to-PE path.
Actual game startup is tested separately. A completed HDR fallback does not
establish a presented frame or interactive menu.
