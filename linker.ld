/*
 * linker.ld — GCC linker script for the ADuCM355
 *
 * Memory map (from the ADuCM355 data sheet, Flash and SRAM Organization):
 *
 *   0x0000..0x013F   Interrupt vector table   80 entries * 4 B = 0x140 B
 *   0x0140..0x017F   Pad                      must be 0xFF
 *   0x0180..0x018F   ReadProtectedKeyHash     four 32-bit words
 *   0x0190..0x0193   CRC of key hash
 *   0x0194..0x0197   LastCRCPage / NumCRCPages
 *   0x0198..0x019B   InCircuitWriteProtectCode
 *   0x019C..0x019F   FlashBlockWriteProtect
 *   0x01A0..0x07F7   Gap (filled 0xFF via objcopy --gap-fill)
 *   0x07F8..0x07FB   CRC32 checksum over page 0 (patched post-build)
 *   0x07FC..0x07FF   Page 0 tail word
 *   0x0800..          .text and .rodata
 *
 * SRAM occupies 0x20000000..0x20003FFF (16 KB).
 */

ENTRY(ResetISR)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 256K
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 16K
}

_estack = ORIGIN(SRAM) + LENGTH(SRAM);

SECTIONS
{
    .isr_vector 0x00000000 :
    {
        KEEP(*(.isr_vector))
    } > FLASH

    .page0_pad 0x00000140 :
    {
        KEEP(*(.page0_pad))
    } > FLASH

    .ReadProtectedKeyHash 0x00000180 :
    {
        KEEP(*(.ReadProtectedKeyHash))
    } > FLASH

    .CRC_ReadProtectedKeyHash 0x00000190 :
    {
        KEEP(*(.CRC_ReadProtectedKeyHash))
    } > FLASH

    .NumCRCPages 0x00000194 :
    {
        KEEP(*(.NumCRCPages))
    } > FLASH

    .InCircuitWriteProtect 0x00000198 :
    {
        KEEP(*(.InCircuitWriteProtect))
    } > FLASH

    .FlashBlockWriteProtect 0x0000019C :
    {
        KEEP(*(.FlashBlockWriteProtect))
    } > FLASH

    .checksum 0x000007F8 :
    {
        KEEP(*(.checksum))
    } > FLASH

    .page0_tail 0x000007FC :
    {
        KEEP(*(.page0_tail))
    } > FLASH

    .text 0x00000800 :
    {
        *(.text*)
        *(.rodata*)
        *(.glue_7)
        *(.glue_7t)
        *(.eh_frame*)
    } > FLASH

    .ARM.exidx :
    {
        *(.ARM.exidx*)
    } > FLASH

    _etext = .;

    .data :
    {
        _data = .;
        *(.data*)
        _edata = .;
    } > SRAM AT > FLASH

    .bss :
    {
        _bss = .;
        *(.bss*)
        *(COMMON)
        _ebss = .;
    } > SRAM

    . = ALIGN(8);
    end  = .;
    _end = .;
}
