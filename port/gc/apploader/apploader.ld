/*
 * Link script for the mmgcport apploader (port/gc/apploader/apploader.c).
 * SPDX-License-Identifier: CC0-1.0
 *
 * The boot program copies the image (.text, .rodata, .data) from disc offset 0x2460 to 0x81200000
 * and calls the entry point recorded in the apploader header, which mkiso.py takes from this
 * ELF's e_entry. .bss follows the image and is cleared by apploader_entry. Everything must stay
 * below 0x81300000, where the IPL itself runs.
 */
OUTPUT_FORMAT("elf32-powerpc")
OUTPUT_ARCH(powerpc:common)
ENTRY(apploader_entry)

SECTIONS
{
    . = 0x81200000;
    .text : {
        KEEP(*(.text.entry))
        *(.text .text.*)
    }
    .rodata : {
        *(.rodata .rodata.*)
        *(.sdata2 .sdata2.*)
    }
    .data : {
        *(.data .data.*)
        *(.sdata .sdata.*)
        . = ALIGN(32);
    }
    __image_end = .;
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(32);
        __bss_end = .;
    }
    /DISCARD/ : {
        *(.comment)
        *(.eh_frame*)
        *(.note*)
        *(.gnu.attributes)
    }
}

ASSERT(__bss_end <= 0x81300000, "the apploader must stay below 0x81300000 (the IPL runs there)")
ASSERT(apploader_entry == 0x81200000, "apploader_entry must come first")
