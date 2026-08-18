/* SPDX-License-Identifier: Apache-2.0 */
#include "bflb_usb_platform.h"

#include <errno.h>
#include <stdint.h>

#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>

LOG_MODULE_REGISTER(wlh_bflb_usb, LOG_LEVEL_INF);

#define BL616_GLB_BASE 0x20000000u
#define BL616_GLB_SWRST_CFG0_OFFSET 0x540u
#define BL616_GLB_CGEN_CFG2_OFFSET 0x588u
#define BL616_GLB_WIFI_PLL_CFG10_OFFSET 0x838u
#define BL616_GLB_GPIO_CFG0_OFFSET 0x8c4u
#define BL616_GLB_USB_BIT 19u
#define BL616_GLB_USB_MASK (1u << BL616_GLB_USB_BIT)
#define BL616_GLB_USBPLL_RSTB_MASK (1u << 28u)
#define BL616_GLB_PU_USBPLL_MMDIV_MASK (1u << 29u)
#define BL616_GPIO_USB_DP 27u
#define BL616_GPIO_USB_DM 28u
#define BL616_GPIO_ANALOG_FUNCTION 10u
#define BL616_GPIO_SMT_MASK (1u << 1u)
#define BL616_GPIO_FUNCTION_SHIFT 8u
#define BL616_GPIO_INTERRUPT_MASK (1u << 22u)
#define BL616_GPIO_SET_CLR_MODE_MASK (1u << 30u)

#define BL616_USB_BASE 0x20072000u
#define BL616_USB_HCMISC_OFFSET 0x40u
#define BL616_USB_HC_WAKEUP_DETECT_MASK (1u << 8u)
#define BL616_USB_HC_CONNECT_DETECT_MASK (1u << 9u)

static void configure_usb_pad(uint32_t pin) {
  const uintptr_t register_address =
      BL616_GLB_BASE + BL616_GLB_GPIO_CFG0_OFFSET + (pin << 2u);
  const uint32_t config =
      BL616_GPIO_INTERRUPT_MASK | BL616_GPIO_SMT_MASK |
      (BL616_GPIO_ANALOG_FUNCTION << BL616_GPIO_FUNCTION_SHIFT) |
      BL616_GPIO_SET_CLR_MODE_MASK;

  /* Equivalent to the Bouffalo SDK's GPIO_ANALOG | GPIO_SMT_EN |
   * GPIO_DRV_0 setup. USB owns these fixed-function pads after startup. */
  sys_write32(config, register_address);
}

int wlh_bflb_usb_platform_prepare(void) {
  const uintptr_t clock_register = BL616_GLB_BASE + BL616_GLB_CGEN_CFG2_OFFSET;
  const uintptr_t reset_register = BL616_GLB_BASE + BL616_GLB_SWRST_CFG0_OFFSET;
  const uintptr_t pll_register =
      BL616_GLB_BASE + BL616_GLB_WIFI_PLL_CFG10_OFFSET;
  uint32_t clock_after;
  uint32_t pll_after;
  uint32_t reset_value;
  unsigned int key;

  key = irq_lock();

  configure_usb_pad(BL616_GPIO_USB_DP);
  configure_usb_pad(BL616_GPIO_USB_DM);

  /* TODO: Remove this explicit gate setup once Zephyr's BL61x clock driver
   * enables GLB_CGEN_CFG2.EXT_USB (bit 19). It currently writes CFG1 bit 13,
   * leaving the controller unclocked and its SFRST bit permanently set. */
  sys_write32(sys_read32(clock_register) | BL616_GLB_USB_MASK, clock_register);
  clock_after = sys_read32(clock_register);

  /* Match the official BL616 SDK's GLB_Set_USB_CLK_From_WIFIPLL(1). The
   * Zephyr BL61x clock driver programs USBPLL_SDMIN but does not power and
   * reset the USB PLL divider which supplies the controller's UCLK. */
  sys_write32(sys_read32(pll_register) | BL616_GLB_PU_USBPLL_MMDIV_MASK,
              pll_register);
  k_busy_wait(3u);
  pll_after = sys_read32(pll_register) | BL616_GLB_USBPLL_RSTB_MASK;
  sys_write32(pll_after, pll_register);
  k_busy_wait(2u);
  sys_write32(pll_after & ~BL616_GLB_USBPLL_RSTB_MASK, pll_register);
  k_busy_wait(2u);
  sys_write32(pll_after, pll_register);
  pll_after = sys_read32(pll_register);

  /* Match GLB_AHB_MCU_Software_Reset(GLB_AHB_MCU_SW_EXT_USB): the reset
   * request is active high and must be returned to its inactive state. */
  reset_value = sys_read32(reset_register) & ~BL616_GLB_USB_MASK;
  sys_write32(reset_value, reset_register);
  (void)sys_read32(reset_register);
  sys_write32(reset_value | BL616_GLB_USB_MASK, reset_register);
  (void)sys_read32(reset_register);
  sys_write32(reset_value, reset_register);
  (void)sys_read32(reset_register);

  irq_unlock(key);

  if ((clock_after & BL616_GLB_USB_MASK) == 0u) {
    LOG_ERR("USB clock gate did not latch");
    return -EIO;
  }
  if ((pll_after &
       (BL616_GLB_PU_USBPLL_MMDIV_MASK | BL616_GLB_USBPLL_RSTB_MASK)) !=
      (BL616_GLB_PU_USBPLL_MMDIV_MASK | BL616_GLB_USBPLL_RSTB_MASK)) {
    LOG_ERR("USB PLL setup did not latch");
    return -EIO;
  }
  return 0;
}

void wlh_bflb_usb_platform_finish_init(void) {
  const uintptr_t register_address = BL616_USB_BASE + BL616_USB_HCMISC_OFFSET;

  /* Current CherryUSB's BL616 port predates these two writes in Bouffalo's
   * USB v2 driver. They enable the PHY connection and wakeup detectors. */
  sys_write32(sys_read32(register_address) | BL616_USB_HC_WAKEUP_DETECT_MASK |
                  BL616_USB_HC_CONNECT_DETECT_MASK,
              register_address);
}
