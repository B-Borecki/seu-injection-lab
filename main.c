#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#define UART0_BASE 0x4000C000u
#define UARTDR     (*(volatile uint32_t *)(UART0_BASE + 0x000u))
#define UARTFR     (*(volatile uint32_t *)(UART0_BASE + 0x018u))

/*
 *  PROTECT_MODE:
 *    0 = no protection (baseline)
 *    1 = TMR (input_curr)
 *    2 = slew-rate limiting (output_cmd)
 *    3 = both (TMR + SRL)
 */
// Jeśli nie podano PROTECT_MODE w kompilacji, ustaw domyślnie baseline
#ifndef PROTECT_MODE
#define PROTECT_MODE 0
#endif
// Flagi kompilacyjne: czy włączyć TMR/SRL w zależności od trybu
#define PROTECT_TMR_EN   (PROTECT_MODE == 1 || PROTECT_MODE == 3)
#define PROTECT_SRL_EN   (PROTECT_MODE == 2 || PROTECT_MODE == 3)

// Okres próbkowania sensora (symulacja)
#define SAMPLE_PERIOD_MS   5u
// Limit próbek, po którym kończymy eksperyment i wypisujemy statystyki
#define MAX_SEQ            20000u
// m = -K * dB
#define K        8
// Saturacja komendy momentu magnetycznego (symulacja ograniczeń aktuatora)
#define CMD_M_MAX          2000
// SRL: maksymalny krok zmiany komendy na próbkę
#define SRL_STEP_MAX       300

// Próbka sensora (syntetyczne pole B)
typedef struct {
  uint32_t seq;
  int32_t  bx;
  int32_t  by;
  int32_t  bz;
} mag_sample_t;

// Komenda cewek (moment magnetyczny m) + flagi saturacji
typedef struct {
  uint32_t seq;
  int32_t  mx;
  int32_t  my;
  int32_t  mz;
  uint32_t sat_flags; // bity: 0=X, 1=Y, 2=Z (czy dana oś weszła w saturację)
} coil_output_cmd_t;

// Kolejka: sensor -> controller
static QueueHandle_t q_mag_samples;
// Kolejka: controller -> actuator
static QueueHandle_t q_output_cmds;

// Liczniki kosztu/aktywności mechanizmów ochrony
static volatile uint32_t g_tmr_calls   = 0;
static volatile uint32_t g_srl_calls   = 0;
static volatile uint32_t g_srl_clamps  = 0;

// Hooki do GDB: punkty wstrzykiwania SEU w dane
__attribute__((noinline))
void seu_hook_input_prev(volatile mag_sample_t *input_prev, volatile mag_sample_t *input_curr_used) {
  (void)input_prev;
  (void)input_curr_used;
}

__attribute__((noinline))
void seu_hook_input_curr(volatile mag_sample_t *input_curr_used) {
  (void)input_curr_used;
}

__attribute__((noinline))
void seu_hook_input_curr_tmr(volatile mag_sample_t *r0, volatile mag_sample_t *r1, volatile mag_sample_t *r2) {
  (void)r0; (void)r1; (void)r2;
}

__attribute__((noinline))
void seu_hook_output_cmd(volatile coil_output_cmd_t *output_cmd) {
  (void)output_cmd;
}

// Hook końca eksperymentu
__attribute__((noinline))
void end_hook(void) {}

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

// SRL: ogranicz zmianę x względem input_prev do +-max_step, ustaw did_clamp gdy ograniczono
static inline int32_t limit_step(int32_t x, int32_t prev, int32_t max_step, uint32_t *did_clamp) {
  int32_t d = x - prev;
  if (d >  max_step) { *did_clamp = 1u; return prev + max_step; }
  if (d < -max_step) { *did_clamp = 1u; return prev - max_step; }
  return x;
}

// TMR: głosowanie większościowe
static inline int32_t tmr_vote_i32(int32_t a, int32_t b, int32_t c) {
  uint32_t ua = (uint32_t)a, ub = (uint32_t)b, uc = (uint32_t)c;
  uint32_t uv = (ua & ub) | (ua & uc) | (ub & uc);
  return (int32_t)uv;
}

