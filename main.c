#ifdef EXPERIMENT_BASELINE
#elif defined(EXPERIMENT_SEU_ONLY)
  #define SEU_ENABLE 1
#elif defined(EXPERIMENT_QUEUE_PROTECT)
  #define SEU_ENABLE 1
  #define QUEUE_PROTECT_ENABLE 1
#elif defined(EXPERIMENT_SEQ_TMR)
  #define SEU_ENABLE 1
  #define QUEUE_PROTECT_ENABLE 1
  #define SEQ_TMR_ENABLE 1
#elif defined(EXPERIMENT_CMD_CLAMP)
  #define SEU_ENABLE 1
  #define QUEUE_PROTECT_ENABLE 1
  #define SEQ_TMR_ENABLE 1
  #define CLAMP_ENABLE 1
#elif defined(EXPERIMENT_SENSOR_CRC)
  #define SEU_ENABLE 1
  #define QUEUE_PROTECT_ENABLE 1
  #define SEQ_TMR_ENABLE 1
  #define CLAMP_ENABLE 1
  #define SENSOR_CRC_ENABLE 1
#elif defined(EXPERIMENT_FULL)
  #define SEU_ENABLE 1
  #define SEQ_TMR_ENABLE 1
  #define SENSOR_CRC_ENABLE 1
  #define QUEUE_PROTECT_ENABLE 1
  #define CLAMP_ENABLE 1
  #define SENSOR_PREV_CRC_ENABLE 1
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
#ifndef SENSOR_PREV_CRC_ENABLE
  #define SENSOR_PREV_CRC_ENABLE 0
#endif
#ifndef SEU_IN_DATA_AND_BSS
  #define SEU_IN_DATA_AND_BSS 0
#endif

#ifndef SEU_SEED
#define SEU_SEED 0x12345678u
#endif

#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "utils.h"
#include "semphr.h"

#ifndef MAX_SEQ
#define MAX_SEQ 1000
#endif
#define K  8
#define CMD_M_MAX  2500

#define SENSOR_MS        10
#define SEU_START_DELAY_MS      50
#define SEU_MIN_INTERVAL_MS     10
#define SEU_MAX_INTERVAL_MS     100

extern uint32_t _sseu;
extern uint32_t _eseu;
extern uint32_t _sdata;
extern uint32_t _ebss;

#if SENSOR_CRC_ENABLE || SENSOR_PREV_CRC_ENABLE
  // próbka sensora z CRC
  typedef struct
  {
    uint32_t seq;
    int32_t  bx;
    int32_t  by;
    int32_t  bz;
    uint8_t crc;
  } sensor_sample;

#else
  // próbka sensora bez ochrony
  typedef struct
  {
    uint32_t seq;
    int32_t  bx;
    int32_t  by;
    int32_t  bz;
  } sensor_sample;
#endif
// komenda cewek
typedef struct
{
  uint32_t seq;
  uint32_t prev_seq;
  int32_t  mx;
  int32_t  my;
  int32_t  mz;
} coil_cmd;

#if QUEUE_PROTECT_ENABLE
  // ochrona wskaźników kolejek DMR + CRC
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
  // kolejki bez ochrony
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
static int32_t diff_x;
__attribute__((section(".seu_section")))
static int32_t diff_y;
__attribute__((section(".seu_section")))
static int32_t diff_z;
__attribute__((section(".seu_section")))
static int32_t moment_x;
__attribute__((section(".seu_section")))
static int32_t moment_y;
__attribute__((section(".seu_section")))
static int32_t moment_z;

// zmienne do statystyk i referencji
static volatile uint32_t sample_count = 0;
static volatile uint32_t error_count = 0;
static volatile uint64_t max_error = 0;
static volatile uint32_t omitted_samples = 0;
static volatile uint32_t good_samples = 0;
static volatile uint32_t seu_count = 0;
static volatile uint32_t expected_seq = 0;
static volatile uint32_t tmr_corrections = 0;
static volatile uint32_t dmr_corrections = 0;
static volatile uint32_t clamp_activations = 0;
static volatile uint32_t crc_detections = 0;
static volatile uint32_t detected_disturbances = 0;

static SemaphoreHandle_t sample_completed;

#if QUEUE_PROTECT_ENABLE || SENSOR_CRC_ENABLE || SENSOR_PREV_CRC_ENABLE

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

