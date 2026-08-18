/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WLH_CHERRYUSB_BFLB_CORE_H
#define WLH_CHERRYUSB_BFLB_CORE_H

#include <stdint.h>

#include <zephyr/irq.h>
#include <zephyr/kernel.h>

/* CherryUSB defines its own utility macro with this name. */
#undef DIV_ROUND_CLOSEST

typedef void (*bflb_irq_handler_t)(int irq, void *argument);

static bflb_irq_handler_t wlh_bflb_irq_handler;
static void *wlh_bflb_irq_argument;

static inline uint32_t getreg32(uintptr_t address) {
  return *(volatile uint32_t *)address;
}

static inline void putreg32(uint32_t value, uintptr_t address) {
  *(volatile uint32_t *)address = value;
}

static inline void bflb_mtimer_delay_us(uint32_t delay_us) {
  k_busy_wait(delay_us);
}

static inline void bflb_mtimer_delay_ms(uint32_t delay_ms) {
  k_msleep(delay_ms);
}

static inline void wlh_bflb_irq_dispatch(const void *unused) {
  (void)unused;
  if (wlh_bflb_irq_handler != NULL)
    wlh_bflb_irq_handler(37, wlh_bflb_irq_argument);
}

static inline int bflb_irq_attach(int irq, bflb_irq_handler_t handler,
                                  void *argument) {
  wlh_bflb_irq_handler = handler;
  wlh_bflb_irq_argument = argument;
  return irq_connect_dynamic((unsigned int)irq, 1u, wlh_bflb_irq_dispatch, NULL,
                             0u);
}

static inline void bflb_irq_enable(int irq) { irq_enable((unsigned int)irq); }

#endif
