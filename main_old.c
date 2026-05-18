#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#define UART0_BASE 0x4000C000u
#define UARTDR     (*(volatile uint32_t *)(UART0_BASE + 0x000u))
#define UARTFR     (*(volatile uint32_t *)(UART0_BASE + 0x018u))

// Okres próbkowania sensora (symulacja)
#define SAMPLE_PERIOD_MS  5u
// Limit próbek, po którym kończymy eksperyment i wypisujemy statystyki
#define MAX_SEQ  20000u
// m = -K * dB
#define K  8
// Saturacja komendy momentu magnetycznego (symulacja ograniczeń aktuatora)
#define CMD_M_MAX  2000

#ifndef SEU_ENABLE
#define SEU_ENABLE 0
#endif

extern uint32_t _sdata;
extern uint32_t _ebss;

// Próbka sensora (syntetyczne pole B)
typedef struct
{
  uint32_t seq;
  int32_t  bx;
  int32_t  by;
  int32_t  bz;
} sensor_sample;

// Komenda cewek (moment magnetyczny m) + flagi saturacji
typedef struct
{
  uint32_t seq;
  int32_t  mx;
  int32_t  my;
  int32_t  mz;
  uint32_t sat_flags; // bity: 0=X, 1=Y, 2=Z (czy dana oś weszła w saturację)
} coil_cmd;

// Kolejka: sensor -> controller
static QueueHandle_t sensor_samples;
// Kolejka: controller -> actuator
static QueueHandle_t coil_cmds;

volatile uint32_t hardfault_count = 0;
static volatile uint32_t sample_count = 0;
static volatile uint32_t sat_count = 0;

// Wyślij pojedynczy znak na UART
static void uart_putc(char c) {
  while (UARTFR & (1u << 5)) {}
  UARTDR = (uint32_t)c;
}

// Wyślij string na UART
static void uart_puts(const char *s) {
  while (*s) uart_putc(*s++);
}

// Wypisz uint32 jako 8 hex znaków
static void uart_puthex_u32(uint32_t v) {
  static const char *HEX = "0123456789ABCDEF";
  for (int i = 7; i >= 0; --i) {
    uart_putc(HEX[(v >> (i * 4)) & 0xFu]);
  }
}

// Wypisz int32 jako hex
static void uart_puthex_i32(int32_t v) {
  uart_puthex_u32((uint32_t)v);
}

// Absolutna wartość int32 zwracana jako uint32
static uint32_t u32_abs_i32(int32_t x) {
  uint32_t ux = (uint32_t)x;
  return (x < 0) ? (uint32_t)(~ux + 1u) : (uint32_t)ux;
}

// Task sensor: generuje syntetyczne próbki pola B i wrzuca do kolejki
static void task_sensor(void *arg) {
  (void)arg;

  uint32_t seq = 0;
  // Bazowe pole (stałe), do którego dodajemy małą deterministyczną wariację
  const int32_t BX0 = 20000;
  const int32_t BY0 = -5000;
  const int32_t BZ0 = 12000;

  while (1)
  {
    // Małe deterministyczne zaburzenia zależne od seq (żeby przebieg nie był idealnie stały)
    int32_t dx = (int32_t)(seq & 0xFFu) - 128;
    int32_t dy = (int32_t)((seq >> 1) & 0xFFu) - 128;
    int32_t dz = (int32_t)((seq >> 2) & 0xFFu) - 128;

    // Próbka magnetometru
    sensor_sample s = {
      .seq = seq++,
      .bx  = BX0 + dx,
      .by  = BY0 + dy,
      .bz  = BZ0 + dz
    };

    // Wyślij próbkę do kontrolera
    if (xQueueSend(sensor_samples, &s, 0) != pdPASS) {
      uart_puts("[ERROR] samples queue full\r\n");
    }

    vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
  }
}

