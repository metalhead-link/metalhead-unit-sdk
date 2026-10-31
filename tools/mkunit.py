#!/usr/bin/env python3
"""Convert a unit ELF (linked with unit.ld, -pie) into a loadable .unt file.

The .unt layout is described in include/unit_abi.h:
    UnitFileHeader (64 bytes) | image | relocation offsets (uint32 each)

32-bit ARM (BeagleBone Black) and 64-bit ARM (Raspberry Pi 5) units; the file's machine
field says which. Only relative relocations are allowed (R_ARM_RELATIVE, R_AARCH64_RELATIVE):
anything else means the unit references a symbol that isn't in the image (e.g. a C library
function), which the loader can't resolve. The loader adds the load address to the word at
each listed offset, so for AArch64 (RELA, the value is in the relocation) the value is written
into the image first.
"""
import struct
import sys
import zlib

UNIT_FILE_MAGIC = 0x31544E55     # "UNT1"
UNIT_IMAGE_MAGIC = 0x54494E55    # "UNIT"
UNIT_NAME_LENGTH = 32
UNIT_MACHINE_ARM32 = 0
UNIT_MACHINE_AARCH64 = 1
HEADER_FORMAT = '<IHHIIIIII32s'
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
assert HEADER_SIZE == 64

PT_LOAD = 1
SHT_SYMTAB = 2
SHT_RELA = 4
SHT_REL = 9
EM_ARM = 40
EM_AARCH64 = 183
R_ARM_NONE = 0
R_ARM_RELATIVE = 23
R_AARCH64_NONE = 0
R_AARCH64_RELATIVE = 1027


def fail(msg):
    sys.exit(f'mkunit: error: {msg}')