// Task sensor: generuje syntetyczne próbki pola B i wrzuca do kolejki
static void task_sensor(void *arg) {
  (void)arg;

  uint32_t seq = 0;
  // Bazowe pole (stałe), do którego dodajemy małą deterministyczną wariację
  const int32_t BX0 = 20000;
  const int32_t BY0 = -5000;
  const int32_t BZ0 = 12000;

  for (;;) {
    // Małe deterministyczne zaburzenia zależne od seq (żeby przebieg nie był idealnie stały)
    int32_t dx = (int32_t)(seq & 0xFFu) - 128;
    int32_t dy = (int32_t)((seq >> 1) & 0xFFu) - 128;
    int32_t dz = (int32_t)((seq >> 2) & 0xFFu) - 128;

    // Zbuduj próbkę magnetometru
    mag_sample_t s = {
      .seq = seq++,
      .bx  = BX0 + dx,
      .by  = BY0 + dy,
      .bz  = BZ0 + dz
    };

    // Wyślij próbkę do kontrolera
    (void)xQueueSend(q_mag_samples, &s, 0);

    vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
  }
}

// Task controller: bierze próbki B, liczy dB i wysyła komendy cewek do aktuatora
static void task_controller(void *arg) {
  (void)arg;

  mag_sample_t input_prev;
  mag_sample_t input_curr;

  // Pobierz pierwszą próbkę, żeby zainicjalizować input_prev
  if (xQueueReceive(q_mag_samples, &input_curr, portMAX_DELAY) == pdPASS) {
    input_prev = input_curr;
  }

  for (;;) {
    // Czekaj na kolejną próbkę sensora
    if (xQueueReceive(q_mag_samples, &input_curr, portMAX_DELAY) != pdPASS) {
      continue;
    }

    // input_curr_used to próbka, która faktycznie wchodzi do obliczeń (tu wstrzykujemy SEU)
    mag_sample_t input_curr_used = input_curr;

#if PROTECT_TMR_EN
    // TMR: utwórz 3 repliki bieżącej próbki (SEU może uszkodzić jedną)
    mag_sample_t r0 = input_curr, r1 = input_curr, r2 = input_curr;
    // Zlicz wywołania TMR (koszt mechanizmu)
    g_tmr_calls++;
    // Hook: wstrzyknięcie SEU w jedną z replik (kontrolowane z GDB)
    seu_hook_input_curr_tmr(&r0, &r1, &r2);
    // Złóż próbkę "used" przez głosowanie większościowe
    input_curr_used.bx = tmr_vote_i32(r0.bx, r1.bx, r2.bx);
    input_curr_used.by = tmr_vote_i32(r0.by, r1.by, r2.by);
    input_curr_used.bz = tmr_vote_i32(r0.bz, r1.bz, r2.bz);
#else
    // Bez ochrony: SEU bezpośrednio w próbce używanej do obliczeń
    seu_hook_input_curr(&input_curr_used);
#endif
    // Hook: możliwość wstrzyknięcia błędu w input_prev albo input_curr_used przed różniczkowaniem
    seu_hook_input_prev(&input_prev, &input_curr_used);
    // Różnica pola: dB = B(seq) - B(seq-1)
    const int32_t dBx = input_curr_used.bx - input_prev.bx;
    const int32_t dBy = input_curr_used.by - input_prev.by;
    const int32_t dBz = input_curr_used.bz - input_prev.bz;
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
    coil_output_cmd_t output_cmd = {
      .seq = input_curr.seq,
      .mx = mx, .my = my, .mz = mz,
      .sat_flags = sat
    };
    // Wyślij komendę do aktuatora
    (void)xQueueSend(q_output_cmds, &output_cmd, 0);

    input_prev = input_curr_used;
  }
}

