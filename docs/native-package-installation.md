# Local package installation readiness

The native runtime can bind an explicit expected file inventory to its validated
process package identity. This supplies readiness evidence for the offline loose
package. It does not authenticate payloads, install anything or enable downloads.
The installation-monitor guest API is still unsupported.

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

Until chunk selection and launch-marker metadata are implemented, any consumer
must conservatively require completion of the whole inventory for launch. No
per-chunk availability, callbacks, installation-monitor scheduling, DLC or license
state follows from these observations.
