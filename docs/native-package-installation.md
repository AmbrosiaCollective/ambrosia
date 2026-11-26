# Local package installation readiness

The native runtime can bind an explicit expected file inventory to its validated
process package identity. This supplies readiness evidence for the offline loose
package. It does not authenticate payloads, install anything or enable downloads.
The native console interface supports whole-package monitor creation, close,
cached progress and manual refresh. This is diagnostic startup work; an
interactive game menu has not been verified.

Generate private metadata from the public expected-size manifest and your local
identity descriptor:

```sh
python3 -I tools/package_inventory.py --manifest game/xbox-1.0.2.1.json \
  --descriptor "$PACKAGE/MicrosoftGame.Config" --output "$WORK/package-inventory.xml"
```

Pass `--package-inventory "$WORK/package-inventory.xml"` to a native run or
startup trace. It is optional, accepted only in run modes and never inferred from
the working directory. Without an inventory, package identity alone supplies no
installation-readiness claim. Keep descriptor-derived inventories outside Git.
The tool refuses existing outputs and input/output aliases. Inspect executable
paths and use the reviewed isolation appropriate to your workload.

The original metadata format is UTF8 or UTF16 XML. This synthetic example is not
a package descriptor copied from the game:

```xml
<Package>
  <Identity Name="Ambrosia.Example" Publisher="CN=Example" Version="1.2.3.4"/>
  <Inventory>
    <File Path="content/example.bin" Size="12"/>
    <File Path="empty.bin" Size="0"/>
  </Inventory>
</Package>
```

Name, publisher and all four version components must match the already bound
process identity. One immutable inventory can be bound per process runtime. The
parser rejects DTDs, entities, unknown structure, traversal, absolute paths,
wildcards and duplicate paths under the namespace's ASCII case fallback. Limits
are 512 KiB of metadata, 1,024 expected files, 1,024 UTF8 bytes and 64 components
per path, and a signed 64-bit aggregate byte total. Existing descriptor parser
bounds also apply. The converter checks the manifest's version against the local
descriptor; the caller remains responsible for selecting the expected manifest
for that package. Sizes are expected values, never learned from asset contents.

Observations use the shared confined host filesystem adapter. Each required path
must resolve to a regular file of exactly the expected logical size. Missing,
short, oversized and substituted directory entries are incomplete. An absent
zero-byte file also prevents completion. Only exact-size files contribute to
installed-byte totals. Unsafe indirection and host inspection failures fail
explicitly. Checks retain confined directory references, consume no guest asset
leases and re-read metadata on every observation. They are sequential checks,
not an atomic filesystem snapshot or a hash verification.

Monitor progress conservatively requires completion of the whole inventory for
launch. It reports the expected logical total, exact-size installed-file bytes,
the total as the launch requirement and separate launchable/completed flags.
Zero-byte required files still affect those flags. There is no per-chunk or
launch-marker interpretation yet.

A zero update interval creates a snapshot that changes only on manual refresh.
A nonzero interval requires a valid task queue with thread-pool work mode. The
native adapter retains that queue after its public handle closes and periodically
rechecks the source using the existing two-worker pool. The interval is a minimum
delay between refreshes; busy work queues can run them later. Closing a monitor
cancels future updates. Failed inspection invalidates progress explicitly;
stale cached completion is not returned after a known failure. Handles are opaque,
never reused and limited to 128 live monitors per runtime. Pending timers share
the existing 1,024-job bound and do not delay runtime shutdown.

Selectors, progress callback registration/delivery, DLC, licensing, downloads and
other package methods remain unsupported. No guest callback is run through this
native scheduler. Absence of a trusted inventory returns an unsupported result
when creating a monitor; identity metadata alone is insufficient.