#if SENSOR_CRC_ENABLE || SENSOR_PREV_CRC_ENABLE

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
      detected_disturbances++;
      dmr_corrections++;
      pq->backup_ptr = primary_ptr;
      pq->backup_crc = primary_crc;
      uart_puts("[");
      uart_puts(name);
      uart_puts(" backup CORRUPTED] backup repaired\r\n");
      return (QueueHandle_t)primary_ptr;
    }
    else if (valid_backup)
    {
      detected_disturbances++;
      dmr_corrections++;
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
      qemu_exit(1);
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
  // funkcje dla kolejek bez ochrony
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

  if (a == b && b == c)
  {
    return a;
  }

  if (a == b)
  {
    seq_tmr[1] = a;
    seq_tmr[2] = a;
    detected_disturbances++;
    tmr_corrections++;
    return a;
  }

  if (a == c)
  {
    seq_tmr[1] = a;
    seq_tmr[2] = a;
    detected_disturbances++;
    tmr_corrections++;
    return a;
  }

  if (b == c)
  {
    seq_tmr[0] = b;
    seq_tmr[2] = b;
    detected_disturbances++;
    tmr_corrections++;
    return b;
  }

  uart_puts("[TMR FAILURE]\r\n");
  qemu_exit(1);
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
  uint8_t detection = 0;
  if (cmd->mx > CMD_M_MAX)
  {
    detection=1;
    ++clamp_activations;
    cmd->mx = CMD_M_MAX;
  }
  if (cmd->mx < -CMD_M_MAX)
  {
    detection=1;
    ++clamp_activations;
    cmd->mx = -CMD_M_MAX;
  }
  if (cmd->my > CMD_M_MAX)
  {
    detection=1;
    ++clamp_activations;
    cmd->my = CMD_M_MAX;
  }
  if (cmd->my < -CMD_M_MAX)
  {
    detection=1;
    ++clamp_activations;
    cmd->my = -CMD_M_MAX;
  }
  if (cmd->mz > CMD_M_MAX)
  {
    detection=1;
    ++clamp_activations;
    cmd->mz = CMD_M_MAX;
  }  
  if (cmd->mz < -CMD_M_MAX)
  {
    detection=1;
    ++clamp_activations;
    cmd->mz = -CMD_M_MAX;
  }

  if (detection) {
    detected_disturbances++;
  }
}

#endif


