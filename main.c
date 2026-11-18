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
#elif defined(EXPERIMENT_SENSOR_ECC)
  #define SEU_ENABLE 1
  #define QUEUE_PROTECT_ENABLE 1
  #define SEQ_TMR_ENABLE 1
  #define CLAMP_ENABLE 1
  #define SENSOR_ECC_ENABLE 1
#elif defined(EXPERIMENT_FULL)
  #define SEU_ENABLE 1
  #define SEQ_TMR_ENABLE 1
  #define SENSOR_ECC_ENABLE 1
  #define QUEUE_PROTECT_ENABLE 1
  #define CLAMP_ENABLE 1
  #define SENSOR_PREV_ECC_ENABLE 1
#elif defined(EXPERIMENT_DATA_BSS)
  #define SEU_IN_DATA_AND_BSS 1
  #define SEU_ENABLE 1
  #define SEQ_TMR_ENABLE 0
  #define SENSOR_ECC_ENABLE 0
  #define QUEUE_PROTECT_ENABLE 0
  #define CLAMP_ENABLE 0
#endif

#ifndef SEU_ENABLE
  #define SEU_ENABLE 0
#endif
#ifndef SEQ_TMR_ENABLE
  #define SEQ_TMR_ENABLE 0
#endif
#ifndef SENSOR_ECC_ENABLE
  #define SENSOR_ECC_ENABLE 0
#endif
#ifndef QUEUE_PROTECT_ENABLE
  #define QUEUE_PROTECT_ENABLE 0
#endif
#ifndef CLAMP_ENABLE
  #define CLAMP_ENABLE 0
#endif
#ifndef SENSOR_PREV_ECC_ENABLE
  #define SENSOR_PREV_ECC_ENABLE 0
#endif
#ifndef SEU_IN_DATA_AND_BSS
  #define SEU_IN_DATA_AND_BSS 0
#endif

#if SENSOR_PREV_ECC_ENABLE && !SENSOR_ECC_ENABLE
#error "SENSOR_PREV_ECC_ENABLE requires SENSOR_ECC_ENABLE"
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

typedef struct
{
    uint32_t seq;
    int32_t bx;
    int32_t by;
    int32_t bz;
#if SENSOR_ECC_ENABLE
    uint8_t ecc[4];
#endif
} sensor_sample;

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

static coil_cmd debug_cmd_after_clamp;

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
static volatile uint32_t ecc_corrections = 0;
static volatile uint32_t ecc_uncorrectable = 0;
static volatile uint32_t ecc_sensor_corrections = 0;
static volatile uint32_t ecc_prev_corrections = 0;
static volatile uint32_t detected_disturbances = 0;

static SemaphoreHandle_t sample_completed;

#if QUEUE_PROTECT_ENABLE

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
  if (cmd->mx > CMD_M_MAX)
  {
      clamp_activations++;
      detected_disturbances++;
      cmd->mx = CMD_M_MAX;
  }
  else if (cmd->mx < -CMD_M_MAX)
  {
      clamp_activations++;
      detected_disturbances++;
      cmd->mx = -CMD_M_MAX;
  }

  if (cmd->my > CMD_M_MAX)
  {
      clamp_activations++;
      detected_disturbances++;
      cmd->my = CMD_M_MAX;
  }
  else if (cmd->my < -CMD_M_MAX)
  {
      clamp_activations++;
      detected_disturbances++;
      cmd->my = -CMD_M_MAX;
  }

  if (cmd->mz > CMD_M_MAX)
  {
      clamp_activations++;
      detected_disturbances++;
      cmd->mz = CMD_M_MAX;
  }
  else if (cmd->mz < -CMD_M_MAX)
  {
      clamp_activations++;
      detected_disturbances++;
      cmd->mz = -CMD_M_MAX;
  }
}

#endif

#if SENSOR_ECC_ENABLE

typedef enum
{
    ECC_OK,
    ECC_CORRECTED,
    ECC_UNCORRECTABLE
} ecc_result_t;

// Zwraca 1 przy nieparzystej liczbie jedynek
static uint8_t parity32(uint32_t x)
{
    x ^= x >> 16;
    x ^= x >> 8;
    x ^= x >> 4;
    x ^= x >> 2;
    x ^= x >> 1;
    return (uint8_t)(x & 1);
}