// Task controller: bierze próbki B, liczy dB i wysyła komendy cewek do aktuatora
static void task_controller(void *arg)
{
  (void)arg;

  sensor_sample prev_sample;
  sensor_sample curr_sample;

  // Pobierz pierwszą próbkę, żeby zainicjalizować prev_sample
  if (xQueueReceive(sensor_samples, &curr_sample, portMAX_DELAY) == pdPASS)
  {
    prev_sample = curr_sample;
  }

  while (1) {
    // Czekaj na kolejną próbkę sensora
    if (xQueueReceive(sensor_samples, &curr_sample, portMAX_DELAY) != pdPASS)
    {
      continue;
    }

    // Różnica pola: dB = B(seq) - B(seq-1)
    const int32_t dBx = curr_sample.bx - prev_sample.bx;
    const int32_t dBy = curr_sample.by - prev_sample.by;
    const int32_t dBz = curr_sample.bz - prev_sample.bz;

    // Prosta kontrola: m = -K * dB
    int32_t mx = -(int32_t)K * dBx;
    int32_t my = -(int32_t)K * dBy;
    int32_t mz = -(int32_t)K * dBz;

    // Saturacja komendy + flagi, które osie zostały ograniczone
    uint32_t sat = 0;
    if (mx >  CMD_M_MAX) { mx =  CMD_M_MAX; sat |= 1u; }
    if (mx < -CMD_M_MAX) { mx = -CMD_M_MAX; sat |= 1u; }
    if (my >  CMD_M_MAX) { my =  CMD_M_MAX; sat |= 2u; }
    if (my < -CMD_M_MAX) { my = -CMD_M_MAX; sat |= 2u; }
    if (mz >  CMD_M_MAX) { mz =  CMD_M_MAX; sat |= 4u; }
    if (mz < -CMD_M_MAX) { mz = -CMD_M_MAX; sat |= 4u; }

    // Zbuduj komendę dla aktuatora
    coil_cmd cmd =
    {
      .seq = curr_sample.seq,
      .mx = mx,
      .my = my,
      .mz = mz,
      .sat_flags = sat
    };

    // Wyślij komendę do aktuatora
    if (xQueueSend(coil_cmds, &cmd, 0) != pdPASS) {
      uart_puts("[ERROR] cmds queue full\r\n");
    }

    prev_sample = curr_sample;
  }
}

// Task actuator: odbiera komendy
static void task_actuator(void *arg) {
  (void)arg;

  coil_cmd cmd;

  // Ile razy wystąpiła saturacja
  uint32_t sat_total = 0;

  while (1) {
    // Czekaj na komendę z kontrolera
    if (xQueueReceive(coil_cmds, &cmd, portMAX_DELAY) != pdPASS) {
      continue;
    }

    sample_count++;

    // Zlicz próbki z saturacją
    if (cmd.sat_flags)
    {
      sat_total++;
    }

    // A(seq) = max(|mx|,|my|,|mz|)
    uint32_t amx = u32_abs_i32(cmd.mx);
    uint32_t amy = u32_abs_i32(cmd.my);
    uint32_t amz = u32_abs_i32(cmd.mz);
    uint32_t amax = amx;
    if (amy > amax) amax = amy;
    if (amz > amax) amax = amz;

    // Log aktuatora
    uart_puts("[ACT] seq=");
    uart_puthex_u32(cmd.seq);
    uart_puts(" m=(");
    uart_puthex_i32(cmd.mx);
    uart_puts(",");
    uart_puthex_i32(cmd.my);
    uart_puts(",");
    uart_puthex_i32(cmd.mz);
    uart_puts(") sat=");
    uart_puthex_u32(cmd.sat_flags);
    uart_puts(" sat_total=");
    uart_puthex_u32(sat_total);
    uart_puts("\r\n");

    if (cmd.seq >= MAX_SEQ)
    {
      uart_puts("[END] experiment finished\r\n");

      uart_puts("samples=");
      uart_puthex_u32(sample_count);
      uart_puts("\r\n");

      uart_puts("sat_total=");
      uart_puthex_u32(sat_total);
      uart_puts("\r\n");

      uart_puts("sat_percent=");
      uart_puthex_u32((sat_total * 100) / sample_count);
      uart_puts("\r\n");

      uart_puts("hardfault_count=");
      uart_puthex_u32(hardfault_count);
      uart_puts("\r\n");

      // wyłącz przerwania
      __asm volatile("cpsid i");
      while (1) {}
    }
  }
}

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
  uint32_t *start = &_sdata+0x00000600;
  uint32_t *end   = &_ebss;

  uint32_t range = (uint32_t)(end - start);
  if (range == 0) return;

  uint32_t idx = xorshift32() % range;
  uint32_t bit = xorshift32() % 32;

  uint32_t *target = start + idx;

  uart_puts("[SEU] addr=");
  uart_puthex_u32((uint32_t)target);
  uart_puts(" bit=");
  uart_puthex_u32(bit);
  uart_puts("\r\n");
  
  *target ^= (1 << bit);
}

static void task_seu(void *arg) {
  (void)arg;

  while (1) {
    seu_inject_random();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// main(): zainicjalizuj kolejki, utwórz taski i wystartuj scheduler
int main(void) {
  uart_puts("START\n");
  sensor_samples = xQueueCreate(8, sizeof(sensor_sample));
  coil_cmds = xQueueCreate(8, sizeof(coil_cmd));

  xTaskCreate(task_sensor, "sensor",  256, NULL, 2, NULL);
  xTaskCreate(task_controller, "controller", 256, NULL, 2, NULL);
  xTaskCreate(task_actuator, "actuator", 256, NULL, 2, NULL);
#if SEU_ENABLE
  xTaskCreate(task_seu, "seu", 256, NULL, 1, NULL);
#endif

  vTaskStartScheduler();
  while (1) {}
}
