/* GD32E230C8: 64 KiB flash at 0x08000000, 8 KiB SRAM at 0x20000000.
 *
 * RAM size is assumed. The factory firmware's initial SP is 0x20000420, which
 * would imply a much smaller stack region than 8 KiB — worth checking against
 * the datasheet before relying on it. Nothing here needs more than a few
 * hundred bytes, so it doesn't matter yet.
 */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 64K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 8K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
    .isr_vector : {
        KEEP(*(.isr_vector))
    } > FLASH

    .text : {
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    _sidata = LOADADDR(.data);

    /* Pinned to the base of RAM so the debugger can read it without symbols. */
    .debug_block (NOLOAD) : {
        . = ALIGN(4);
        KEEP(*(.debug_block))
    } > RAM

    .data : {
        . = ALIGN(4);
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss : {
        . = ALIGN(4);
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    /DISCARD/ : {
        *(.ARM.exidx*)
    }
}
