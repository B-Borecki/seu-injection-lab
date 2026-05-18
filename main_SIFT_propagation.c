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

static const int16_t sin_table[] = {
    0, 17, 35, 52, 70, 87, 105, 122, 139, 156,
    173, 190, 207, 224, 241, 258, 275, 291, 307,
    324, 342, 358, 375, 391, 407, 423, 438, 454,
    469, 484, 499, 514, 529, 543, 557, 571, 585,
    598, 611, 624, 637, 649, 661, 673, 685, 696,
    707, 718, 729, 739, 749, 759, 768, 777, 786,
    795, 803, 811, 819, 826, 833, 840, 847, 853,
    859, 865, 870, 875, 880, 885, 889, 893, 896,
    900, 903, 906, 908, 910, 912, 914, 915, 916,
    917, 917, 917, 917, 917, 916, 915, 914, 913,
    912, 910, 908, 906, 904, 901, 898, 895, 892,
    888, 884, 880, 876, 871, 866, 861, 856, 850,
    844, 838, 832, 825, 819, 812, 805, 798, 790,
    783, 775, 767, 759, 750, 742, 733, 724, 715,
    706, 696, 687, 677, 667, 657, 647, 636, 626,
    615, 604, 593, 582, 571, 559, 548, 536, 524,
    512, 500, 488, 476, 463, 451, 438, 425, 412,
    399, 386, 373, 359, 346, 332, 319, 305, 291,
    277, 263, 249, 235, 221, 207, 192, 178, 163,
    149, 134, 120, 105, 90, 75, 60, 45, 30, 15, 0,
    -15, -30, -45, -60, -75, -90, -105, -120, -134, -149,
    -163, -178, -192, -207, -221, -235, -249, -263, -277, -291,
    -305, -319, -332, -346, -359, -373, -386, -399, -412, -425,
    -438, -451, -463, -476, -488, -500, -512, -524, -536, -548,
    -559, -571, -582, -593, -604, -615, -626, -636, -647, -657,
    -667, -677, -687, -696, -706, -715, -724, -733, -742, -750,
    -759, -767, -775, -783, -790, -798, -805, -812, -819, -825,
    -832, -838, -844, -850, -856, -861, -866, -871, -876, -880,
    -884, -888, -892, -895, -898, -901, -904, -906, -908, -910,
    -912, -913, -914, -915, -916, -917, -917, -917, -917, -916,
    -915, -914, -912, -910, -908, -906, -903, -900, -896, -893,
    -889, -885, -880, -875, -870, -865, -859, -853, -847, -840,
    -833, -826, -819, -811, -803, -795, -786, -777, -768, -759,
    -749, -739, -729, -718, -707, -696, -685, -673, -661, -649,
    -637, -624, -611, -598, -585, -571, -557, -543, -529, -514,
    -499, -484, -469, -454, -438, -423, -407, -391, -375, -358,
    -342, -324, -307, -291, -275, -258, -241, -224, -207, -190,
    -173, -156, -139, -122, -105, -87, -70, -52, -35, -17
};

static volatile uint32_t sample_count = 0;
static volatile uint32_t error_count = 0;
static volatile uint32_t max_error = 0;
static volatile int32_t seq_prev = -1;
static volatile uint32_t omitted_samples = 0;
static volatile uint32_t good_samples = 0;
static volatile uint32_t propagation_counter = 0;

static volatile int32_t propagation_mx = 0;
static volatile int32_t propagation_my = 0;
static volatile int32_t propagation_mz = 0;


// Obliczenia referencyjne, służą do badania zaburzeń w symulowanym układzie
static void compute_reference(uint32_t seq, uint32_t seq_prev, int32_t *mx_ref, int32_t *my_ref, int32_t *mz_ref)
{
  const int32_t BX = 20000;
  const int32_t BY = -5000;
  const int32_t BZ = 12000;

  int32_t dx  = (int32_t)(seq & 0xFFu) - 128;
  int32_t dy  = (int32_t)((seq >> 1) & 0xFFu) - 128;
  int32_t dz  = (int32_t)((seq >> 2) & 0xFFu) - 128;

  int32_t variation = (500 * sin_table[seq % 360]) / 1000;

  int32_t bx  = BX + dx + variation;
  int32_t by  = BY + dy + variation / 2;
  int32_t bz  = BZ + dz + variation / 3;

  int32_t dxp = (int32_t)(seq_prev & 0xFFu) - 128;
  int32_t dyp = (int32_t)((seq_prev >> 1) & 0xFFu) - 128;
  int32_t dzp = (int32_t)((seq_prev >> 2) & 0xFFu) - 128;

  int32_t variation_prev = (500 * sin_table[seq_prev % 360]) / 1000;

  int32_t bxp = BX + dxp + variation_prev;
  int32_t byp = BY + dyp + variation_prev / 2;
  int32_t bzp = BZ + dzp + variation_prev / 3;

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

    int32_t variation = (500 * sin_table[seq % 360]) / 1000;

    if (propagation_mx + propagation_my + propagation_mz != 0) {
      propagation_counter++;
    }
    
    sample_sensor = (sensor_sample){
      .seq = seq,
      .bx  = BX + dx + variation + propagation_mx,
      .by  = BY + dy + variation / 2 + propagation_my,
      .bz  = BZ + dz + variation / 3 + propagation_mz,
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
    
    cmd_controller = (coil_cmd){
      .seq = sample_controller.seq,
      .mx = mx,
      .my = my,
      .mz = mz,
    };

    vTaskDelay(pdMS_TO_TICKS(1)); 
    
    if (cmd_controller.mx > CMD_M_MAX) cmd_controller.mx = CMD_M_MAX;
    if (cmd_controller.mx < -CMD_M_MAX) cmd_controller.mx = -CMD_M_MAX;
    if (cmd_controller.my > CMD_M_MAX) cmd_controller.my = CMD_M_MAX;
    if (cmd_controller.my < -CMD_M_MAX) cmd_controller.my = -CMD_M_MAX;
    if (cmd_controller.mz > CMD_M_MAX) cmd_controller.mz = CMD_M_MAX;
    if (cmd_controller.mz < -CMD_M_MAX) cmd_controller.mz = -CMD_M_MAX;

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

    if (err > 0) {
      error_count++;

      if (err > max_error) max_error = err;

      uart_puts("[ERR] seq=");
      uart_putdec_u32(cmd_actuator.seq);
      uart_puts(" err=");
      uart_putdec_u32(err);
      uart_puts("\r\n");

      propagation_mx = (cmd_actuator.mx - mx_ref) / 100;
      propagation_my = (cmd_actuator.my - my_ref) / 100;
      propagation_mz = (cmd_actuator.mz - mz_ref) / 100;
    } else {
      good_samples++;
      propagation_mx = 0;
      propagation_my = 0;
      propagation_mz = 0;
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

      uart_puts("max_error=0x");
      uart_puthex_u32(max_error);
      uart_puts("\r\n");

      uart_puts("propagations=");
      uart_putdec_u32(propagation_counter);
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

    vTaskDelay(pdMS_TO_TICKS(50));
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