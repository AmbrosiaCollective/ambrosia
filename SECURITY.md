# Reporting security concerns

For a suspected vulnerability, contact a maintainer privately through the
[Ambrosia community](https://discord.gg/xEaVQtW5jH) and request a private reporting
route. Do not post exploit details, credentials or private paths in a public
channel or issue. If direct contact is unavailable, ask for a private contact
without including technical details.

Include the affected revision, platform, impact and a minimal synthetic
reproduction. Do not send game files, keys, personal data or an unredacted
environment dump. Acknowledge any limits in the reproduction.

Ambrosia is under development and has no supported release series yet. A report
will be assessed against its exact revision and the current development branch;
there is no published response-time guarantee. Report vulnerabilities in an
external dependency to that dependency's maintainers as appropriate, and tell
Ambrosia which component is affected.

The contributor runner executes repository code in-process; it is not a sandbox.
Inspect untrusted changes and use your own execution isolation. The offline
doctor discovers prerequisites and does not certify the security of installed
tools or a complete build.
