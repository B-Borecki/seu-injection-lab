/* Punkt wejścia po resecie (adres startowy programu) */
ENTRY(Reset_Handler)

/* zdefiniowane przez ARM dla architektury Cortex-M */
MEMORY
{
/* Pamięć programu (kod/stałe)  */
  FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 256K
/* Pamięć RAM (dane/stack/heap) */
  SRAM  (rwx): ORIGIN = 0x20000000, LENGTH = 64K
}

SECTIONS
{
/* Zachowaj tablicę wektorów przerwań,
kod programu i stałe lądują w pamięci FLASH */
  .text : {
    KEEP(*(.isr_vector))
    *(.text*)
    *(.rodata*)
  } > FLASH

/* Adres w FLASH, skąd kopiujemy inicjalizowane dane .data,
zapisujemy w zmiennej, żeby móc go użyć w startup.c*/
  _sidata = LOADADDR(.data);

/* Zapisujemy do zmiennej początek i koniec .data w RAM,
.data to zmienne globalne/statyczne z inicjalizacją,
działają w RAM, ale wartość do inicjalizacji trzymana jest we FLASH */
  .data : {
    _sdata = .;
    *(.data*)
    _edata = .;
  } > SRAM AT > FLASH

/* Zmienne globalne/statyczne bez inicjalizacji,
zerowane na początku programu, trafiają do RAM */
  .bss : {
    _sbss = .;
    *(.bss*)
    *(COMMON)
    _ebss = .;
  } > SRAM
}
