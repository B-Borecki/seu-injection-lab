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
 *    1 = TMR (curr)
 *    2 = slew-rate limiting (cmd)
 *    3 = both (TMR + SRL)
 */
#ifndef PROTECT_MODE
#define PROTECT_MODE 0
#endif
#define PROTECT_TMR_EN   (PROTECT_MODE == 1 || PROTECT_MODE == 3)
#define PROTECT_SRL_EN   (PROTECT_MODE == 2 || PROTECT_MODE == 3)

#define SAMPLE_PERIOD_MS   5u
#define MAX_SEQ            20000u
// m = -K * dB, then saturation to +-CMD_M_MAX */
#define K        8
#define CMD_M_MAX          2000
// Slew-rate limiting: max allowed change per axis per sample
#define SRL_STEP_MAX       300
// Statistics window
#define STAT_WIN           1000u
#define SPIKE_THR          500

typedef struct {
  uint32_t seq;
  int32_t  bx;
  int32_t  by;
  int32_t  bz;
} mag_sample_t;

typedef struct {
  uint32_t seq;
  int32_t  mx;
  int32_t  my;
  int32_t  mz;
  uint32_t sat_flags; // bit0=X, bit1=Y, bit2=Z
} coil_cmd_t;

static QueueHandle_t q_mag_samples;
static QueueHandle_t q_cmds;

// Stats
static volatile uint32_t g_tmr_calls   = 0;
static volatile uint32_t g_srl_calls   = 0;
static volatile uint32_t g_srl_clamps  = 0;


__attribute__((noinline))
void seu_hook_prev(volatile mag_sample_t *prev, volatile mag_sample_t *curr_used) {
  (void)prev;
  (void)curr_used;
}

__attribute__((noinline))
void seu_hook_curr(volatile mag_sample_t *curr_used) {
  (void)curr_used;
}

__attribute__((noinline))
void seu_hook_curr_tmr(volatile mag_sample_t *r0, volatile mag_sample_t *r1, volatile mag_sample_t *r2) {
  (void)r0; (void)r1; (void)r2;
}

__attribute__((noinline))
void seu_hook_cmd(volatile coil_cmd_t *cmd) {
  (void)cmd;
}

__attribute__((noinline))
void end_hook(void) {}


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
  return (x < 0) ? (uint32_t)(~ux + 1u) : (uint32_t)ux;
}

static inline int32_t limit_step(int32_t x, int32_t prev, int32_t max_step, uint32_t *did_clamp) {
  int32_t d = x - prev;
  if (d >  max_step) { *did_clamp = 1u; return prev + max_step; }
  if (d < -max_step) { *did_clamp = 1u; return prev - max_step; }
  return x;
}

// TMR majority vote
static inline int32_t tmr_vote_i32(int32_t a, int32_t b, int32_t c) {
  uint32_t ua = (uint32_t)a, ub = (uint32_t)b, uc = (uint32_t)c;
  uint32_t uv = (ua & ub) | (ua & uc) | (ub & uc);
  return (int32_t)uv;
}

static void task_sensor(void *arg) {
  (void)arg;

  uint32_t seq = 0;
  // Base field (synthetic, constant)
  const int32_t BX0 = 20000;
  const int32_t BY0 = -5000;
  const int32_t BZ0 = 12000;

  for (;;) {
    // Small deterministic variation
    int32_t dx = (int32_t)(seq & 0xFFu) - 128;
    int32_t dy = (int32_t)((seq >> 1) & 0xFFu) - 128;
    int32_t dz = (int32_t)((seq >> 2) & 0xFFu) - 128;

    mag_sample_t s = {
      .seq = seq++,
      .bx  = BX0 + dx,
      .by  = BY0 + dy,
      .bz  = BZ0 + dz
    };

    (void)xQueueSend(q_mag_samples, &s, 0);

    uart_puts("[MAG  ] seq=");
    uart_puthex_u32(s.seq);
    uart_puts(" B=(");
    uart_puthex_i32(s.bx); uart_puts(",");
    uart_puthex_i32(s.by); uart_puts(",");
    uart_puthex_i32(s.bz);
    uart_puts(")\r\n");

    vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
  }
}