// Generuje SEC-DED dla 32-bitowej wartości
static uint8_t ecc_encode32(uint32_t data)
{
  uint8_t ecc = 0;
  uint32_t bit = 0;
  // 32 bity danych + 6 pozycji Hamminga (pozycje 1 do 38)
  for (uint32_t pos = 1; pos <= 38; pos++)
  {
    // Sprawdź czy potęga dwójki
    if ((pos & (pos - 1)) == 0)
      continue;
    if ((data >> bit) & 1)
    {
      ecc ^= (uint8_t)pos;
    }
    bit++;
  }
  uint8_t global = parity32(data) ^ parity32(ecc & 0b00111111);
  return (uint8_t)((ecc & 0b00111111) | (global << 7));
}

// Sprawdza i ewentualnie naprawia 32-bitową wartość
static ecc_result_t ecc_check32(uint32_t *data, uint8_t *ecc)
{
  uint8_t old_ecc = *ecc;
  uint32_t current_data = *data;
  uint8_t new_ecc = ecc_encode32(current_data);
  uint8_t syndrome = (old_ecc ^ new_ecc) & 0b00111111;

  uint8_t old_global = (old_ecc >> 7) & 1u;
  uint8_t new_global = parity32(current_data) ^ parity32(old_ecc & 0b00111111);
  uint8_t global_bad = (old_global != new_global);

  if (syndrome == 0 && !global_bad)
  {
    return ECC_OK;
  }

  // Dwa błędy: syndrom != 0, a globalna parzystość się zgadza
  if (syndrome != 0 && !global_bad)
  {
    return ECC_UNCORRECTABLE;
  }

  // Uszkodzony sam bit globalnej parzystości
  if (syndrome == 0 && global_bad)
  {
    *ecc = new_ecc;
    return ECC_CORRECTED;
  }

  // Uszkodzony jeden z bitów ECC
  if ((syndrome & (syndrome - 1)) == 0)
  {
    *ecc = new_ecc;
    return ECC_CORRECTED;
  }

  // Uszkodzony jeden bit danych
  uint32_t parity_positions = 0;
  uint32_t temp = syndrome;
  while (temp > 1) 
  {
    temp >>= 1;
    parity_positions++;
  }
  uint32_t bit = syndrome - parity_positions - 2;
  if (bit >= 32)
  {
    return ECC_UNCORRECTABLE;
  }
  current_data ^= (1 << bit);

  *data = current_data;
  *ecc = ecc_encode32(current_data);
  return ECC_CORRECTED;
}

typedef enum
{
    ECC_SENSOR,
    ECC_PREV_SAMPLE
} ecc_source_t;

