#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#define UART0_BASE 0x4000C000u
#define UARTDR     (*(volatile uint32_t *)(UART0_BASE + 0x000))
#define UARTFR     (*(volatile uint32_t *)(UART0_BASE + 0x018))

#ifndef SEFI_MODE
#define SEFI_MODE 0
#endif
#define SEFI_TMR_EN   (SEFI_MODE == 1 || SEFI_MODE == 3)
#define SEFI_CMD_EN   (SEFI_MODE == 2 || SEFI_MODE == 3)

volatile uint32_t g_magic = 0x12345678;

typedef struct {
  uint32_t seq;
  int32_t Bx;
  int32_t By;
  int32_t Bz;
} mag_sample_t;

typedef struct {
  uint32_t seq;
  int32_t mx;
  int32_t my;
  int32_t mz;
  uint32_t sat_flags;
} coil_cmd_t;

static QueueHandle_t q_mag;
static QueueHandle_t q_cmd;

__attribute__((noinline)) void seu_hook_prev(volatile mag_sample_t *prev, volatile mag_sample_t *curr) {
  (void)prev;
  (void)curr;
}

__attribute__((noinline)) void seu_hook_curr_tmr(volatile mag_sample_t *c0, volatile mag_sample_t *c1, volatile mag_sample_t *c2) {
  (void)c0;
  (void)c1;
  (void)c2;
}

__attribute__((noinline)) void seu_hook_cmd(volatile coil_cmd_t *cmd) {
  (void)cmd;
}

__attribute__((noinline)) void end_hook(void) { }

static void uart_putc(char c) {
  while (UARTFR & (1u << 5)) {}
  UARTDR = (uint32_t)c;
}

static void uart_puts(const char *s) {
  while (*s) uart_putc(*s++);
}

static void uart_puthex(uint32_t v) {
  const char *hex = "0123456789ABCDEF";
  for (int i = 7; i >= 0; --i) {
    uart_putc(hex[(v >> (i * 4)) & 0xFu]);
  }
}