static void task_controller(void *arg) {
  (void)arg;

  mag_sample_t prev;
  mag_sample_t curr;
  // First sample for prev init
  if (xQueueReceive(q_mag_samples, &curr, portMAX_DELAY) == pdPASS) {
    prev = curr;
    uart_puts("[CTRL ] seq=");
    uart_puthex_u32(curr.seq);
    uart_puts(" init prev\r\n");
  }

  for (;;) {
    if (xQueueReceive(q_mag_samples, &curr, portMAX_DELAY) != pdPASS) {
      continue;
    }
    // used = sample that actually enters computation
    mag_sample_t used = curr;

#if PROTECT_TMR_EN
    // 3 replicas for TMR; GDB can flip one replica
    mag_sample_t r0 = curr, r1 = curr, r2 = curr;

    g_tmr_calls++;

    seu_hook_curr_tmr(&r0, &r1, &r2);

    used.bx = tmr_vote_i32(r0.bx, r1.bx, r2.bx);
    used.by = tmr_vote_i32(r0.by, r1.by, r2.by);
    used.bz = tmr_vote_i32(r0.bz, r1.bz, r2.bz);
#else
    // Run without protection. GDB flips directly the sample used for computation
    seu_hook_curr(&used);
#endif

    seu_hook_prev(&prev, &used);

    const int32_t dBx = used.bx - prev.bx;
    const int32_t dBy = used.by - prev.by;
    const int32_t dBz = used.bz - prev.bz;

    int32_t mx = -(int32_t)K * dBx;
    int32_t my = -(int32_t)K * dBy;
    int32_t mz = -(int32_t)K * dBz;

    uint32_t sat = 0;
    if (mx >  CMD_M_MAX) { mx =  CMD_M_MAX; sat |= 1u; }
    if (mx < -CMD_M_MAX) { mx = -CMD_M_MAX; sat |= 1u; }
    if (my >  CMD_M_MAX) { my =  CMD_M_MAX; sat |= 2u; }
    if (my < -CMD_M_MAX) { my = -CMD_M_MAX; sat |= 2u; }
    if (mz >  CMD_M_MAX) { mz =  CMD_M_MAX; sat |= 4u; }
    if (mz < -CMD_M_MAX) { mz = -CMD_M_MAX; sat |= 4u; }

    uart_puts("[CTRL ] seq=");
    uart_puthex_u32(curr.seq);
    uart_puts(" dB=(");
    uart_puthex_i32(dBx); uart_puts(",");
    uart_puthex_i32(dBy); uart_puts(",");
    uart_puthex_i32(dBz);
    uart_puts(") m=(");
    uart_puthex_i32(mx); uart_puts(",");
    uart_puthex_i32(my); uart_puts(",");
    uart_puthex_i32(mz);
    uart_puts(") sat=");
    uart_puthex_u32(sat);
    uart_puts("\r\n");

    coil_cmd_t cmd = {
      .seq = curr.seq,
      .mx = mx, .my = my, .mz = mz,
      .sat_flags = sat
    };

    (void)xQueueSend(q_cmds, &cmd, 0);

    prev = used;
  }
}


