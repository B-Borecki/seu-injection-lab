#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "utils.h"

#define MAX_SEQ  5000u
#define K  8
#define CMD_M_MAX  2000

#ifndef SEU_ENABLE
#define SEU_ENABLE 0
#endif

extern uint32_t _sdata;
extern uint32_t _ebss;
extern uint8_t _sseu;
extern uint8_t _eseu;

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
} coil_cmd;

static QueueHandle_t sensor_samples;
static QueueHandle_t coil_cmds;

__attribute__((section(".seu_section")))
static sensor_sample sample_sensor;
__attribute__((section(".seu_section")))
static sensor_sample sample_controller;
__attribute__((section(".seu_section")))
static sensor_sample prev_sample;
__attribute__((section(".seu_section")))
static coil_cmd cmd_controller;
__attribute__((section(".seu_section")))
static coil_cmd cmd_actuator;
__attribute__((section(".seu_section")))
static uint32_t seq;
__attribute__((section(".seu_section")))
static int32_t dBx;
__attribute__((section(".seu_section")))
static int32_t dBy;
__attribute__((section(".seu_section")))
static int32_t dBz;
__attribute__((section(".seu_section")))
static int32_t mx;
__attribute__((section(".seu_section")))
static int32_t my;
__attribute__((section(".seu_section")))
static int32_t mz;

static volatile uint32_t sample_count = 0;
static volatile uint32_t error_count = 0;
static volatile uint32_t max_error = 0;

// Obliczenia referencyjne, służą do badania zaburzeń w symulowanym układzie
static void compute_reference(uint32_t seq, int32_t *mx_ref, int32_t *my_ref, int32_t *mz_ref)
{
  const int32_t BX = 20000;
  const int32_t BY = -5000;
  const int32_t BZ = 12000;

  int32_t dx  = (int32_t)(seq & 0xFFu) - 128;
  int32_t dy  = (int32_t)((seq >> 1) & 0xFFu) - 128;
  int32_t dz  = (int32_t)((seq >> 2) & 0xFFu) - 128;

  int32_t bx  = BX + dx;
  int32_t by  = BY + dy;
  int32_t bz  = BZ + dz;

  uint32_t seq_prev = seq - 1;

  int32_t dxp = (int32_t)(seq_prev & 0xFFu) - 128;
  int32_t dyp = (int32_t)((seq_prev >> 1) & 0xFFu) - 128;
  int32_t dzp = (int32_t)((seq_prev >> 2) & 0xFFu) - 128;

  int32_t bxp = BX + dxp;
  int32_t byp = BY + dyp;
  int32_t bzp = BZ + dzp;

  int32_t dBx = bx - bxp;
  int32_t dBy = by - byp;
  int32_t dBz = bz - bzp;

  int32_t mx = -K * dBx;
  int32_t my = -K * dBy;
  int32_t mz = -K * dBz;

  if (mx > CMD_M_MAX) mx = CMD_M_MAX;
  if (mx < -CMD_M_MAX) mx = -CMD_M_MAX;
  if (my > CMD_M_MAX) my = CMD_M_MAX;
  if (my < -CMD_M_MAX) my = -CMD_M_MAX;
  if (mz > CMD_M_MAX) mz = CMD_M_MAX;
  if (mz < -CMD_M_MAX) mz = -CMD_M_MAX;

  *mx_ref = mx;
  *my_ref = my;
  *mz_ref = mz;
}

