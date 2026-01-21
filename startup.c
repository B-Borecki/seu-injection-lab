#include <stdint.h>

extern int main(void);
extern void vPortSVCHandler(void);
extern void xPortPendSVHandler(void);
extern void xPortSysTickHandler(void);

void Reset_Handler(void);
void Default_Handler(void);

extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;

__attribute__((section(".isr_vector")))
const void *vector_table[] = {
  (void *)(0x20000000u + 0x00010000u), // Stack = SRAM start + 64KB
  Reset_Handler,
  Default_Handler, // NMI
  Default_Handler, // HardFault
  Default_Handler, // MemManage
  Default_Handler, // BusFault
  Default_Handler, // UsageFault
  0, 0, 0, 0,       // Reserved
  vPortSVCHandler, // SVCall
  Default_Handler, // DebugMon
  0,               // Reserved
  xPortPendSVHandler, // PendSV
  xPortSysTickHandler  // SysTick
};

static void init_data_bss(void) {
  uint32_t *src = &_sidata;
  uint32_t *dst = &_sdata;
  while (dst < &_edata) {
    *dst++ = *src++;
  }

  uint32_t *bss = &_sbss;
  while (bss < &_ebss) {
    *bss++ = 0u;
  }
}

void Reset_Handler(void) {
  init_data_bss();
  (void)main();
  while (1) {}
}

void Default_Handler(void) {
  while (1) {}
}