class Elf:
    def __init__(self, data):
        if data[:4] != b'\x7fELF' or data[5] != 1:
            fail('not a little-endian ELF file')
        self.data = data
        self.is64 = data[4] == 2
        if self.is64:
            (e_type, e_machine, _, _, e_phoff, e_shoff, _, _, e_phentsize, e_phnum,
             e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from('<HHIQQQIHHHHHH', data, 16)
        else:
            (e_type, e_machine, _, _, e_phoff, e_shoff, _, _, e_phentsize, e_phnum,
             e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from('<HHIIIIIHHHHHH', data, 16)
        if (e_machine, self.is64) == (EM_ARM, False):
            self.machine, self.word = UNIT_MACHINE_ARM32, 4
        elif (e_machine, self.is64) == (EM_AARCH64, True):
            self.machine, self.word = UNIT_MACHINE_AARCH64, 8
        else:
            fail('not a 32-bit ARM or 64-bit ARM ELF file')
        if e_type != 3:  # ET_DYN
            fail('not position independent (link with -pie and unit.ld)')

        self.segments = []
        for i in range(e_phnum):
            off = e_phoff + i * e_phentsize
            if self.is64:
                p_type, _, p_offset, p_vaddr, _, p_filesz, p_memsz, _ = struct.unpack_from('<IIQQQQQQ', data, off)
            else:
                p_type, p_offset, p_vaddr, _, p_filesz, p_memsz, _, _ = struct.unpack_from('<IIIIIIII', data, off)
            if p_type == PT_LOAD:
                self.segments.append((p_offset, p_vaddr, p_filesz, p_memsz))

        self.sections = []
        for i in range(e_shnum):
            off = e_shoff + i * e_shentsize
            if self.is64:
                name, stype, _, addr, offset, size, link, _, _, entsize = struct.unpack_from('<IIQQQQIIQQ', data, off)
            else:
                name, stype, _, addr, offset, size, link, _, _, entsize = struct.unpack_from('<IIIIIIIIII', data, off)
            self.sections.append(dict(name=name, type=stype, addr=addr, offset=offset, size=size,
                                      link=link, entsize=entsize))
        strtab = self.sections[e_shstrndx]
        for s in self.sections:
            s['name'] = self.string(strtab, s['name'])

    def string(self, table, index):
        start = table['offset'] + index
        return self.data[start:self.data.index(b'\0', start)].decode()

    def section(self, name):
        return next((s for s in self.sections if s['name'] == name), None)

    def symbols(self, section):
        """(name, value) of each entry in a symbol table section"""
        strs = self.sections[section['link']]
        size = 24 if self.is64 else 16
        for off in range(section['offset'], section['offset'] + section['size'], size):
            if self.is64:
                st_name, _, _, _, st_value, _ = struct.unpack_from('<IBBHQQ', self.data, off)
            else:
                st_name, st_value, _, _, _, _ = struct.unpack_from('<IIIBBH', self.data, off)
            yield self.string(strs, st_name), st_value

    def relocations(self):
        """(offset, type, symbol index, addend or None) of every dynamic relocation"""
        for s in self.sections:
            if s['type'] == SHT_REL and not self.is64:
                for off in range(s['offset'], s['offset'] + s['size'], 8):
                    r_offset, r_info = struct.unpack_from('<II', self.data, off)
                    yield r_offset, r_info & 0xff, r_info >> 8, None
            elif s['type'] == SHT_RELA and self.is64:
                for off in range(s['offset'], s['offset'] + s['size'], 24):
                    r_offset, r_info, r_addend = struct.unpack_from('<QQq', self.data, off)
                    yield r_offset, r_info & 0xffffffff, r_info >> 32, r_addend
            elif s['type'] in (SHT_REL, SHT_RELA) and s['size']:
                fail(f'unexpected relocation section {s["name"]}')


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: mkunit.py unit.elf unit.unt')
    elf = Elf(open(sys.argv[1], 'rb').read())
    data = elf.data
    if not elf.segments:
        fail('no loadable segments')

    # Flatten all loadable segments into one image starting at address 0.
    file_end = max(vaddr + filesz for _, vaddr, filesz, _ in elf.segments)
    mem_end = max(vaddr + memsz for _, vaddr, _, memsz in elf.segments)
    if min(vaddr for _, vaddr, _, _ in elf.segments) != 0:
        fail('image must be linked at address 0 (use unit.ld)')
    image = bytearray(file_end)
    for offset, vaddr, filesz, _ in elf.segments:
        image[vaddr:vaddr + filesz] = data[offset:offset + filesz]
    image_size = (file_end + elf.word - 1) & ~(elf.word - 1)
    image.extend(b'\0' * (image_size - file_end))
    bss_size = max(0, mem_end - image_size)

    # Nothing runs static constructors (C++ globals with constructors, __attribute__((constructor)))
    for s in elf.sections:
        if s['size'] and s['name'] in ('.init_array', '.preinit_array', '.ctors'):
            fail(f'{s["name"]} is not empty: units have no static constructors, '
                 f'initialise globals in unit_entry() or createInstance()')

    # Relocations
    none, relative = ((R_ARM_NONE, R_ARM_RELATIVE) if elf.machine == UNIT_MACHINE_ARM32
                      else (R_AARCH64_NONE, R_AARCH64_RELATIVE))
    relocs = []
    for r_offset, r_type, r_sym, r_addend in elf.relocations():
        if r_type == none:
            continue
        if r_type != relative:
            dynsym = elf.section('.dynsym')
            names = [n for n, _ in elf.symbols(dynsym)] if dynsym else []
            fail(f'unsupported relocation type {r_type} at 0x{r_offset:x} '
                 f'(symbol "{names[r_sym] if r_sym < len(names) else "?"}"): units may only use '
                 f'code and data from their own image and the UnitHostApi')
        if r_offset % elf.word or r_offset + elf.word > image_size:
            fail(f'relocation at 0x{r_offset:x} outside the image')
        if r_addend is not None:
            struct.pack_into('<Q', image, r_offset, r_addend)
        relocs.append(r_offset)
    relocs.sort()

    symtab = next((s for s in elf.sections if s['type'] == SHT_SYMTAB), None)
    entry = dict(elf.symbols(symtab)).get('unit_entry') if symtab else None
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
                         len(relocs), entry, zlib.crc32(body), elf.machine, name)
    with open(sys.argv[2], 'wb') as f:
        f.write(header + body)
    machine = 'arm32' if elf.machine == UNIT_MACHINE_ARM32 else 'aarch64'
    print(f'{sys.argv[2]}: {type_name.decode()} ABI {abi} {machine}, image {image_size} bytes, '
          f'bss {bss_size} bytes, {len(relocs)} relocations, entry 0x{entry:x}')


if __name__ == '__main__':
    main()