static void ecc_check_sensor(sensor_sample *sample, ecc_source_t source)
{
    sensor_sample corrected = *sample;

    uint32_t data[4] =
    {
        corrected.seq,
        (uint32_t)corrected.bx,
        (uint32_t)corrected.by,
        (uint32_t)corrected.bz
    };

    for (uint32_t i = 0; i < 4; i++)
    {
        ecc_result_t result =
            ecc_check32(&data[i], &corrected.ecc[i]);

        if (result == ECC_UNCORRECTABLE)
        {
            ecc_uncorrectable++;
            detected_disturbances++;

            uart_puts("[ECC UNCORRECTABLE]\r\n");
            qemu_exit(1);
        }

        if (result == ECC_CORRECTED)
        {
            ecc_corrections++;
            detected_disturbances++;

            if (source == ECC_SENSOR)
            {
                ecc_sensor_corrections++;
                uart_puts("[SENSOR ECC CORRECTED]\r\n");
            }
            else
            {
                ecc_prev_corrections++;
                uart_puts("[PREV SAMPLE ECC CORRECTED]\r\n");
            }
        }
    }

    corrected.seq = data[0];
    corrected.bx  = (int32_t)data[1];
    corrected.by  = (int32_t)data[2];
    corrected.bz  = (int32_t)data[3];

    *sample = corrected;
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

  uart_puts("[ADDR] cmd_controller.my=");
  uart_puthex_u32((uint32_t)&cmd_controller.my);
  uart_puts("\r\n");

  uart_puts("[ADDR] cmd_actuator.my=");
  uart_puthex_u32((uint32_t)&cmd_actuator.my);
  uart_puts("\r\n");

  uart_puts("[ADDR] moment_y=");
  uart_puthex_u32((uint32_t)&moment_y);
  uart_puts("\r\n");

  uart_puts("[ADDR] prev_sample.by=");
  uart_puthex_u32((uint32_t)&prev_sample.by);
  uart_puts("\r\n");

  uart_puts("[ADDR] sample_controller.by=");
  uart_puthex_u32((uint32_t)&sample_controller.by);
  uart_puts("\r\n");

  uart_puts("[ADDR] sample_sensor.bz=");
  uart_puthex_u32((uint32_t)&sample_sensor.bz);
  uart_puts("\r\n");

  uart_puts("[ADDR] sample_controller.bz=");
  uart_puthex_u32((uint32_t)&sample_controller.bz);
  uart_puts("\r\n");

  uart_puts("[ADDR] prev_sample.bz=");
  uart_puthex_u32((uint32_t)&prev_sample.bz);
  uart_puts("\r\n");

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
    
    #if SENSOR_ECC_ENABLE
        sample_sensor.ecc[0] = ecc_encode32(sample_sensor.seq);
        sample_sensor.ecc[1] = ecc_encode32(sample_sensor.bx);
        sample_sensor.ecc[2] = ecc_encode32(sample_sensor.by);
        sample_sensor.ecc[3] = ecc_encode32(sample_sensor.bz);
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

  if (xQueueReceive(get_sensor_samples(), &sample_controller, portMAX_DELAY) == pdPASS)
  {
  #if SENSOR_ECC_ENABLE
      ecc_check_sensor(&sample_controller, ECC_SENSOR);
  #endif

    prev_sample = sample_controller;
    good_samples++;
    xSemaphoreGive(sample_completed);
  }

  while (1)
  {
    if (xQueueReceive(get_sensor_samples(), &sample_controller, portMAX_DELAY) != pdPASS) {continue;}

    #if SENSOR_ECC_ENABLE
        ecc_check_sensor(&sample_controller, ECC_SENSOR);
    #endif


    #if SENSOR_PREV_ECC_ENABLE
        ecc_check_sensor(&prev_sample, ECC_PREV_SAMPLE);
    #endif

    diff_x = saturate_int32((int64_t)sample_controller.bx - prev_sample.bx);
    diff_y = saturate_int32((int64_t)sample_controller.by - prev_sample.by);
    diff_z = saturate_int32((int64_t)sample_controller.bz - prev_sample.bz);

    moment_x = saturate_int32(-(int64_t)K * diff_x);
    moment_y = saturate_int32(-(int64_t)K * diff_y);
    moment_z = saturate_int32(-(int64_t)K * diff_z);

    cmd_controller = (coil_cmd)
    {
        .seq = sample_controller.seq,
        .prev_seq = prev_sample.seq,
        .mx = moment_x,
        .my = moment_y,
        .mz = moment_z
    };

    #if CLAMP_ENABLE
        clamp_cmd(&cmd_controller);
    #endif
    debug_cmd_after_clamp = cmd_controller;

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

      uart_puts("[DEBUG ERR] seq=");
      uart_putdec_u32(cmd_actuator.seq);

      uart_puts(" prev_seq=");
      uart_putdec_u32(cmd_actuator.prev_seq);

      uart_puts(" expected_seq=");
      uart_putdec_u32(expected_seq);

      uart_puts("\r\n");

      uart_puts("[DEBUG CMD] in actuator mx=");
      uart_puthex_i32(cmd_actuator.mx);

      uart_puts(" my=");
      uart_puthex_i32(cmd_actuator.my);

      uart_puts(" mz=");
      uart_puthex_i32(cmd_actuator.mz);

      uart_puts("\r\n");

      uart_puts("After clamp mx=");
      uart_puthex_i32(debug_cmd_after_clamp.mx);

      uart_puts(" my=");
      uart_puthex_i32(debug_cmd_after_clamp.my);

      uart_puts(" mz=");
      uart_puthex_i32(debug_cmd_after_clamp.mz);

      uart_puts("\r\n");

      uart_puts("[DEBUG REF] mx=");
      uart_puthex_i32(mx_ref);

      uart_puts(" my=");
      uart_puthex_i32(my_ref);

      uart_puts(" mz=");
      uart_puthex_i32(mz_ref);

      uart_puts("\r\n");

      uart_puts("[DEBUG DIFF] ex=");
      uart_putdec_u64((uint64_t)ex);

      uart_puts(" ey=");
      uart_putdec_u64((uint64_t)ey);

      uart_puts(" ez=");
      uart_putdec_u64((uint64_t)ez);

      uart_puts("\r\n");
    }
    else
    {
      good_samples++;
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

        uart_puts("ecc corrections=");
        uart_putdec_u32(ecc_corrections);
        uart_puts("\r\n");

        uart_puts("ecc sensor corrections=");
        uart_putdec_u32(ecc_sensor_corrections);
        uart_puts("\r\n");

        uart_puts("ecc prev corrections=");
        uart_putdec_u32(ecc_prev_corrections);
        uart_puts("\r\n");

        uart_puts("ecc uncorrectable=");
        uart_putdec_u32(ecc_uncorrectable);
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
