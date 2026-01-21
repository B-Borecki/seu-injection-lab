ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 256K
  SRAM  (rwx): ORIGIN = 0x20000000, LENGTH = 64K
}

SECTIONS
{
  .text : {
    KEEP(*(.isr_vector))
    *(.text*)
    *(.rodata*)
  } > FLASH

  _sidata = LOADADDR(.data);

  .data : {
    _sdata = .;
    *(.data*)
    _edata = .;
  } > SRAM AT > FLASH

  .bss : {
    _sbss = .;
    *(.bss*)
    *(COMMON)
    _ebss = .;
  } > SRAM
}
