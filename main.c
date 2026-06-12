#ifdef EXPERIMENT_BASELINE
  #define SEU_ENABLE 0
  #define SEQ_TMR_ENABLE 0
  #define SENSOR_CRC_ENABLE 0
  #define QUEUE_PROTECT_ENABLE 0
  #define CLAMP_ENABLE 0
#elif defined(EXPERIMENT_SEU_ONLY)
  #define SEU_ENABLE 1
  #define SEQ_TMR_ENABLE 0
  #define SENSOR_CRC_ENABLE 0
  #define QUEUE_PROTECT_ENABLE 0
  #define CLAMP_ENABLE 0
#elif defined(EXPERIMENT_QUEUE_PROTECT)
  #define SEU_ENABLE 1
  #define SEQ_TMR_ENABLE 0
  #define SENSOR_CRC_ENABLE 0
  #define QUEUE_PROTECT_ENABLE 1
  #define CLAMP_ENABLE 0
#elif defined(EXPERIMENT_SEQ_TMR)
  #define SEU_ENABLE 1
  #define SEQ_TMR_ENABLE 1
  #define SENSOR_CRC_ENABLE 0
  #define QUEUE_PROTECT_ENABLE 1
  #define CLAMP_ENABLE 0
#elif defined(EXPERIMENT_SENSOR_CRC)
  #define SEU_ENABLE 1
  #define SEQ_TMR_ENABLE 1
  #define SENSOR_CRC_ENABLE 1
  #define QUEUE_PROTECT_ENABLE 1
  #define CLAMP_ENABLE 0
#elif defined(EXPERIMENT_CMD_CLAMP)
  #define SEU_ENABLE 1
  #define SEQ_TMR_ENABLE 1
  #define SENSOR_CRC_ENABLE 1
  #define QUEUE_PROTECT_ENABLE 1
  #define CLAMP_ENABLE 1
#elif defined(EXPERIMENT_FULL)
  #define SEU_ENABLE 1
  #define SEQ_TMR_ENABLE 1
  #define SENSOR_CRC_ENABLE 1
  #define QUEUE_PROTECT_ENABLE 1
  #define CLAMP_ENABLE 1
#elif defined(EXPERIMENT_DATA_BSS)
  #define SEU_IN_DATA_AND_BSS 1
  #define SEU_ENABLE 1
  #define SEQ_TMR_ENABLE 0
  #define SENSOR_CRC_ENABLE 0
  #define QUEUE_PROTECT_ENABLE 0
  #define CLAMP_ENABLE 0
#endif

#ifndef SEU_ENABLE
  #define SEU_ENABLE 0
#endif
#ifndef SEQ_TMR_ENABLE
  #define SEQ_TMR_ENABLE 0
#endif
#ifndef SENSOR_CRC_ENABLE
  #define SENSOR_CRC_ENABLE 0
#endif
#ifndef QUEUE_PROTECT_ENABLE
  #define QUEUE_PROTECT_ENABLE 0
#endif
#ifndef CLAMP_ENABLE
  #define CLAMP_ENABLE 0
#endif
#ifndef SEU_IN_DATA_AND_BSS
  #define SEU_IN_DATA_AND_BSS 0
#endif

#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "utils.h"

#define MAX_SEQ  20000
#define K  8
#define CMD_M_MAX  2000

extern uint32_t _sseu;
extern uint32_t _eseu;
extern uint32_t _sdata;
extern uint32_t _ebss;

#if SENSOR_CRC_ENABLE

  typedef struct
  {
    uint32_t seq;
    int32_t  bx;
    int32_t  by;
    int32_t  bz;
    uint8_t crc;
  } sensor_sample;

#else

  typedef struct
  {
    uint32_t seq;
    int32_t  bx;
    int32_t  by;
    int32_t  bz;
  } sensor_sample;
  #endif

typedef struct
{
  uint32_t seq;
  uint32_t prev_seq;
  int32_t  mx;
  int32_t  my;
  int32_t  mz;
} coil_cmd;

#if QUEUE_PROTECT_ENABLE
  typedef struct
  {
    uint32_t primary_ptr;
    uint8_t  primary_crc;
    uint32_t backup_ptr;
    uint8_t  backup_crc;
  } protected_queue_t;
  
  __attribute__((section(".seu_section")))
  static protected_queue_t sensor_samples;
  __attribute__((section(".seu_section")))
  static protected_queue_t coil_cmds;