// SENSOR
static void task_sensor(void *arg) {
  (void)arg;

  int32_t seq = 0;
  const int32_t BX = 20000;
  const int32_t BY = -5000;
  const int32_t BZ = 12000;

  while (1) {
    int32_t dx = (int32_t)(seq & 0xFF) - 128;
    int32_t dy = (int32_t)((seq >> 1) & 0xFF) - 128;
    int32_t dz = (int32_t)((seq >> 2) & 0xFF) - 128;
    
    sample_sensor = (sensor_sample){
      .seq = seq,
      .bx  = BX + dx,
      .by  = BY + dy,
      .bz  = BZ + dz
    };
    
    vTaskDelay(pdMS_TO_TICKS(1)); 

    xQueueSend(sensor_samples, &sample_sensor, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    ++seq;
  }
}

// CONTROLLER
static void task_controller(void *arg) {
  (void)arg;

  if (xQueueReceive(sensor_samples, &sample_controller, portMAX_DELAY) == pdPASS) {
    prev_sample = sample_controller;
  }

  while (1) {
    if (xQueueReceive(sensor_samples, &sample_controller, portMAX_DELAY) != pdPASS)
      continue;

    dBx = sample_controller.bx - prev_sample.bx;
    dBy = sample_controller.by - prev_sample.by;
    dBz = sample_controller.bz - prev_sample.bz;

    mx = -K * dBx;
    my = -K * dBy;
    mz = -K * dBz;

    if (mx > CMD_M_MAX) mx = CMD_M_MAX;
    if (mx < -CMD_M_MAX) mx = -CMD_M_MAX;
    if (my > CMD_M_MAX) my = CMD_M_MAX;
    if (my < -CMD_M_MAX) my = -CMD_M_MAX;
    if (mz > CMD_M_MAX) mz = CMD_M_MAX;
    if (mz < -CMD_M_MAX) mz = -CMD_M_MAX;
    
    cmd_controller = (coil_cmd){
      .seq = sample_controller.seq,
      .mx = mx,
      .my = my,
      .mz = mz,
    };

    vTaskDelay(pdMS_TO_TICKS(1)); 

    xQueueSend(coil_cmds, &cmd_controller, 0);
    prev_sample = sample_controller;
  }
}

// ACTUATOR + DETEKCJA BŁĘDÓW
static void task_actuator(void *arg) {
  (void)arg;

  while (1) {
    if (xQueueReceive(coil_cmds, &cmd_actuator, portMAX_DELAY) != pdPASS)
      continue;
    sample_count++;

    vTaskDelay(pdMS_TO_TICKS(1)); 

    if (cmd_actuator.seq > 0)
    {
      int32_t mx_ref, my_ref, mz_ref;

      compute_reference(cmd_actuator.seq, &mx_ref, &my_ref, &mz_ref);

      uint32_t err = u32_abs_i32(cmd_actuator.mx - mx_ref) + u32_abs_i32(cmd_actuator.my - my_ref) + u32_abs_i32(cmd_actuator.mz - mz_ref);

      if (err > 0) {
        error_count++;
        if (err > max_error) max_error = err;

        uart_puts("[ERR] seq=");
        uart_puthex_u32(cmd_actuator.seq);
        uart_puts(" err=");
        uart_puthex_u32(err);
        uart_puts("\r\n");
      }
    }

    if (sample_count >= MAX_SEQ)
    {
      uart_puts("[END]\r\n");
      uart_puthex_u32(cmd_actuator.seq);
      uart_puts("samples=");
      uart_puthex_u32(sample_count);
      uart_puts("\r\n");

      uart_puts("errors=");
      uart_puthex_u32(error_count);
      uart_puts("\r\n");

      uart_puts("max_error=");
      uart_puthex_u32(max_error);
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

static void task_seu(void *arg) {
  (void)arg;

  while (1) {
    uint8_t *start = &_sseu;
    uint8_t *end   = &_eseu;

    uint32_t range = (uint32_t)(end - start);
    if (!range) continue;

    uint32_t idx = xorshift32() % range;
    uint32_t bit = xorshift32() % 8;

    uint8_t *target = start + idx;

    uart_puts("[SEU] addr=");
    uart_puthex_u32((uint32_t)target);
    uart_puts(" bit=");
    uart_puthex_u32(bit);
    uart_puts("\r\n");

    *target ^= (1u << bit);

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

int main(void) {
  uart_puts("START\n");

  sensor_samples = xQueueCreate(8, sizeof(sensor_sample));
  coil_cmds = xQueueCreate(8, sizeof(coil_cmd));

  xTaskCreate(task_sensor, "sensor", 256, NULL, 2, NULL);
  xTaskCreate(task_controller, "controller", 256, NULL, 2, NULL);
  xTaskCreate(task_actuator, "actuator", 256, NULL, 2, NULL);

#if SEU_ENABLE
  xTaskCreate(task_seu, "seu", 256, NULL, 3, NULL);
#endif

  vTaskStartScheduler();
  while (1) {}
}