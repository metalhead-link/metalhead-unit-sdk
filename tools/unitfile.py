#!/usr/bin/env python3
"""A .unt file's resources (include/unit_abi.h): the faceplates it carries after its code.

    unitfile.py list unit.unt                          what the file is and the resources it has
    unitfile.py add unit.unt Plugin=face.html ...      add or replace plugins' faceplates
    unitfile.py faceplate unit.unt Plugin [out.html]   a faceplate's HTML (to stdout without out)
    unitfile.py strip unit.unt                         remove the resources

add and strip rewrite the file in place and update its crc32. Faceplates are stored gzip
compressed, and are at most 256 KB each before compression (doc/faceplates.md).

Also a module: mkunit.py and metalhead-essentials' build-store.py use read(), write() and
with_faceplates().
"""
import gzip
import struct
import sys
import zlib

UNIT_FILE_MAGIC = 0x31544E55        # "UNT1"
HEADER_SIZE = 64
UNIT_NAME_LENGTH = 32
RESOURCES_MAGIC = 0x53455255        # "URES"
RESOURCES_MAX = 64
RESOURCE_MAX_SIZE = 256 * 1024
TABLE_FORMAT = '<II'
ENTRY_FORMAT = f'<IIII{UNIT_NAME_LENGTH}s'
TABLE_SIZE = struct.calcsize(TABLE_FORMAT)
ENTRY_SIZE = struct.calcsize(ENTRY_FORMAT)
assert (TABLE_SIZE, ENTRY_SIZE) == (8, 48)

FACEPLATE = 1
KINDS = {FACEPLATE: 'faceplate'}
MAX_FACEPLATE = 256 * 1024          # the HTML, before compression


class UnitFileError(Exception):
    pass


def read(data):
    """(code, resources): the file up to its relocations, and [(kind, plugin, bytes)]"""
    if len(data) < HEADER_SIZE:
        raise UnitFileError('too short for a unit file')
    magic, _, header_size, image_size, _, reloc_count, _, crc, _, name = struct.unpack_from(
        f'<IHHIIIIII{UNIT_NAME_LENGTH}s', data, 0)
    if magic != UNIT_FILE_MAGIC or header_size != HEADER_SIZE:
        raise UnitFileError('not a unit file')
    end = HEADER_SIZE + image_size + 4 * reloc_count
    if end > len(data):
        raise UnitFileError('truncated')
    if zlib.crc32(data[HEADER_SIZE:]) != crc:
        raise UnitFileError('checksum mismatch')
    resources = []
    if end < len(data):
        table = data[end:]
        if len(table) < TABLE_SIZE:
            raise UnitFileError('resources: truncated table')
        rmagic, count = struct.unpack_from(TABLE_FORMAT, table, 0)
        if rmagic != RESOURCES_MAGIC or count > RESOURCES_MAX or TABLE_SIZE + count * ENTRY_SIZE > len(table):
            raise UnitFileError('resources: invalid table')
        for i in range(count):
            kind, offset, size, _, plugin = struct.unpack_from(ENTRY_FORMAT, table, TABLE_SIZE + i * ENTRY_SIZE)
            if b'\0' not in plugin or not plugin.split(b'\0')[0]:
                raise UnitFileError(f'resources: entry {i} has no plugin name')
            if offset < TABLE_SIZE + count * ENTRY_SIZE or size > RESOURCE_MAX_SIZE or offset + size > len(table):
                raise UnitFileError(f'resources: entry {i} lies outside the file')
            resources.append((kind, plugin.split(b'\0')[0].decode(), bytes(table[offset:offset + size])))
    return bytes(data[:end]), resources


def write(code, resources):
    """The unit file: code (as read() returns it) with these resources, and its crc32 updated"""
    if len(resources) > RESOURCES_MAX:
        raise UnitFileError(f'more than {RESOURCES_MAX} resources')
    tail = b''
    if resources:
        offset = TABLE_SIZE + len(resources) * ENTRY_SIZE
        entries, blobs = [], []
        for kind, plugin, blob in resources:
            name = plugin.encode()
            if not name or len(name) >= UNIT_NAME_LENGTH:
                raise UnitFileError(f'plugin name "{plugin}" must be 1..{UNIT_NAME_LENGTH - 1} bytes')
            if len(blob) > RESOURCE_MAX_SIZE:
                raise UnitFileError(f'{plugin}: a resource is at most {RESOURCE_MAX_SIZE} bytes')
            entries.append(struct.pack(ENTRY_FORMAT, kind, offset, len(blob), 0, name))
            blobs.append(blob)
            offset += len(blob)
        tail = struct.pack(TABLE_FORMAT, RESOURCES_MAGIC, len(resources)) + b''.join(entries) + b''.join(blobs)
    body = code[HEADER_SIZE:] + tail
    return code[:24] + struct.pack('<I', zlib.crc32(body)) + code[28:HEADER_SIZE] + body


def pack_faceplate(plugin, html):
    """A faceplate's HTML (bytes) as stored: gzip, with no timestamp so builds are reproducible"""
    if len(html) > MAX_FACEPLATE:
        raise UnitFileError(f'{plugin}: the faceplate is {len(html)} bytes; the limit is {MAX_FACEPLATE}')
    return gzip.compress(html, compresslevel=9, mtime=0)


def with_faceplates(data, faceplates):
    """The unit file with these faceplates ({plugin: HTML bytes}) added or replaced"""
    code, resources = read(data)
    resources = [r for r in resources if not (r[0] == FACEPLATE and r[1] in faceplates)]
    resources += [(FACEPLATE, plugin, pack_faceplate(plugin, html)) for plugin, html in faceplates.items()]
    return write(code, resources)


def faceplate(data, plugin):
    """A plugin's faceplate HTML (bytes), or None"""
    for kind, name, blob in read(data)[1]:
        if kind == FACEPLATE and name == plugin:
            return gzip.decompress(blob)
    return None


def main(argv):
    usage = __doc__.split('\n\n')[1]
    if len(argv) < 2:
        sys.exit(f'usage:\n{usage}')
    command, path, rest = argv[0], argv[1], argv[2:]
    data = open(path, 'rb').read()
    if command == 'list' and not rest:
        code, resources = read(data)
        name = code[32:64].split(b'\0')[0].decode()
        print(f'{path}: {name}, code {len(code)} bytes, {len(resources)} resources')
        for kind, plugin, blob in resources:
            extra = f' ({len(gzip.decompress(blob))} bytes unpacked)' if kind == FACEPLATE else ''
            print(f'  {KINDS.get(kind, f"kind {kind}")} {plugin}: {len(blob)} bytes{extra}')
    elif command == 'add' and rest:
        faceplates = {}
        for arg in rest:
            plugin, sep, file = arg.partition('=')
            if not sep:
                sys.exit(f'unitfile: expected Plugin=file.html, not {arg}')
            faceplates[plugin] = open(file, 'rb').read()
        out = with_faceplates(data, faceplates)
        open(path, 'wb').write(out)
        print(f'{path}: {len(out)} bytes, faceplates for {", ".join(faceplates)}')
    elif command == 'faceplate' and len(rest) in (1, 2):
        html = faceplate(data, rest[0])
        if html is None:
            sys.exit(f'unitfile: {path} has no faceplate for {rest[0]}')
        if len(rest) == 2:
            open(rest[1], 'wb').write(html)
        else:
            sys.stdout.buffer.write(html)
    elif command == 'strip' and not rest:
        open(path, 'wb').write(write(read(data)[0], []))
    else:
        sys.exit(f'usage:\n{usage}')


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except UnitFileError as e:
        sys.exit(f'unitfile: error: {e}')