// Task actuator: odbiera komendy, opcjonalnie SRL, liczy statystyki i kończy eksperyment
static void task_actuator(void *arg) {
  (void)arg;

  coil_output_cmd_t output_cmd;

  // Ile razy wystąpiła saturacja
  uint32_t sat_total = 0;

  // Stan SRL: poprzednie wartości m (do limitowania kroku)
  int32_t last_mx = 0, last_my = 0, last_mz = 0;
  uint8_t have_last = 0;

  for (;;) {
    // Czekaj na komendę z kontrolera
    if (xQueueReceive(q_output_cmds, &output_cmd, portMAX_DELAY) != pdPASS) {
      continue;
    }

    // Warunek końca eksperymentu
    if (output_cmd.seq >= MAX_SEQ) {
      end_hook();

      // Wypisz liczniki kosztu ochrony
      uart_puts("[COST ] protect_mode=");
      uart_puthex_u32((uint32_t)PROTECT_MODE);
      uart_puts(" tmr_calls=");
      uart_puthex_u32(g_tmr_calls);
      uart_puts(" srl_calls=");
      uart_puthex_u32(g_srl_calls);
      uart_puts(" srl_clamps=");
      uart_puthex_u32(g_srl_clamps);
      uart_puts("\r\n");
      // Sygnalizuj koniec i zatrzymaj system
      uart_puts("[END]\r\n");
      __asm volatile("cpsid i");
      for (;;) {}
    }

    // Hook: wstrzyknięcie SEU w komendę aktuatora
    seu_hook_output_cmd(&output_cmd);

#if PROTECT_SRL_EN
    // Zlicz wywołania SRL (koszt mechanizmu)
    g_srl_calls++;

    // SRL: ogranicz narastanie/zmianę komendy między próbkami
    if (!have_last) {
      // Pierwsza próbka: ustaw stan bez limitowania
      last_mx = output_cmd.mx; last_my = output_cmd.my; last_mz = output_cmd.mz;
      have_last = 1;
    } else {
      uint32_t c = 0u;
      // Limituj każdą oś osobno i zlicz ile razy przycięto
      c = 0u;
      output_cmd.mx = limit_step(output_cmd.mx, last_mx, SRL_STEP_MAX, &c);
      g_srl_clamps += c;
      c = 0u;
      output_cmd.my = limit_step(output_cmd.my, last_my, SRL_STEP_MAX, &c);
      g_srl_clamps += c;
      c = 0u;
      output_cmd.mz = limit_step(output_cmd.mz, last_mz, SRL_STEP_MAX, &c);
      g_srl_clamps += c;
      // Zaktualizuj stan SRL
      last_mx = output_cmd.mx; last_my = output_cmd.my; last_mz = output_cmd.mz;
    }
#else
    // Bez SRL: wyczyść stan, żeby po przełączeniu trybu nie było starej historii
    have_last = 0;
#endif
    // Zlicz próbki z saturacją
    if (output_cmd.sat_flags) sat_total++;

    // A(seq)=max(|mx|,|my|,|mz|)
    uint32_t amx = u32_abs_i32(output_cmd.mx);
    uint32_t amy = u32_abs_i32(output_cmd.my);
    uint32_t amz = u32_abs_i32(output_cmd.mz);
    uint32_t amax = amx;
    if (amy > amax) amax = amy;
    if (amz > amax) amax = amz;

    // Log aktuatora
    uart_puts("[ACT  ] seq=");
    uart_puthex_u32(output_cmd.seq);
    uart_puts(" m=(");
    uart_puthex_i32(output_cmd.mx); uart_puts(",");
    uart_puthex_i32(output_cmd.my); uart_puts(",");
    uart_puthex_i32(output_cmd.mz);
    uart_puts(") sat=");
    uart_puthex_u32(output_cmd.sat_flags);
    uart_puts(" sat_total=");
    uart_puthex_u32(sat_total);
    uart_puts("\r\n");
  }
}

// main(): zainicjalizuj kolejki, utwórz taski i wystartuj scheduler
int main(void) {
  // Buforowane kolejki komunikacyjne między taskami
  q_mag_samples = xQueueCreate(8, sizeof(mag_sample_t));
  q_output_cmds = xQueueCreate(8, sizeof(coil_output_cmd_t));
  // 3 taski: sensor -> controller -> actuator
  xTaskCreate(task_sensor, "sensor",  256, NULL, 2, NULL);
  xTaskCreate(task_controller, "control", 256, NULL, 2, NULL);
  xTaskCreate(task_actuator, "act", 256, NULL, 2, NULL);
  // Start schedulera FreeRTOS (od tego momentu taski przejmują sterowanie)
  vTaskStartScheduler();

  for (;;) {}
}
