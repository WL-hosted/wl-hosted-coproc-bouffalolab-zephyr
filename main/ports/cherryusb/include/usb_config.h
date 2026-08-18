/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CHERRYUSB_CONFIG_H
#define CHERRYUSB_CONFIG_H

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define CONFIG_USB_PRINTF(...) printk(__VA_ARGS__)
#define CONFIG_USB_DBG_LEVEL USB_DBG_INFO
#define CONFIG_USB_ALIGN_SIZE 4
#define USB_NOCACHE_RAM_SECTION __attribute__((section("USB_NOCACHE")))
#define CONFIG_USBDEV_REQUEST_BUFFER_LEN 512
#define CONFIG_USBDEV_MAX_BUS 1
#define CONFIG_USBDEV_EP_NUM 5
#define usb_phyaddr2ramaddr(addr) (addr)
#define usb_ramaddr2phyaddr(addr) (addr)

/* CherryUSB's Bouffalo port uses this legacy macro internally. */
#if defined(CONFIG_CHERRYUSB_DEVICE_SPEED_HS)
#define CONFIG_USB_HS 1
#endif

/* CherryUSB defines its own utility macro with this name. */
#undef DIV_ROUND_CLOSEST

#endif
