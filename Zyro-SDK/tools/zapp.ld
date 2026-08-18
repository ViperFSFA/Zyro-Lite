/*  Zyro-SDK linker script for .zApp binaries.
 * 
 * These addresses are NOT real memory. a fixed, made-up layout
 * used only so the linker can fully resolve every internal pointer at
 * build time. pack_zapp.py then reads --emit-relocs relocation records to
 * work out, for every absolute pointer left in the binary, which of the
 * three sections it pointed into and at what offset. and turns that into
 * a ZappReloc entry the firmware re-applies wherever it actually put the
 * section in RAM at load time. Direct branches/calls within a section need
 * no such fixup: they're PC-relative and stay correct as long as the whole
 * section moves as one contiguous block, which the loader guarantees.
 *
 * 1MB spacing between regions guarantees the three ranges
 * can never be confused with each other when pack_zapp.py classifies a
 * resolved address. it has nothing to do with actual RAM usage. The real
 * size caps (ZAPP_TEXT_MAX / ZAPP_RODATA_DATA_MAX) are enforced by
 * pack_zapp.py against the firmware's own include/config.h values, and
 * re-checked by the firmware itself at load time. */

MEMORY
{
    TEXT   (rx)  : ORIGIN = 0x00000000, LENGTH = 0x00100000
    RODATA (r)   : ORIGIN = 0x00100000, LENGTH = 0x00100000
    DATA   (rw)  : ORIGIN = 0x00200000, LENGTH = 0x00100000
}

SECTIONS
{
    .text : {
        *(.literal .literal.*)
        *(.iram0.literal .iram0.literal.*)
        *(.text .text.*)
        *(.iram0.text .iram0.text.*)
    } > TEXT

    .rodata : {
        *(.rodata .rodata.*)
        *(.rodata1)
    } > RODATA

    .data : {
        *(.data .data.*)
        *(.sdata .sdata.*)
    } > DATA

    .bss (NOLOAD) : {
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
    } > DATA

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.xt.prop*)
        *(.xt.lit*)
        *(.debug*)
    }
}

ENTRY(zapp_entry)
