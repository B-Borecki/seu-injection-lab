#include <stdint.h>

void *memcpy(void *dest, const void *src, unsigned int n);
void *memset(void *dest, int val, unsigned int n);

void uart_putc(char c);
void uart_puts(const char *s);

void uart_puthex_u32(uint32_t v);
void uart_puthex_i32(int32_t v);

uint32_t u32_abs_i32(int32_t x);

void uart_putdec_u32(uint32_t v);
void uart_putdec_i32(int32_t v);

void uart_puthex_u64(int32_t v);

extern const int16_t sin_table[];