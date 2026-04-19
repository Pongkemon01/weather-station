/**
 * @file    ldscript_app.ld
 * @brief   Linker script for the STM32L476RG application firmware.
 *
 * Flash layout:
 *   Bootloader  0x08000000 – 0x08007FFF  (32 KB, pages  0–15,  Bank 1)
 *   Application 0x08008000 – 0x0807FFFF  (480 KB, pages 16–255, Bank 1)
 *   Bank 2      0x08080000 – 0x080FFFFF  (512 KB — reserved)
 *
 * RAM layout:
 *   SRAM1  0x20000000 – 0x20017FFF  (96 KB — FreeRTOS heap, stacks, data)
 *   SRAM2  0x10000000 – 0x10007FFF  (32 KB — OTA state flags; NOT in budget)
 *
 * Size constraints (enforced by ASSERT statements at end of file):
 *   Code (.text + .rodata)        ≤ 480 KB
 *   Data (.data + .bss)           ≤ 96 KB
 */

ENTRY(Reset_Handler)

_estack = ORIGIN(RAM) + LENGTH(RAM);   /* Top of SRAM1 */

_Min_Heap_Size  = 0x800;    /* 2 KB minimum heap  */
_Min_Stack_Size = 0x800;    /* 2 KB minimum stack */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08008000, LENGTH = 480K
    RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 96K
    RAM2  (xrw) : ORIGIN = 0x10000000, LENGTH = 32K   /* SRAM2 — OTA flags */
}

SECTIONS
{
    /* ── Interrupt vector table (must be at application Flash origin) ── */
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } >FLASH

    /* ── Code and read-only data ─────────────────────────────────────── */
    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        *(.eh_frame)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
        _etext = .;
    } >FLASH

    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
    } >FLASH

    .ARM.extab :
    {
        . = ALIGN(4);
        *(.ARM.extab* .gnu.linkonce.armextab.*)
        . = ALIGN(4);
    } >FLASH

    .ARM :
    {
        . = ALIGN(4);
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
        . = ALIGN(4);
    } >FLASH

    .preinit_array :
    {
        . = ALIGN(4);
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP(*(.preinit_array*))
        PROVIDE_HIDDEN (__preinit_array_end = .);
        . = ALIGN(4);
    } >FLASH

    .init_array :
    {
        . = ALIGN(4);
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array*))
        PROVIDE_HIDDEN (__init_array_end = .);
        . = ALIGN(4);
    } >FLASH

    .fini_array :
    {
        . = ALIGN(4);
        PROVIDE_HIDDEN (__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array*))
        PROVIDE_HIDDEN (__fini_array_end = .);
        . = ALIGN(4);
    } >FLASH

    /* ── Initialised data (load from Flash, copy to RAM at startup) ──── */
    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        *(.RamFunc)
        *(.RamFunc*)
        . = ALIGN(4);
        _edata = .;
    } >RAM AT> FLASH

    /* ── Zero-initialised data ───────────────────────────────────────── */
    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        __bss_start__ = _sbss;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        __bss_end__ = _ebss;
    } >RAM

    /* ── Heap + stack guard ──────────────────────────────────────────── */
    ._user_heap_stack :
    {
        . = ALIGN(8);
        PROVIDE ( end = . );
        PROVIDE ( _end = . );
        . = . + _Min_Heap_Size;
        . = . + _Min_Stack_Size;
        . = ALIGN(8);
    } >RAM

    /* ── SRAM2 section (OTA state flags — not counted in RAM budget) ─── */
    .sram2 (NOLOAD) :
    {
        . = ALIGN(4);
        *(.sram2)
        *(.sram2*)
        . = ALIGN(4);
    } >RAM2

    /* ── Discard unwanted sections ───────────────────────────────────── */
    /DISCARD/ :
    {
        libc.a ( * )
        libm.a ( * )
        libgcc.a ( * )
    }

    .ARM.attributes 0 : { *(.ARM.attributes) }

    /* ── Size constraints (linker aborts if violated) ───────────────── */
    ASSERT((_etext - ORIGIN(FLASH)) <= 480K,
           "Application code exceeds 480 KB Flash budget!")
    ASSERT((_ebss - ORIGIN(RAM)) <= 96K,
           "Application data exceeds 96 KB SRAM1 budget!")
}
