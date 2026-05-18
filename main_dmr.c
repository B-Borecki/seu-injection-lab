#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "utils.h"

#define MAX_SEQ  5000
#define K  8
#define CMD_M_MAX  2000

#ifndef SEU_ENABLE
#define SEU_ENABLE 0
#endif

extern uint32_t _sseu;
extern uint32_t _eseu;

typedef struct {
  uint32_t seq;
  int32_t  bx;
  int32_t  by;
  int32_t  bz;
  uint8_t crc;
} sensor_sample;

typedef struct {
  uint32_t seq; uint32_t seq_inv;
  int32_t  mx;  int32_t  mx_inv;
  int32_t  my;  int32_t  my_inv;
  int32_t  mz;  int32_t  mz_inv;
} coil_cmd;

//__attribute__((section(".seu_section")))
static QueueHandle_t sensor_samples; 
//__attribute__((section(".seu_section")))
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

// Globalny chroniony licznik sekwencji
__attribute__((section(".seu_section")))
static uint32_t seq_tmr[3] = {0, 0, 0};

static uint32_t get_seq(void) {
    // Głosowanie TMR
    if (seq_tmr[0] == seq_tmr[1] || seq_tmr[0] == seq_tmr[2])
        return seq_tmr[0];
    return seq_tmr[1];
}
static void inc_seq(void) {
    uint32_t new_seq = get_seq() + 1;
    seq_tmr[0] = new_seq;
    seq_tmr[1] = new_seq;
    seq_tmr[2] = new_seq;
}

static uint8_t crc8(sensor_sample *s) {
    uint8_t crc = 0;
    uint8_t *bytes = (uint8_t*)s;
    
    for (int i = 0; i < 16; i++) {
        crc ^= bytes[i];
        // Dzielenie: przesun i odejmij wielomian gdy najstarszy bit = 1
        for (int j = 0; j < 8; j++) {
            if (crc & 0b10000000) {
                crc = (crc << 1) ^ 0b00000111;
            } else {
                crc = crc << 1;
            }
        }
    }
    return crc;
}

void fix_cmd(coil_cmd *c) {
    if (c->seq != (~c->seq_inv)) {
        c->seq = (c->seq + (~c->seq_inv)) / 2;
        c->seq_inv = ~c->seq;
    }
    if (c->mx != (~c->mx_inv)) {
        c->mx = (c->mx + (~c->mx_inv)) / 2;
        c->mx_inv = ~c->mx;
    }
    if (c->my != (~c->my_inv)) {
        c->my = (c->my + (~c->my_inv)) / 2;
        c->my_inv = ~c->my;
    }
    if (c->mz != (~c->mz_inv)) {
        c->mz = (c->mz + (~c->mz_inv)) / 2;
        c->mz_inv = ~c->mz;
    }   
}

static volatile uint32_t sample_count = 0;
static volatile uint32_t error_count = 0;
static volatile uint32_t max_error = 0;
static volatile uint32_t seq_prev = -1;
static volatile uint32_t omitted_samples = 0;
static volatile uint32_t good_samples = 0;
static volatile uint32_t error_sum = 0;

// Obliczenia referencyjne, służą do badania zaburzeń w symulowanym układzie
static void compute_reference(uint32_t seq, uint32_t seq_prev, int32_t *mx_ref, int32_t *my_ref, int32_t *mz_ref)
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

  const int32_t BX = 20000;
  const int32_t BY = -5000;
  const int32_t BZ = 12000;

  while (1) {
    seq = get_seq();
    int32_t dx = (int32_t)(seq & 0xFF) - 128;
    int32_t dy = (int32_t)((seq >> 1) & 0xFF) - 128;
    int32_t dz = (int32_t)((seq >> 2) & 0xFF) - 128;
    
    sample_sensor = (sensor_sample){
      .seq = seq,
      .bx  = BX + dx,
      .by  = BY + dy,
      .bz  = BZ + dz,
    };

    sample_sensor.crc = crc8(&sample_sensor);

    vTaskDelay(pdMS_TO_TICKS(1)); 

    xQueueSend(sensor_samples, &sample_sensor, 0);
    sample_count++;
    vTaskDelay(pdMS_TO_TICKS(5));
    inc_seq();
  }
}

