#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#define UART0_BASE 0x4000C000u
#define UARTDR     (*(volatile uint32_t *)(UART0_BASE + 0x000u))
#define UARTFR     (*(volatile uint32_t *)(UART0_BASE + 0x018u))

#define SAMPLE_PERIOD_MS  5u
#define MAX_SEQ  20000u
#define K  8
#define CMD_M_MAX  2000

#ifndef SEU_ENABLE
#define SEU_ENABLE 0
#endif

extern uint32_t _sdata;
extern uint32_t _ebss;

typedef struct {
  uint32_t seq;
  int32_t  bx;
  int32_t  by;
  int32_t  bz;
} sensor_sample;

typedef struct {
  uint32_t seq;
  int32_t  mx;
  int32_t  my;
  int32_t  mz;
  sensor_sample sensor;
} coil_cmd;

static QueueHandle_t sensor_samples;
static QueueHandle_t coil_cmds;

volatile uint32_t hardfault_count = 0;
static volatile uint32_t sample_count = 0;
static volatile uint32_t error_count = 0;
static volatile uint32_t max_error = 0;

// UART
static void uart_putc(char c) {
  while (UARTFR & (1u << 5)) {}
  UARTDR = (uint32_t)c;
}

static void uart_puts(const char *s) {
  while (*s) uart_putc(*s++);
}

static void uart_puthex_u32(uint32_t v) {
  static const char *HEX = "0123456789ABCDEF";
  for (int i = 7; i >= 0; --i) {
    uart_putc(HEX[(v >> (i * 4)) & 0xFu]);
  }
}

static void uart_puthex_i32(int32_t v) {
  uart_puthex_u32((uint32_t)v);
}

static uint32_t u32_abs_i32(int32_t x) {
  uint32_t ux = (uint32_t)x;
  return (x < 0) ? (uint32_t)(~ux + 1u) : ux;
}

// SENSOR
static void task_sensor(void *arg) {
  (void)arg;

  uint32_t seq = 0;
  const int32_t BX0 = 20000;
  const int32_t BY0 = -5000;
  const int32_t BZ0 = 12000;

  while (1) {
    int32_t dx = (int32_t)(seq & 0xFFu) - 128;
    int32_t dy = (int32_t)((seq >> 1) & 0xFFu) - 128;
    int32_t dz = (int32_t)((seq >> 2) & 0xFFu) - 128;

    sensor_sample s = {
      .seq = seq++,
      .bx  = BX0 + dx,
      .by  = BY0 + dy,
      .bz  = BZ0 + dz
    };

    xQueueSend(sensor_samples, &s, 0);
    vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
  }
}

// CONTROLLER
static void task_controller(void *arg) {
  (void)arg;

  sensor_sample prev_sample;
  sensor_sample curr_sample;

  if (xQueueReceive(sensor_samples, &curr_sample, portMAX_DELAY) == pdPASS) {
    prev_sample = curr_sample;
  }

  while (1) {
    if (xQueueReceive(sensor_samples, &curr_sample, portMAX_DELAY) != pdPASS)
      continue;

    int32_t dBx = curr_sample.bx - prev_sample.bx;
    int32_t dBy = curr_sample.by - prev_sample.by;
    int32_t dBz = curr_sample.bz - prev_sample.bz;

    int32_t mx = -K * dBx;
    int32_t my = -K * dBy;
    int32_t mz = -K * dBz;

    if (mx > CMD_M_MAX) mx = CMD_M_MAX;
    if (mx < -CMD_M_MAX) mx = -CMD_M_MAX;
    if (my > CMD_M_MAX) my = CMD_M_MAX;
    if (my < -CMD_M_MAX) my = -CMD_M_MAX;
    if (mz > CMD_M_MAX) mz = CMD_M_MAX;
    if (mz < -CMD_M_MAX) mz = -CMD_M_MAX;

    coil_cmd cmd =
    {
      .seq = curr_sample.seq,
      .mx = mx,
      .my = my,
      .mz = mz,
      .sensor = curr_sample
    };

    xQueueSend(coil_cmds, &cmd, 0);
    prev_sample = curr_sample;
  }
}