#else
  __attribute__((section(".seu_section")))
  static QueueHandle_t sensor_samples;
  __attribute__((section(".seu_section")))
  static QueueHandle_t coil_cmds;
#endif

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


// zmienne do statystyk i referencji
static volatile uint32_t sample_count = 0;
static volatile uint32_t error_count = 0;
static volatile uint32_t max_error = 0;
static volatile int32_t seq_prev = -1;
static volatile uint32_t omitted_samples = 0;
static volatile uint32_t good_samples = 0;
static volatile uint32_t propagation_count = 0;
static volatile uint32_t seu_count = 0;
static volatile uint32_t expected_seq = 0;

// propagacja wykorzystywana do zaburzania kolejnych próbek
static volatile int32_t propagation_mx = 0;
static volatile int32_t propagation_my = 0;
static volatile int32_t propagation_mz = 0;

#if QUEUE_PROTECT_ENABLE || SENSOR_CRC_ENABLE

static uint8_t crc8(uint8_t *data, int len)
{
  uint8_t crc = 0;
  for (int i = 0; i < len; i++)
  {
    crc ^= data[i];
    for (int j = 0; j < 8; j++)
    {
      if (crc & 0x80)
      {
        crc = (crc << 1) ^ 0x07;
      }
      else
      {
        crc = crc << 1;
      }
    }
  }
  return crc;
}

#endif

#if SENSOR_CRC_ENABLE

static uint8_t crc8_sensor(sensor_sample *s)
{
  return crc8((uint8_t*)s, 16);
}

#endif

#if QUEUE_PROTECT_ENABLE

  static uint8_t crc8_ptr(uint32_t ptr)
  {
    return crc8((uint8_t*)&ptr, 4);
  }

  static void init_protected_queue(protected_queue_t *pq, QueueHandle_t q)
  {
    uint32_t ptr = (uint32_t)q;
    
    pq->primary_ptr = ptr;
    pq->primary_crc = crc8_ptr(ptr);
    
    pq->backup_ptr = ptr;
    pq->backup_crc = crc8_ptr(ptr);
  }

  static QueueHandle_t get_protected_queue(protected_queue_t *pq, const char *name)
  {
    uint32_t primary_ptr = pq->primary_ptr;
    uint32_t backup_ptr = pq->backup_ptr;
    
    uint8_t primary_crc = crc8_ptr(primary_ptr);
    uint8_t backup_crc = crc8_ptr(backup_ptr);
    
    int valid_primary = (primary_crc == pq->primary_crc);
    int valid_backup = (backup_crc == pq->backup_crc);
    
    if (valid_primary && valid_backup)
    {
      return (QueueHandle_t)primary_ptr;
    }
    
    if (valid_primary)
    {
      pq->backup_ptr = primary_ptr;
      pq->backup_crc = primary_crc;
      uart_puts("[");
      uart_puts(name);
      uart_puts(" backup CORRUPTED] backup repaired\r\n");
      return (QueueHandle_t)primary_ptr;
    }
    else if (valid_backup)
    {
      pq->primary_ptr = backup_ptr;
      pq->primary_crc = backup_crc;
      uart_puts("[");
      uart_puts(name);
      uart_puts(" CORRUPTED] primary repaired\r\n");
      return (QueueHandle_t)backup_ptr;
    }
    else
    {
      uart_puts("[");
      uart_puts(name);
      uart_puts(" CRITICAL] both pointers corrupted!\r\n");
      __asm volatile("udf #0");
      return NULL;
    }
  }
  
  static void set_sensor_samples(QueueHandle_t q)
  {
    init_protected_queue(&sensor_samples, q);
  }
  static QueueHandle_t get_sensor_samples(void)
  {
    return get_protected_queue(&sensor_samples, "SAMPLE QUEUE");
  }
  static void set_coil_cmds(QueueHandle_t q)
  {
    init_protected_queue(&coil_cmds, q);
  }
  static QueueHandle_t get_coil_cmds(void)
  {
    return get_protected_queue(&coil_cmds, "CMD QUEUE");
  }

