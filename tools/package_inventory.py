"""Convert expected package size metadata and an identity descriptor to a local inventory.

This reads metadata only. It does not scan or hash asset payloads. Inventories
derived from a supplied package descriptor belong in the user's private work
directory, outside Git. Standard library only; no child processes or networking.
"""
import argparse
import json
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parent))
from report_paths import same_report_path

METADATA_LIMIT = 512 * 1024


def read_metadata(path):
    with path.open('rb') as stream:
        content = stream.read(METADATA_LIMIT + 1)
    if not content or len(content) > METADATA_LIMIT:
        raise ValueError('metadata is empty or exceeds 512 KiB')
    return content


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate manifest field')
        result[key] = value
    return result


def identity_from_descriptor(content):
    normalized = content.upper().replace(b'\0', b'')
    if b'<!DOCTYPE' in normalized or b'<!ENTITY' in normalized:
        raise ValueError('DTD and entity declarations are not supported')
    root = ET.fromstring(content)
    local_name = lambda tag: tag.rsplit('}', 1)[-1]
    if local_name(root.tag) not in ('Game', 'Package'):
        raise ValueError('expected a game or appx identity descriptor')
    if sum(1 for _ in root.iter()) > 2048:
        raise ValueError('descriptor element count exceeds its bound')
    identities = [node for node in root if local_name(node.tag) == 'Identity']
    if len(identities) != 1:
        raise ValueError('expected exactly one direct identity')
    identity = {key: identities[0].get(key, '') for key in ('Name', 'Publisher', 'Version')}
    if not re.fullmatch(r'[A-Za-z0-9.-]{1,32}', identity['Name']):
        raise ValueError('invalid package identifier')
    if not 1 <= len(identity['Publisher'].encode('utf-8')) <= 256:
        raise ValueError('invalid publisher length')
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+', identity['Version']):
        raise ValueError('expected a four-component version')
    if any(int(part) > 65535 for part in identity['Version'].split('.')):
        raise ValueError('version component exceeds uint16')
    return identity


def create_inventory(manifest_content, descriptor_content):
    manifest = json.loads(manifest_content, object_pairs_hook=unique_object)
    identity = identity_from_descriptor(descriptor_content)
    if not isinstance(manifest, dict) or manifest.get('version') != identity['Version']:
        raise ValueError('manifest and descriptor versions must agree exactly')
    files = manifest.get('files')
    if not isinstance(files, dict) or not 1 <= len(files) <= 1024:
        raise ValueError('expected 1..1024 file requirements')
    root = ET.Element('Package')
    ET.SubElement(root, 'Identity', identity)
    inventory = ET.SubElement(root, 'Inventory')
    names = set()
    total = 0
    for path, metadata in sorted(files.items()):
        if not isinstance(metadata, dict):
            raise ValueError('file metadata must be an object')
        size = metadata.get('size')
        if type(size) is not int or not 0 <= size <= (1 << 63) - 1:
            raise ValueError('expected an unsigned file size in the signed-offset range')
        if not path or len(path.encode('utf-8')) > 1024:
            raise ValueError('invalid relative path length')
        if any(ord(unit) < 32 or ord(unit) == 127 or unit in '\\:<>"|?*' for unit in path):
            raise ValueError('invalid relative path character')
        components = path.split('/')
        if len(components) > 64 or any(not part or part in ('.', '..') or
                                       part.endswith(('.', ' ')) for part in components):
            raise ValueError('expected a canonical relative path')
        folded = path.translate(str.maketrans('ABCDEFGHIJKLMNOPQRSTUVWXYZ',
                                             'abcdefghijklmnopqrstuvwxyz'))
        if folded in names:
            raise ValueError('duplicate file path under ASCII case fallback')
        names.add(folded)
        total += size
        if total > (1 << 63) - 1:
            raise ValueError('aggregate expected bytes exceed the supported range')
        ET.SubElement(inventory, 'File', {'Path': path, 'Size': str(size)})
    ET.indent(root, space='  ')
    result = ET.tostring(root, encoding='unicode') + '\n'
    if len(result.encode('utf-8')) > METADATA_LIMIT:
        raise ValueError('generated inventory exceeds 512 KiB')
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--descriptor', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    arguments = parser.parse_args(argv)
    try:
        if any(same_report_path(arguments.output, source)
               for source in (arguments.manifest, arguments.descriptor)):
            raise ValueError('output must differ from every input')
        content = create_inventory(read_metadata(arguments.manifest), read_metadata(arguments.descriptor))
        with arguments.output.open('x', encoding='utf-8', newline='\n') as stream:
            stream.write(content)
    except (OSError, ValueError, ET.ParseError, TypeError) as error:
        print(f'package inventory: conversion failed ({type(error).__name__})', file=sys.stderr)
        return 1
    print('Wrote local metadata inventory; payload integrity has not been verified.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