// Obliczenia referencyjne, służą do badania zaburzeń w symulowanym układzie
static void compute_reference(uint32_t seq, uint32_t seq_prev, int32_t *mx_ref, int32_t *my_ref, int32_t *mz_ref)
{
  const int32_t BX = 20000;
  const int32_t BY = -5000;
  const int32_t BZ = 12000;

  int32_t dx  = (int32_t)(seq & 0xFFu) - 128;
  int32_t dy  = (int32_t)((seq >> 1) & 0xFFu) - 128;
  int32_t dz  = (int32_t)((seq >> 2) & 0xFFu) - 128;

  int32_t variation = (500 * sin_table[seq % 356]) / 1000;

  int32_t bx  = BX + dx + variation;
  int32_t by  = BY + dy + variation / 2;
  int32_t bz  = BZ + dz + variation / 3;

  int32_t dxp = (int32_t)(seq_prev & 0xFFu) - 128;
  int32_t dyp = (int32_t)((seq_prev >> 1) & 0xFFu) - 128;
  int32_t dzp = (int32_t)((seq_prev >> 2) & 0xFFu) - 128;

  int32_t variation_prev = (500 * sin_table[seq_prev % 356]) / 1000;

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

// SENSOR, generuje próbki
static void task_sensor(void *arg)
{
  (void)arg;

  // wartości bazowe czujnika
  const int32_t BX = 20000;
  const int32_t BY = -5000;
  const int32_t BZ = 12000;

  while (1)
  {
    uint32_t seq = get_seq();
    
    // symulacja poruszania się satelity
    int32_t dx = (int32_t)(seq & 0xFF) - 128;
    int32_t dy = (int32_t)((seq >> 1) & 0xFF) - 128;
    int32_t dz = (int32_t)((seq >> 2) & 0xFF) - 128;
    
    // sinusoidalna zmiana, szum
    int32_t variation = (500 * sin_table[seq % 356]) / 1000;
    
    sample_sensor = (sensor_sample)
    {
      .seq = seq,
      .bx  = BX + dx + variation,
      .by  = BY + dy + variation / 2,
      .bz  = BZ + dz + variation / 3
    };
    
    #if SENSOR_CRC_ENABLE
        // oblicz crc przed wysłaniem próbki
        sample_sensor.crc = crc8_sensor(&sample_sensor);
    #endif
    
    xQueueSend(get_sensor_samples(), &sample_sensor, portMAX_DELAY);

    sample_count++;
    xSemaphoreTake(sample_completed, portMAX_DELAY);
    inc_seq();
    vTaskDelay(pdMS_TO_TICKS(SENSOR_MS));
  }
}

// CONTROLLER, przetwarza próbki na komendy dla cewek
static void task_controller(void *arg)
{
  (void)arg;

  // Pierwsza próbka jest porpawna, bo task_seu startuje z opóźnieniem
  if (xQueueReceive(get_sensor_samples(), &sample_controller, portMAX_DELAY) == pdPASS)
  {
    prev_sample = sample_controller;
    good_samples++;
    xSemaphoreGive(sample_completed);
  }

  while (1)
  {
    if (xQueueReceive(get_sensor_samples(), &sample_controller, portMAX_DELAY) != pdPASS) {continue;}

    #if SENSOR_CRC_ENABLE
      // weryfikacja poprawności próbki
      if (sample_controller.crc != crc8_sensor(&sample_controller))
      {
        crc_detections++;
        detected_disturbances++;
        omitted_samples++;
        uart_puts("[SENSOR SAMPLE CORRUPTED] omitting sample\r\n");
        xSemaphoreGive(sample_completed);
        continue;
      }
    #endif

    #if SENSOR_PREV_CRC_ENABLE
      if (prev_sample.crc != crc8_sensor(&prev_sample))
      {
        prev_sample = sample_controller;
        crc_detections++;
        detected_disturbances++;
        omitted_samples++;
        uart_puts("[PREVIOUS SENSOR SAMPLE CORRUPTED] omitting sample\r\n");
        xSemaphoreGive(sample_completed);
        continue;
      }
    #endif

    diff_x = saturate_int32((int64_t)sample_controller.bx - prev_sample.bx);
    diff_y = saturate_int32((int64_t)sample_controller.by - prev_sample.by);
    diff_z = saturate_int32((int64_t)sample_controller.bz - prev_sample.bz);

    moment_x = -(int64_t)K * diff_x;
    moment_y = -(int64_t)K * diff_y;
    moment_z = -(int64_t)K * diff_z;

    cmd_controller = (coil_cmd)
    {
        .seq = sample_controller.seq,
        .prev_seq = prev_sample.seq,
        .mx = saturate_int32(moment_x),
        .my = saturate_int32(moment_y),
        .mz = saturate_int32(moment_z)
    };

    #if CLAMP_ENABLE
        clamp_cmd(&cmd_controller);
    #endif

    xQueueSend(get_coil_cmds(), &cmd_controller, portMAX_DELAY);

    prev_sample = sample_controller;
  }
}

// ACTUATOR, wykonuje komendy i wykrywa błędy
static void task_actuator(void *arg)
{
  (void)arg;

  while (1)
  {
    if (xQueueReceive(get_coil_cmds(), &cmd_actuator, portMAX_DELAY) != pdPASS) {continue;}
  
    int32_t mx_ref, my_ref, mz_ref;

    // oblicz referencję
    compute_reference(expected_seq, cmd_actuator.prev_seq, &mx_ref, &my_ref, &mz_ref);

    // błąd = suma bezwzględnych różnic na każdej osi
    int64_t ex = (int64_t)cmd_actuator.mx - (int64_t)mx_ref;
    int64_t ey = (int64_t)cmd_actuator.my - (int64_t)my_ref;
    int64_t ez = (int64_t)cmd_actuator.mz - (int64_t)mz_ref;
    if (ex < 0) ex = -ex;
    if (ey < 0) ey = -ey;
    if (ez < 0) ez = -ez;

    uint64_t err = (uint64_t)ex + (uint64_t)ey + (uint64_t)ez;

    if (err > 0)
    {
      error_count++;

      if (err > max_error) {
        max_error = err;
      }
      
      uart_puts("[ERR] seq=");
      uart_putdec_u32(cmd_actuator.seq);
      uart_puts(" err=");
      uart_putdec_u64(err);
      uart_puts("\r\n");
    }
    else
    {
      good_samples++;
    }

    if (err > 1000000)
    {
        uart_puts("[DEBUG] mx=");
        uart_puthex_i32(cmd_actuator.mx);

        uart_puts(" my=");
        uart_puthex_i32(cmd_actuator.my);

        uart_puts(" mz=");
        uart_puthex_i32(cmd_actuator.mz);

        uart_puts(" mx_ref=");
        uart_puthex_i32(mx_ref);

        uart_puts(" my_ref=");
        uart_puthex_i32(my_ref);

        uart_puts(" mz_ref=");
        uart_puthex_i32(mz_ref);

        uart_puts("\r\n");
    }

    if (sample_count >= MAX_SEQ)
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

        uart_puts("max error=");
        uart_putdec_u64(max_error);
        uart_puts("\r\n");

        uart_puts("detected disturbances=");
        uart_putdec_u32(detected_disturbances);
        uart_puts("\r\n");

        uart_puts("tmr corrections=");
        uart_putdec_u32(tmr_corrections);
        uart_puts("\r\n");

        uart_puts("dmr corrections=");
        uart_putdec_u32(dmr_corrections);
        uart_puts("\r\n");

        uart_puts("crc detections=");
        uart_putdec_u32(crc_detections);
        uart_puts("\r\n");

        uart_puts("clamp activations=");
        uart_putdec_u32(clamp_activations);
        uart_puts("\r\n");

        qemu_exit(0);
    }

    xSemaphoreGive(sample_completed);
  }
}

