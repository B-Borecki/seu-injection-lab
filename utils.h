#include <stdint.h>
#include <limits.h>

void *memcpy(void *dest, const void *src, unsigned int n);
void *memset(void *dest, int val, unsigned int n);

void uart_putc(char c);
void uart_puts(const char *s);

void uart_puthex_u32(uint32_t v);
void uart_puthex_i32(int32_t v);

uint32_t u32_abs_i32(int32_t x);

void uart_putdec_u32(uint32_t v);
void uart_putdec_i32(int32_t v);

void uart_puthex_u64(uint64_t v);
void uart_putdec_u64(uint64_t v);

int32_t saturate_int32(int64_t value);

extern const int16_t sin_table[];

__attribute__((noreturn)) void qemu_exit(int status);