#else

  #define set_sensor_samples(q) (sensor_samples = (q))
  #define get_sensor_samples() (sensor_samples)
  #define set_coil_cmds(q) (coil_cmds = (q))
  #define get_coil_cmds() (coil_cmds)

#endif

#if SEQ_TMR_ENABLE
// Chroniony licznik sekwencji
__attribute__((section(".seu_section")))
static uint32_t seq_tmr[3] = {0, 0, 0};

static uint32_t get_seq(void)
{
  uint32_t a = seq_tmr[0];
  uint32_t b = seq_tmr[1];
  uint32_t c = seq_tmr[2];

  if (a == b || a == c)
  {
    return a;
  }
  if (b == c)
  {
    return b;
  }

  uart_puts("[TMR FAILURE]\r\n");
  __asm volatile("udf #0");

  return 0;
}

static void inc_seq(void)
{
  uint32_t new_seq = get_seq() + 1;
  seq_tmr[0] = new_seq;
  seq_tmr[1] = new_seq;
  seq_tmr[2] = new_seq;

  /* zmienna do liczenia referencji */ expected_seq++;
}

#else
  __attribute__((section(".seu_section")))
  static uint32_t seq = 0;
  static uint32_t get_seq(void) { return seq; }
  static void inc_seq(void) { seq++; /* zmienna do liczenia referencji */ expected_seq++;}

#endif

#if CLAMP_ENABLE

static void clamp_cmd(coil_cmd *cmd)
{
  if (cmd->mx > CMD_M_MAX) cmd->mx = CMD_M_MAX;
  if (cmd->mx < -CMD_M_MAX) cmd->mx = -CMD_M_MAX;
  
  if (cmd->my > CMD_M_MAX) cmd->my = CMD_M_MAX;
  if (cmd->my < -CMD_M_MAX) cmd->my = -CMD_M_MAX;
  
  if (cmd->mz > CMD_M_MAX) cmd->mz = CMD_M_MAX;
  if (cmd->mz < -CMD_M_MAX) cmd->mz = -CMD_M_MAX;
}

#endif

// Obliczenia referencyjne, służą do badania zaburzeń w symulowanym układzie
static void compute_reference(uint32_t seq, int32_t seq_prev, int32_t *mx_ref, int32_t *my_ref, int32_t *mz_ref)
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

  *mx_ref = mx;
  *my_ref = my;
  *mz_ref = mz;
}

// SENSOR
static void task_sensor(void *arg)
{
  (void)arg;

  const int32_t BX = 20000;
  const int32_t BY = -5000;
  const int32_t BZ = 12000;

  while (1)
  {
    uint32_t seq = get_seq();
    int32_t dx = (int32_t)(seq & 0xFF) - 128;
    int32_t dy = (int32_t)((seq >> 1) & 0xFF) - 128;
    int32_t dz = (int32_t)((seq >> 2) & 0xFF) - 128;

    int32_t variation = (500 * sin_table[seq % 360]) / 1000;

    if (propagation_mx + propagation_my + propagation_mz != 0)
    {
      propagation_count++;
    }
    
    sample_sensor = (sensor_sample)
    {
      .seq = seq,
      .bx  = BX + dx + variation + propagation_mx,
      .by  = BY + dy + variation / 2 + propagation_my,
      .bz  = BZ + dz + variation / 3 + propagation_mz,
    };

    #if SENSOR_CRC_ENABLE

        sample_sensor.crc = crc8_sensor(&sample_sensor);

    #endif

    // Dłuższy czas na SEU, symulacja uszkodzenia w kolejce
    vTaskDelay(pdMS_TO_TICKS(1)); 
    if (xQueueSend(get_sensor_samples(), &sample_sensor, 0) != pdPASS) {
      uart_puts("[SAMPLE QUEUE FULL]\r\n");
    }

    sample_count++;
    vTaskDelay(pdMS_TO_TICKS(10));
    inc_seq();
  }
}