static void sensor_task(void *arg) {
  (void)arg;
  uint32_t seq = 0;

  const int32_t B0x = 20000;
  const int32_t B0y = -5000;
  const int32_t B0z = 12000;

  while (1) {
    int32_t dx = (int32_t)(seq & 0xFF) - 128;
    int32_t dy = (int32_t)((seq >> 1) & 0xFF) - 128;
    int32_t dz = (int32_t)((seq >> 2) & 0xFF) - 128;

    mag_sample_t s = {
      .seq = seq++,
      .Bx  = B0x + dx,
      .By  = B0y + dy,
      .Bz  = B0z + dz
    };

    if (xQueueSend(q_mag, &s, 0) != pdPASS) {
      uart_puts("[DROP ] seq=");
      uart_puthex(s.seq);
      uart_puts("\r\n");
    }

    uart_puts("[MAG  ] seq=");
    uart_puthex(s.seq);
    uart_puts(" Bx=");
    uart_puthex((uint32_t)s.Bx);
    uart_puts(" By=");
    uart_puthex((uint32_t)s.By);
    uart_puts(" Bz=");
    uart_puthex((uint32_t)s.Bz);
    uart_puts("\r\n");

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

static inline int32_t tmr_vote_i32(int32_t a, int32_t b, int32_t c) {
  uint32_t ua = (uint32_t)a, ub = (uint32_t)b, uc = (uint32_t)c;
  uint32_t uv = (ua & ub) | (ua & uc) | (ub & uc);
  return (int32_t)uv;
}

static void control_task(void *arg) {
  (void)arg;

  mag_sample_t curr;
  mag_sample_t prev;

  const int32_t K = 8;
  const int32_t M_MAX = 2000;

  if (xQueueReceive(q_mag, &curr, portMAX_DELAY) == pdPASS) {
    prev = curr;
    uart_puts("[CTRL ] seq=");
    uart_puthex(curr.seq);
    uart_puts(" init prev\r\n");
  }

  uint32_t last_seq = 0;

  while (1) {
    if (xQueueReceive(q_mag, &curr, portMAX_DELAY) == pdPASS) {
      uint32_t ds = curr.seq - last_seq;
      if (last_seq != 0 && ds != 1) {
        uart_puts("[GAP  ] last=");
        uart_puthex(last_seq);
        uart_puts(" curr=");
        uart_puthex(curr.seq);
        uart_puts("\r\n");
      }
      last_seq = curr.seq;

      mag_sample_t used = curr;

#if SEFI_TMR_EN
      mag_sample_t c0 = curr;
      mag_sample_t c1 = curr;
      mag_sample_t c2 = curr;

      seu_hook_curr_tmr(&c0, &c1, &c2);

      used.Bx = tmr_vote_i32(c0.Bx, c1.Bx, c2.Bx);
      used.By = tmr_vote_i32(c0.By, c1.By, c2.By);
      used.Bz = tmr_vote_i32(c0.Bz, c1.Bz, c2.Bz);
#endif

      seu_hook_prev(&prev, &used);

      int32_t dBx = used.Bx - prev.Bx;
      int32_t dBy = used.By - prev.By;
      int32_t dBz = used.Bz - prev.Bz;

      int32_t mx = -K * dBx;
      int32_t my = -K * dBy;
      int32_t mz = -K * dBz;

      uint32_t sat = 0;
      if (mx >  M_MAX) { mx =  M_MAX; sat |= 1u; }
      if (mx < -M_MAX) { mx = -M_MAX; sat |= 1u; }
      if (my >  M_MAX) { my =  M_MAX; sat |= 2u; }
      if (my < -M_MAX) { my = -M_MAX; sat |= 2u; }
      if (mz >  M_MAX) { mz =  M_MAX; sat |= 4u; }
      if (mz < -M_MAX) { mz = -M_MAX; sat |= 4u; }

      uart_puts("[CTRL ] seq=");
      uart_puthex(curr.seq);
      uart_puts(" dB=(");
      uart_puthex((uint32_t)dBx);
      uart_puts(",");
      uart_puthex((uint32_t)dBy);
      uart_puts(",");
      uart_puthex((uint32_t)dBz);
      uart_puts(")");
      uart_puts(" m=(");
      uart_puthex((uint32_t)mx);
      uart_puts(",");
      uart_puthex((uint32_t)my);
      uart_puts(",");
      uart_puthex((uint32_t)mz);
      uart_puts(") sat=");
      uart_puthex(sat);
      uart_puts("\r\n");

      coil_cmd_t cmd = {
        .seq = curr.seq,
        .mx = mx,
        .my = my,
        .mz = mz,
        .sat_flags = sat
      };
      xQueueSend(q_cmd, &cmd, 0);

      prev = used;
    }
  }
}

static uint32_t u32_abs_i32(int32_t x) { return (x < 0) ? (uint32_t)(-x) : (uint32_t)x; }

#define MAX_SEQ 20000u
#define CMD_STEP_MAX  300

static inline int32_t limit_step_i32(int32_t x, int32_t prev, int32_t max_step) {
  int32_t d = x - prev;
  if (d >  max_step) return prev + max_step;
  if (d < -max_step) return prev - max_step;
  return x;
}

static void actuator_task(void *arg) {
  (void)arg;

  coil_cmd_t cmd;
  uint32_t sat_cnt = 0;
  uint32_t sat_x = 0, sat_y = 0, sat_z = 0;
  #define STAT_WIN 1000u

  uint32_t sat_cnt_prev = 0;
  uint32_t win_samples = 0;
  uint64_t sum_abs_m = 0;

  static int32_t last_mx = 0, last_my = 0, last_mz = 0;
  static uint8_t have_last = 0;

  while (1) {
    if (xQueueReceive(q_cmd, &cmd, portMAX_DELAY) == pdPASS) {
      if (cmd.seq >= MAX_SEQ) {
        end_hook();
        uart_puts("[END]\r\n");
        __asm volatile("cpsid i");
        while (1) {}
      }
      seu_hook_cmd(&cmd);

#if SEFI_CMD_EN
      if (!have_last) {
        last_mx = cmd.mx; last_my = cmd.my; last_mz = cmd.mz;
        have_last = 1;
      } else {
        cmd.mx = limit_step_i32(cmd.mx, last_mx, CMD_STEP_MAX);
        cmd.my = limit_step_i32(cmd.my, last_my, CMD_STEP_MAX);
        cmd.mz = limit_step_i32(cmd.mz, last_mz, CMD_STEP_MAX);
      }
      last_mx = cmd.mx; last_my = cmd.my; last_mz = cmd.mz;
#else
      have_last = 0;
#endif

      if (cmd.sat_flags) sat_cnt++;
      if (cmd.sat_flags & 1u) sat_x++;
      if (cmd.sat_flags & 2u) sat_y++;
      if (cmd.sat_flags & 4u) sat_z++;

      uint32_t amx = u32_abs_i32(cmd.mx);
      uint32_t amy = u32_abs_i32(cmd.my);
      uint32_t amz = u32_abs_i32(cmd.mz);

      uint32_t amax = amx;
      if (amy > amax) amax = amy;
      if (amz > amax) amax = amz;

      sum_abs_m += amax;
      win_samples++;

      uart_puts("[ACT  ] seq=");
      uart_puthex(cmd.seq);
      uart_puts(" m=(");
      uart_puthex((uint32_t)cmd.mx);
      uart_puts(",");
      uart_puthex((uint32_t)cmd.my);
      uart_puts(",");
      uart_puthex((uint32_t)cmd.mz);
      uart_puts(") sat=");
      uart_puthex(cmd.sat_flags);
      uart_puts(" sat_cnt=");
      uart_puthex(sat_cnt);
      uart_puts("\r\n");
    }

    if ((cmd.seq % STAT_WIN) == 0u && cmd.seq != 0u) {
      uint32_t sat_win = sat_cnt - sat_cnt_prev;
      sat_cnt_prev = sat_cnt;

      uint32_t avg_abs_m = (win_samples > 0) ? (uint32_t)(sum_abs_m / win_samples) : 0;

      uart_puts("[STAT ] seq=");
      uart_puthex(cmd.seq);
      uart_puts(" sat_win=");
      uart_puthex(sat_win);
      uart_puts(" avg|m|=");
      uart_puthex(avg_abs_m);
      uart_puts("\r\n");

      sum_abs_m = 0;
      win_samples = 0;
    }
  }
}

int main(void) {
  uart_puts("\r\n[BOOT] FreeRTOS start\r\n");

  q_mag = xQueueCreate(8, sizeof(mag_sample_t));
  q_cmd = xQueueCreate(8, sizeof(coil_cmd_t));

  xTaskCreate(sensor_task, "sensor", 256, NULL, 2, NULL);
  xTaskCreate(control_task, "control", 256, NULL, 2, NULL);
  xTaskCreate(actuator_task, "act", 256, NULL, 2, NULL);

  uart_puts("[BOOT] starting scheduler\r\n");
  vTaskStartScheduler();

  uart_puts("[BOOT] scheduler failed\r\n");
  while (1) {}
}