#if SEU_ENABLE
// RNG + SEU
static uint32_t rng_state = SEU_SEED;

static uint32_t xorshift32(void)
{
  uint32_t x = rng_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rng_state = x;
  return x;
}

// zadanie wstrzykujące błędy SEU
static void task_seu(void *arg) {
  (void)arg;

  // opóźnienie startu, pierwsze próbki są poprawne
  vTaskDelay(pdMS_TO_TICKS(SEU_START_DELAY_MS)); 
  while (1)
  {
    #if SEU_IN_DATA_AND_BSS
      // uszkadzaj sekcje .data i .bss
      uint8_t *start = (uint8_t*)&_sdata;
      uint8_t *end   = (uint8_t*)&_ebss;

    #else
      // uszkadzaj sekcję .seu_section
      uint8_t *start = (uint8_t*)&_sseu;
      uint8_t *end   = (uint8_t*)&_eseu;

    #endif

    uint32_t range = end - start;

    // losowy bajt
    uint32_t byte = xorshift32() % range;
    // losowy bit
    uint8_t bit = xorshift32() % 8;

    uint8_t *target = start + byte;

    uart_puts("[SEU] addr=");
    uart_puthex_u32((uint32_t)target);
    uart_puts(" bit=");
    uart_puthex_u32(bit);
    uart_puts("\r\n");

    // odwróć bit
    *target ^= (1 << bit);
    seu_count++;

    // losowe opóźnienie od SEU_MIN_INTERVAL_MS ms do SEU_MAX_INTERVAL_MS ms
    vTaskDelay(pdMS_TO_TICKS(SEU_MIN_INTERVAL_MS + (xorshift32() % (SEU_MAX_INTERVAL_MS - SEU_MIN_INTERVAL_MS + 1))));
  }
}
#endif

int main(void)
{
  uart_puts("START\r\n");
  uart_puts("SEED=");
  uart_puthex_u32(SEU_SEED);
  uart_puts("\r\n");

  set_sensor_samples(xQueueCreate(8, sizeof(sensor_sample)));
  set_coil_cmds(xQueueCreate(8, sizeof(coil_cmd)));
  sample_completed = xSemaphoreCreateBinary();

  xTaskCreate(task_sensor, "sensor", 256, NULL, 2, NULL);
  xTaskCreate(task_controller, "controller", 256, NULL, 2, NULL);
  xTaskCreate(task_actuator, "actuator", 256, NULL, 2, NULL);

#if SEU_ENABLE
  xTaskCreate(task_seu, "seu", 256, NULL, 3, NULL);
#endif

  vTaskStartScheduler();

  while (1) {}
}
