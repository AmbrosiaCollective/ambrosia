"""Original metadata-only converter checks; no asset inputs or subprocesses."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import package_inventory


class PackageInventoryTests(unittest.TestCase):
    descriptor = b"<Game><Identity Name='Example' Publisher='CN=Example' Version='1.2.3.4'/></Game>"

    def manifest(self, files):
        return json.dumps({'version': '1.2.3.4', 'files': files}).encode()

    def test_expected_sizes_and_identity_without_payload_access(self):
        content = package_inventory.create_inventory(
            self.manifest({'nested/caf\u00e9.bin': {'size': 12}, 'empty.bin': {'size': 0}}), self.descriptor)
        root = ET.fromstring(content)
        self.assertEqual(root.find('Identity').get('Name'), 'Example')
        self.assertEqual([node.attrib for node in root.find('Inventory')],
                         [{'Path': 'empty.bin', 'Size': '0'}, {'Path': 'nested/caf\u00e9.bin', 'Size': '12'}])
        self.assertTrue(content.endswith('\n'))
        self.assertNotIn('\r', content)

    def test_paths_sizes_duplicates_and_version_conflicts(self):
        for path in ('../a', '/a', 'a\\b', 'a//b', 'a/./b', 'C:/a', 'a*', 'a.', 'a\t'):
            with self.subTest(path=path), self.assertRaises(ValueError):
                package_inventory.create_inventory(self.manifest({path: {'size': 1}}), self.descriptor)
        for size in (-1, True, '1', 1 << 63):
            with self.subTest(size=size), self.assertRaises(ValueError):
                package_inventory.create_inventory(self.manifest({'a': {'size': size}}), self.descriptor)
        for content in (self.manifest({'a': {'size': 1}, 'A': {'size': 1}}),
                        b'{"version":"1.2.3.4","files":{"a":{"size":1,"size":2}}}',
                        b'{"version":"1.2.3.5","files":{"a":{"size":1}}}'):
            with self.assertRaises(ValueError):
                package_inventory.create_inventory(content, self.descriptor)

    def test_entities_empty_inventory_and_limits(self):
        for descriptor in (b'<!DOCTYPE Game [<!ENTITY x "Example">]>' + self.descriptor,
                           b'<!DOCTYPE Game SYSTEM "file:///not-an-input">' + self.descriptor):
            with self.assertRaises(ValueError):
                package_inventory.create_inventory(self.manifest({'a': {'size': 1}}), descriptor)
        for files in ({}, {f'f{index}': {'size': 0} for index in range(1025)},
                      {'a': {'size': (1 << 63) - 1}, 'b': {'size': 1}}):
            with self.assertRaises(ValueError):
                package_inventory.create_inventory(self.manifest(files), self.descriptor)

    def test_cli_refuses_existing_outputs_and_input_aliases(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = root / 'expected.json'
            descriptor = root / 'descriptor.xml'
            output = root / 'inventory.xml'
            original = self.manifest({'a': {'size': 1}})
            manifest.write_bytes(original)
            descriptor.write_bytes(self.descriptor)
            arguments = ['--manifest', str(manifest), '--descriptor', str(descriptor), '--output']
            self.assertEqual(package_inventory.main(arguments + [str(output)]), 0)
            before = output.read_bytes()
            self.assertEqual(package_inventory.main(arguments + [str(output)]), 1)
            self.assertEqual(output.read_bytes(), before)
            self.assertEqual(package_inventory.main(arguments + [str(manifest)]), 1)
            self.assertEqual(manifest.read_bytes(), original)
            with self.assertRaises(ValueError):
                too_large = root / 'too-large.json'
                too_large.write_bytes(b' ' * (package_inventory.METADATA_LIMIT + 1))
                package_inventory.read_metadata(too_large)


if __name__ == '__main__':
    unittest.main()
