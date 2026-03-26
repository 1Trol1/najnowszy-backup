OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(kmain)

SECTIONS
{
    /* Bazowy adres wirtualny — Twój loader i tak relokuje */
    . = 0x400000;

    .text : ALIGN(0x1000)
    {
        *(.text .text.*)
    }

    .rodata : ALIGN(0x1000)
    {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(0x1000)
    {
        *(.data .data.*)
    }

    .bss : ALIGN(0x1000)
    {
        *(.bss .bss.* COMMON)
    }
}