// CONTROLLER
static void task_controller(void *arg) {
  (void)arg;

  // Pierwsza próbka jest porpawna, bo task_seu startuje z opóźnieniem
  if (xQueueReceive(sensor_samples, &sample_controller, portMAX_DELAY) == pdPASS) {
      prev_sample = sample_controller;
      seq_prev = sample_controller.seq;
      good_samples++;
    }

  while (1) {
    if (xQueueReceive(sensor_samples, &sample_controller, portMAX_DELAY) != pdPASS) {
      continue;
    }

    if (sample_controller.crc != crc8(&sample_controller)) {
      omitted_samples++;
      uart_puts("[CRC NOT MATCH]");
      uart_puts("\r\n");
      continue;
    }

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
      .seq_inv = ~sample_controller.seq,
      .mx = mx,
      .mx_inv = ~mx,
      .my = my,
      .my_inv = ~my,
      .mz = mz,
      .mz_inv = ~mz
    };

    vTaskDelay(pdMS_TO_TICKS(1)); 

    fix_cmd(&cmd_controller);

    xQueueSend(coil_cmds, &cmd_controller, 0);
    prev_sample = sample_controller;
  }
}

// ACTUATOR + DETEKCJA BŁĘDÓW
static void task_actuator(void *arg) {
  (void)arg;

  while (1) {
    if (xQueueReceive(coil_cmds, &cmd_actuator, portMAX_DELAY) != pdPASS) {
      continue;
    }

    vTaskDelay(pdMS_TO_TICKS(1)); 

    int32_t mx_ref, my_ref, mz_ref;
    compute_reference(cmd_actuator.seq, seq_prev, &mx_ref, &my_ref, &mz_ref);

    uint32_t err = u32_abs_i32(cmd_actuator.mx - mx_ref) + u32_abs_i32(cmd_actuator.my - my_ref) + u32_abs_i32(cmd_actuator.mz - mz_ref);
    error_sum += err;

    if (err > 0) {
      error_count++;
      if (err > max_error) max_error = err;

      uart_puts("[ERR] seq=");
      uart_putdec_u32(cmd_actuator.seq);
      uart_puts(" err=");
      uart_putdec_u32(err);
      uart_puts("\r\n");
    } else {
      good_samples++;
    }

    if (sample_count >= MAX_SEQ && cmd_actuator.seq >= MAX_SEQ)
    {
      uart_puts("[END]\r\n");
      uart_puts("samples=");
      uart_putdec_u32(sample_count);
      uart_puts("\r\n");

      uart_puts("errors=");
      uart_putdec_u32(error_count);
      uart_puts("\r\n");

      uart_puts("samples ommited=");
      uart_putdec_u32(omitted_samples);
      uart_puts("\r\n");

      uart_puts("good samples=");
      uart_putdec_u32(good_samples);
      uart_puts("\r\n");   

      uart_puts("max error=0x");
      uart_puthex_u32(max_error);
      uart_puts("\r\n");

      uart_puts("errors sum=0x");
      uart_puthex_u32(error_sum);
      uart_puts("\r\n");

      __asm volatile("cpsid i");
      while (1) {}
    }

    seq_prev = cmd_actuator.seq;
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

  vTaskDelay(pdMS_TO_TICKS(50)); 
  while (1) {
    uint8_t *start = (uint8_t*)&_sseu;
    uint8_t *end   = (uint8_t*)&_eseu;

    uint32_t range = end - start;

    uint32_t byte = xorshift32() % range;
    uint8_t bit = xorshift32() % 8;

    uint8_t *target = start + byte;

    uart_puts("[SEU] addr=");
    uart_puthex_u32((uint32_t)target);
    uart_puts(" bit=");
    uart_puthex_u32(bit);
    uart_puts("\r\n");

    *target ^= (1 << bit);

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