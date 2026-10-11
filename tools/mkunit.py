#!/usr/bin/env python3
"""Convert a unit ELF (linked with unit.ld, -pie) into a loadable .unt file.

The .unt layout is described in ccs/units/include/unit_abi.h:
    UnitFileHeader (64 bytes) | image | relocation offsets (uint32 each)

Only R_ARM_RELATIVE relocations are allowed: anything else means the unit references a
symbol that isn't in the image (e.g. a C library function), which the loader can't resolve.
"""
import struct
import sys
import zlib

UNIT_FILE_MAGIC = 0x31544E55     # "UNT1"
UNIT_IMAGE_MAGIC = 0x54494E55    # "UNIT"
UNIT_NAME_LENGTH = 32
HEADER_FORMAT = '<IHHIIIIII32s'
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
assert HEADER_SIZE == 64

PT_LOAD = 1
SHT_SYMTAB = 2
SHT_RELA = 4
SHT_REL = 9
R_ARM_NONE = 0
R_ARM_RELATIVE = 23
EM_ARM = 40


def fail(msg):
    sys.exit(f'mkunit: error: {msg}')


def read_elf(data):
    if data[:4] != b'\x7fELF' or data[4] != 1 or data[5] != 1:
        fail('not a 32-bit little-endian ELF file')
    (e_type, e_machine, _, _, e_phoff, e_shoff, _, _, e_phentsize, e_phnum,
     e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from('<HHIIIIIHHHHHH', data, 16)
    if e_machine != EM_ARM:
        fail('not an ARM ELF file')
    if e_type != 3:  # ET_DYN
        fail('not position independent (link with -pie and unit.ld)')

    segments = []
    for i in range(e_phnum):
        p_type, p_offset, p_vaddr, _, p_filesz, p_memsz, _, _ = struct.unpack_from(
            '<IIIIIIII', data, e_phoff + i * e_phentsize)
        if p_type == PT_LOAD:
            segments.append((p_offset, p_vaddr, p_filesz, p_memsz))

    sections = []
    for i in range(e_shnum):
        sh = struct.unpack_from('<IIIIIIIIII', data, e_shoff + i * e_shentsize)
        sections.append(dict(name=sh[0], type=sh[1], addr=sh[3], offset=sh[4], size=sh[5],
                             link=sh[6], entsize=sh[9]))
    strtab = sections[e_shstrndx]

    def section_name(s):
        start = strtab['offset'] + s['name']
        return data[start:data.index(b'\0', start)].decode()

    for s in sections:
        s['name'] = section_name(s)
    return segments, sections


def symbols(data, sections):
    for s in sections:
        if s['type'] != SHT_SYMTAB:
            continue
        strs = sections[s['link']]
        for off in range(s['offset'], s['offset'] + s['size'], 16):
            st_name, st_value, st_size, st_info, _, st_shndx = struct.unpack_from('<IIIBBH', data, off)
            start = strs['offset'] + st_name
            yield data[start:data.index(b'\0', start)].decode(), st_value


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: mkunit.py unit.elf unit.unt')
    data = open(sys.argv[1], 'rb').read()
    segments, sections = read_elf(data)
    if not segments:
        fail('no loadable segments')

    # Flatten all loadable segments into one image starting at address 0.
    file_end = max(vaddr + filesz for _, vaddr, filesz, _ in segments)
    mem_end = max(vaddr + memsz for _, vaddr, _, memsz in segments)
    if min(vaddr for _, vaddr, _, _ in segments) != 0:
        fail('image must be linked at address 0 (use unit.ld)')
    image = bytearray(file_end)
    for offset, vaddr, filesz, _ in segments:
        image[vaddr:vaddr + filesz] = data[offset:offset + filesz]
    image_size = (file_end + 3) & ~3
    image.extend(b'\0' * (image_size - file_end))
    bss_size = max(0, mem_end - image_size)

    # Relocations
    relocs = []
    sym_names = None
    for s in sections:
        if s['type'] == SHT_RELA:
            fail(f'unexpected RELA section {s["name"]}')
        if s['type'] != SHT_REL:
            continue
        for off in range(s['offset'], s['offset'] + s['size'], 8):
            r_offset, r_info = struct.unpack_from('<II', data, off)
            r_type, r_sym = r_info & 0xff, r_info >> 8
            if r_type == R_ARM_NONE:
                continue
            if r_type != R_ARM_RELATIVE:
                if sym_names is None:
                    dynsym = next((x for x in sections if x['name'] == '.dynsym'), None)
                    dynstr = next((x for x in sections if x['name'] == '.dynstr'), None)
                    sym_names = {}
                    if dynsym and dynstr:
                        for i, o in enumerate(range(dynsym['offset'], dynsym['offset'] + dynsym['size'], 16)):
                            st_name = struct.unpack_from('<I', data, o)[0]
                            st = dynstr['offset'] + st_name
                            sym_names[i] = data[st:data.index(b'\0', st)].decode()
                fail(f'unsupported relocation type {r_type} at 0x{r_offset:x} '
                     f'(symbol "{sym_names.get(r_sym, "?")}"): units may only use code and data '
                     f'from their own image and the UnitHostApi')
            if r_offset % 4 or r_offset + 4 > image_size:
                fail(f'relocation at 0x{r_offset:x} outside the image')
            relocs.append(r_offset)
    relocs.sort()

    entry = dict(symbols(data, sections)).get('unit_entry')
    if entry is None:
        fail('unit_entry not defined')

    magic, abi, name = struct.unpack_from(f'<II{UNIT_NAME_LENGTH}s', image, 0)
    if magic != UNIT_IMAGE_MAGIC:
        fail('image does not start with a UnitImageHeader (missing UNIT_DECLARE?)')
    type_name = name.split(b'\0')[0]
    if not type_name or b'\0' not in name:
        fail('invalid unit name in UNIT_DECLARE')

    body = bytes(image) + struct.pack(f'<{len(relocs)}I', *relocs)
    header = struct.pack(HEADER_FORMAT, UNIT_FILE_MAGIC, abi, HEADER_SIZE, image_size, bss_size,
                         len(relocs), entry, zlib.crc32(body), 0, name)
    with open(sys.argv[2], 'wb') as f:
        f.write(header + body)
    print(f'{sys.argv[2]}: {type_name.decode()} ABI {abi}, image {image_size} bytes, '
          f'bss {bss_size} bytes, {len(relocs)} relocations, entry 0x{entry:x}')


if __name__ == '__main__':
    main()