// ACTUATOR + DETEKCJA BŁĘDÓW
static void task_actuator(void *arg) {
  (void)arg;

  coil_cmd cmd;
  sensor_sample prev_sample = {0};
  int first = 1;

  while (1) {
    if (xQueueReceive(coil_cmds, &cmd, portMAX_DELAY) != pdPASS)
      continue;

    sample_count++;

    if (first) {
      prev_sample = cmd.sensor;
      first = 0;
      continue;
    }

    // 🔥 prawdziwe dane, nie zgadywane
    int32_t dBx = cmd.sensor.bx - prev_sample.bx;
    int32_t dBy = cmd.sensor.by - prev_sample.by;
    int32_t dBz = cmd.sensor.bz - prev_sample.bz;

    int32_t mx_ref = -K * dBx;
    int32_t my_ref = -K * dBy;
    int32_t mz_ref = -K * dBz;

    if (mx_ref > CMD_M_MAX) mx_ref = CMD_M_MAX;
    if (mx_ref < -CMD_M_MAX) mx_ref = -CMD_M_MAX;
    if (my_ref > CMD_M_MAX) my_ref = CMD_M_MAX;
    if (my_ref < -CMD_M_MAX) my_ref = -CMD_M_MAX;
    if (mz_ref > CMD_M_MAX) mz_ref = CMD_M_MAX;
    if (mz_ref < -CMD_M_MAX) mz_ref = -CMD_M_MAX;

    uint32_t err =
      u32_abs_i32(cmd.mx - mx_ref) +
      u32_abs_i32(cmd.my - my_ref) +
      u32_abs_i32(cmd.mz - mz_ref);

    if (err > 0) {
      error_count++;
      if (err > max_error) max_error = err;

      uart_puts("[ERR] seq=");
      uart_puthex_u32(cmd.seq);
      uart_puts(" err=");
      uart_puthex_u32(err);
      uart_puts("\r\n");
    }

    prev_sample = cmd.sensor;

    if (cmd.seq >= MAX_SEQ)
    {
      uart_puts("[END]\r\n");

      uart_puts("samples=");
      uart_puthex_u32(sample_count);
      uart_puts("\r\n");

      uart_puts("errors=");
      uart_puthex_u32(error_count);
      uart_puts("\r\n");

      uart_puts("max_error=");
      uart_puthex_u32(max_error);
      uart_puts("\r\n");

      uart_puts("hardfaults=");
      uart_puthex_u32(hardfault_count);
      uart_puts("\r\n");

      __asm volatile("cpsid i");
      while (1) {}
    }
  }
}

// RNG + SEU
static uint32_t rng_state = 0x12345678;

static uint32_t xorshift32(void) {
  uint32_t x = rng_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rng_state = x;
  return x;
}

static void seu_inject_random(void) {
  uint32_t *start = &_sdata + 0x600;
  uint32_t *end   = &_ebss;

  uint32_t range = (uint32_t)(end - start);
  if (!range) return;

  uint32_t idx = xorshift32() % range;
  uint32_t bit = xorshift32() % 32;

  uint32_t *target = start + idx;

  uart_puts("[SEU] ");
  uart_puthex_u32((uint32_t)target);
  uart_puts("\r\n");

  *target ^= (1u << bit);
}

static void task_seu(void *arg) {
  while (1) {
    seu_inject_random();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// MAIN
int main(void) {
  uart_puts("START\n");

  sensor_samples = xQueueCreate(8, sizeof(sensor_sample));
  coil_cmds = xQueueCreate(8, sizeof(coil_cmd));

  xTaskCreate(task_sensor, "sensor", 256, NULL, 2, NULL);
  xTaskCreate(task_controller, "controller", 256, NULL, 2, NULL);
  xTaskCreate(task_actuator, "actuator", 256, NULL, 2, NULL);

#if SEU_ENABLE
  xTaskCreate(task_seu, "seu", 256, NULL, 1, NULL);
#endif

  vTaskStartScheduler();
  while (1) {}
}