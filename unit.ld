/*
 * Linker script for loadable units: one position-independent image linked at address 0,
 * starting with the UnitImageHeader. The loader copies the file-backed part, zeroes the rest
 * (bss) and applies the relative relocations that mkunit.py extracts from .rel(a).dyn.
 */
ENTRY(unit_entry)

SECTIONS
{
    . = 0;

    .unit_header : { KEEP(*(.unit_header)) }

    .text : {
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    . = ALIGN(8);
    .data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) }
    .got : { *(.got.plt) *(.got) }
    .data : { *(.data .data.*) }

    /* Dynamic-linking bookkeeping ends up in the image but is never used at runtime. */
    .dynamic : { *(.dynamic) }
    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }
    .hash : { *(.hash) }
    .gnu.hash : { *(.gnu.hash) }
    .rel.dyn : { *(.rel.dyn) *(.rel.*) }
    .rela.dyn : { *(.rela.dyn) *(.rela.*) }

    . = ALIGN(8);
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }
    . = ALIGN(8);

    /DISCARD/ : {
        *(.interp)
        *(.comment)
        *(.note .note.*)
        *(.ARM.exidx .ARM.exidx.*)
        *(.ARM.extab .ARM.extab.*)
        *(.ARM.attributes)
    }
}
