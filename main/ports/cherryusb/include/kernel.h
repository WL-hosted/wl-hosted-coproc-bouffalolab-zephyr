/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WLH_CHERRYUSB_ZEPHYR_KERNEL_COMPAT_H
#define WLH_CHERRYUSB_ZEPHYR_KERNEL_COMPAT_H

/*
 * CherryUSB's Zephyr OSAL still includes the pre-Zephyr-3 compatibility name
 * <kernel.h>.  Keep the workaround local to this adapter so neither Zephyr nor
 * the pinned upstream submodule needs an unreviewed patch.
 *
 * TODO: Remove this forwarding header when CherryUSB includes
 * <zephyr/kernel.h>; the pinned revision and CI build are the removal gate.
 */
#include <zephyr/kernel.h>

/* z_current_get() became the public k_current_get() API in Zephyr 4.x. */
#define z_current_get k_current_get

#endif /* WLH_CHERRYUSB_ZEPHYR_KERNEL_COMPAT_H */
