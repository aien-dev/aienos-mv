/* apple_image.ld -- layout of the CK_APPLE=1 raw arm64 Image (Linux arm64
 * boot protocol, Documentation/arch/arm64/booting.rst section 4 "Call the
 * kernel image"): a 64-byte header at offset 0, then code. Linked position
 * independent at 0; apple_entry.S applies its own R_AARCH64_RELATIVE
 * relocations against the load address (the loader may place the image at
 * any 2 MiB-aligned base, flags bit 3).
 *
 *   u32 code0        b _apple_start
 *   u32 code1        0
 *   u64 text_offset  0
 *   u64 image_size   __image_end (file bytes + bss, 4 KiB rounded)
 *   u64 flags        0xa: little endian, 4K kernel pages, placement anywhere
 *   u64 res2..res4   0
 *   u32 magic        0x644d5241 "ARM\x64"
 *   u32 res5         0 (no PE header: this image is not a UEFI application) */
OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_apple_start)

SECTIONS
{
    . = 0;
    __image_base = .;
    .head : {
        LONG(0x14000000 | ((_apple_start - __image_base) >> 2))
        LONG(0)
        QUAD(0)
        QUAD(__image_end - __image_base)
        QUAD(0xa)
        QUAD(0) QUAD(0) QUAD(0)
        LONG(0x644d5241)
        LONG(0)
    }

    . = ALIGN(4096);
    __text_start = .;
    .text : { KEEP(*(.text.apple_entry)) *(.text .text.*) }
    . = ALIGN(4096);
    __text_end = .;

    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(8);
    .rela.dyn : { __rela_start = .; *(.rela .rela.*) __rela_end = .; }
    .dynamic : { *(.dynamic) }
    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }
    .hash : { *(.hash) }
    .gnu.hash : { *(.gnu.hash) }
    .data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) }
    .got : { *(.got) *(.got.plt) }
    .data : { *(.data .data.*) }
    . = ALIGN(4096);
    __data_file_end = .;
    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }
    . = ALIGN(4096);
    __image_end = .;

    /DISCARD/ : { *(.interp) *(.comment) *(.note*) *(.eh_frame*) *(.plt) }
}