// CONTROLLER
static void task_controller(void *arg)
{
  (void)arg;

  // Pierwsza próbka jest porpawna, bo task_seu startuje z opóźnieniem
  if (xQueueReceive(get_sensor_samples(), &sample_controller, portMAX_DELAY) == pdPASS)
  {
    prev_sample = sample_controller;
    good_samples++;
  }

  while (1)
  {
    if (xQueueReceive(get_sensor_samples(), &sample_controller, portMAX_DELAY) != pdPASS) {continue;}

    #if SENSOR_CRC_ENABLE

      if (sample_controller.crc != crc8_sensor(&sample_controller))
      {
        omitted_samples++;
        propagation_mx = 0;
        propagation_my = 0;
        propagation_mz = 0;
        uart_puts("[SENSOR SAMPLE CORRUPTED] omitting sample\r\n");
        continue;
      }

    #endif

    dBx = sample_controller.bx - prev_sample.bx;
    dBy = sample_controller.by - prev_sample.by;
    dBz = sample_controller.bz - prev_sample.bz;

    mx = -K * dBx;
    my = -K * dBy;
    mz = -K * dBz;
    
    cmd_controller = (coil_cmd)
    {
      .seq = sample_controller.seq,
      .prev_seq = prev_sample.seq,
      .mx = mx,
      .my = my,
      .mz = mz,
    };

    // Dłuższy czas na SEU, symulacja uszkodzenia w kolejce
    vTaskDelay(pdMS_TO_TICKS(1)); 
    #if CLAMP_ENABLE

      clamp_cmd(&cmd_controller);

    #endif

    if (xQueueSend(get_coil_cmds(), &cmd_controller, 0) != pdPASS) {
      uart_puts("[CMD QUEUE FULL]\r\n");
    }
    prev_sample = sample_controller;
  }
}

// ACTUATOR + DETEKCJA BŁĘDÓW
static void task_actuator(void *arg)
{
  (void)arg;

  while (1)
  {
    if (xQueueReceive(get_coil_cmds(), &cmd_actuator, portMAX_DELAY) != pdPASS) {continue;}
    
    int32_t mx_ref, my_ref, mz_ref;

    compute_reference(expected_seq, cmd_actuator.prev_seq, &mx_ref, &my_ref, &mz_ref);

    uint32_t err = u32_abs_i32(cmd_actuator.mx - mx_ref) + u32_abs_i32(cmd_actuator.my - my_ref) + u32_abs_i32(cmd_actuator.mz - mz_ref);

    if (err > 0)
    {
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
    }
    else
    {
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

      uart_puts("SEUs=");
      uart_putdec_u32(seu_count);
      uart_puts("\r\n");

      uart_puts("good samples=");
      uart_putdec_u32(good_samples);
      uart_puts("\r\n"); 

      uart_puts("errors=");
      uart_putdec_u32(error_count);
      uart_puts("\r\n");
      
      uart_puts("samples omitted=");
      uart_putdec_u32(omitted_samples);
      uart_puts("\r\n");

      uart_puts("max_error=0x");
      uart_puthex_u32(max_error);
      uart_puts("\r\n");

      uart_puts("propagations=");
      uart_putdec_u32(propagation_count);
      uart_puts("\r\n");      

      __asm volatile("cpsid i");
      while (1) {}
    }
  }
}

// RNG + SEU
static uint32_t rng_state = 0x12345678;

static uint32_t xorshift32(void)
{
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
  while (1)
  {
    #if SEU_IN_DATA_AND_BSS

      uint8_t *start = (uint8_t*)&_sdata;
      uint8_t *end   = (uint8_t*)&_ebss;

    #else

      uint8_t *start = (uint8_t*)&_sseu;
      uint8_t *end   = (uint8_t*)&_eseu;

    #endif

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
    seu_count++;

    vTaskDelay((xorshift32() % 190) + 10);
  }
}

int main(void)
{
  uart_puts("START\n");

  set_sensor_samples(xQueueCreate(8, sizeof(sensor_sample)));
  set_coil_cmds(xQueueCreate(8, sizeof(coil_cmd)));

  xTaskCreate(task_sensor, "sensor", 256, NULL, 2, NULL);
  xTaskCreate(task_controller, "controller", 256, NULL, 2, NULL);
  xTaskCreate(task_actuator, "actuator", 256, NULL, 2, NULL);

#if SEU_ENABLE

  xTaskCreate(task_seu, "seu", 256, NULL, 3, NULL);

#endif

  vTaskStartScheduler();
  while (1) {}
}