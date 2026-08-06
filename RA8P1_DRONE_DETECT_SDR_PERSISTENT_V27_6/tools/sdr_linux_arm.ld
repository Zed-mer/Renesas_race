ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
    stack PT_GNU_STACK FLAGS(6);
}

SECTIONS
{
    . = 0x00010000;
    .text :
    {
        KEEP(*(.text.start))
        *(.text*)
        *(.rodata*)
        *(.ARM.extab*)
        *(.ARM.exidx*)
    } :text

    . = ALIGN(0x1000);
    .data :
    {
        *(.data*)
    } :data

    .bss (NOLOAD) :
    {
        . = ALIGN(64);
        *(.bss*)
        *(COMMON)
    } :data

    /DISCARD/ :
    {
        *(.comment*)
        *(.note*)
    }
}