static void task_actuator(void *arg) {
  (void)arg;

  coil_cmd_t cmd;
  // stats
  uint32_t sat_total = 0;
  uint32_t sat_total_prev = 0;
  uint32_t win_samples = 0;
  uint64_t sum_amax = 0;
  // SRL state
  int32_t last_mx = 0, last_my = 0, last_mz = 0;
  uint8_t have_last = 0;

  int32_t prev_mx = 0, prev_my = 0, prev_mz = 0;
  uint8_t have_prev_m = 0;

  for (;;) {
    if (xQueueReceive(q_cmds, &cmd, portMAX_DELAY) != pdPASS) {
      continue;
    }

    if (cmd.seq >= MAX_SEQ) {
      end_hook();

      uart_puts("[COST ] protect_mode=");
      uart_puthex_u32((uint32_t)PROTECT_MODE);
      uart_puts(" tmr_calls=");
      uart_puthex_u32(g_tmr_calls);
      uart_puts(" srl_calls=");
      uart_puthex_u32(g_srl_calls);
      uart_puts(" srl_clamps=");
      uart_puthex_u32(g_srl_clamps);
      uart_puts("\r\n");

      uart_puts("[END]\r\n");
      __asm volatile("cpsid i");
      for (;;) {}
    }

    seu_hook_cmd(&cmd);

#if PROTECT_SRL_EN
    g_srl_calls++;
    // Slew-rate limiting
    if (!have_last) {
      last_mx = cmd.mx; last_my = cmd.my; last_mz = cmd.mz;
      have_last = 1;
    } else {
      uint32_t c = 0u;

      c = 0u; cmd.mx = limit_step(cmd.mx, last_mx, SRL_STEP_MAX, &c); g_srl_clamps += c;
      c = 0u; cmd.my = limit_step(cmd.my, last_my, SRL_STEP_MAX, &c); g_srl_clamps += c;
      c = 0u; cmd.mz = limit_step(cmd.mz, last_mz, SRL_STEP_MAX, &c); g_srl_clamps += c;

      last_mx = cmd.mx; last_my = cmd.my; last_mz = cmd.mz;
    }
#else
    have_last = 0;
#endif

    if (cmd.sat_flags) sat_total++;
    // A(seq)=max(|mx|,|my|,|mz|)
    uint32_t amx = u32_abs_i32(cmd.mx);
    uint32_t amy = u32_abs_i32(cmd.my);
    uint32_t amz = u32_abs_i32(cmd.mz);

    uint32_t amax = amx;
    if (amy > amax) amax = amy;
    if (amz > amax) amax = amz;

    sum_amax += amax;
    win_samples++;

    uint32_t dm = 0;
    if (!have_prev_m) {
      prev_mx = cmd.mx; prev_my = cmd.my; prev_mz = cmd.mz;
      have_prev_m = 1;
      dm = 0;
    } else {
      uint32_t dx = u32_abs_i32(cmd.mx - prev_mx);
      uint32_t dy = u32_abs_i32(cmd.my - prev_my);
      uint32_t dz = u32_abs_i32(cmd.mz - prev_mz);
      dm = dx; if (dy > dm) dm = dy; if (dz > dm) dm = dz;
      prev_mx = cmd.mx; prev_my = cmd.my; prev_mz = cmd.mz;
    }

    uart_puts("[ACT  ] seq=");
    uart_puthex_u32(cmd.seq);
    uart_puts(" m=(");
    uart_puthex_i32(cmd.mx); uart_puts(",");
    uart_puthex_i32(cmd.my); uart_puts(",");
    uart_puthex_i32(cmd.mz);
    uart_puts(") sat=");
    uart_puthex_u32(cmd.sat_flags);
    uart_puts(" sat_total=");
    uart_puthex_u32(sat_total);
    uart_puts("\r\n");
    // Window stats every STAT_WIN samples
    if ((cmd.seq % STAT_WIN) == 0u && cmd.seq != 0u) {
      uint32_t sat_win = sat_total - sat_total_prev;
      sat_total_prev = sat_total;

      uint32_t avg_amax = (win_samples > 0u) ? (uint32_t)(sum_amax / win_samples) : 0u;

      uart_puts("[STAT ] seq=");
      uart_puthex_u32(cmd.seq);
      uart_puts(" sat_win=");
      uart_puthex_u32(sat_win);
      uart_puts(" avgA=");
      uart_puthex_u32(avg_amax);
      uart_puts(" dm_spike=");
      uart_puthex_u32((dm > SPIKE_THR) ? 1u : 0u);
      uart_puts("\r\n");

      sum_amax = 0;
      win_samples = 0;
    }
  }
}

int main(void) {
  q_mag_samples = xQueueCreate(8, sizeof(mag_sample_t));
  q_cmds = xQueueCreate(8, sizeof(coil_cmd_t));
  xTaskCreate(task_sensor, "sensor",  256, NULL, 2, NULL);
  xTaskCreate(task_controller, "control", 256, NULL, 2, NULL);
  xTaskCreate(task_actuator, "act", 256, NULL, 2, NULL);
  vTaskStartScheduler();
  for (;;) {}
}
