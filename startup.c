#include <stdint.h>

// main() z programu użytkownika (docelowo uruchamiane po inicjalizacji RAM)
extern int main(void);
// Handlery wyjątków używane przez FreeRTOS (SVC/PendSV/SysTick)
extern void vPortSVCHandler(void);
extern void xPortPendSVHandler(void);
extern void xPortSysTickHandler(void);
// Handler resetu (punkt wejścia po starcie CPU) oraz domyślny handler wyjątków
void Reset_Handler(void);
void Default_Handler(void);
// Zmienne z linker.ld: adresy potrzebne do skopiowania .data i wyzerowania .bss
extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;
// Tablica wektorów przerwań umieszczona w sekcji .isr_vector we FLASH
__attribute__((section(".isr_vector")))
const void *vector_table[] = {
// Początkowa wartość stosu: koniec SRAM 0x20000000 + 64KB (stos rośnie w dół)
  (void *)(0x20000000u + 0x00010000u),
// Adres funkcji wykonywanej po resecie
  Reset_Handler,
// Kolejne wpisy: handlery wyjątków rdzenia Cortex-M
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
// Zarezerwowane wpisy w tablicy wektorów
  0, 0, 0, 0,
// SVCall używany przez FreeRTOS
  vPortSVCHandler,
// Debug monitor (tu nieużywany)
  Default_Handler,
  0,// Reserved
// PendSV używany przez FreeRTOS do przełączania kontekstu
  xPortPendSVHandler,
// SysTick używany przez FreeRTOS jako tick systemowy
  xPortSysTickHandler
};

// Inicjalizacja pamięci: kopiowanie .data z FLASH do RAM i zerowanie .bss w RAM
static void init_data_bss(void) {
  uint32_t *src = &_sidata;
  uint32_t *dst = &_sdata;
  while (dst < &_edata) {
    *dst++ = *src++;
  }

// Zakres .bss w RAM musi zostać wyzerowany
  uint32_t *bss = &_sbss;
  while (bss < &_ebss) {
    *bss++ = 0u;
  }
}

// Handler resetu: przygotuj RAM, uruchom main()
void Reset_Handler(void) {
  init_data_bss();
  (void)main();
  while (1) {}
}

// Domyślny handler: zatrzymaj CPU w nieskończonej pętli
void Default_Handler(void) {
  while (1) {}
}
