/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WLH_WIFI_BACKEND_H
#define WLH_WIFI_BACKEND_H

#include "wlh/coproc.h"

int wlh_wifi_backend_init(wlh_coproc_t *coproc);
void wlh_wifi_backend_transport_dead(void);
void wlh_wifi_backend_transport_alive(void);
wlh_coproc_wifi_ops_t wlh_wifi_backend_ops(void);
uint32_t wlh_wifi_backend_rx_capacity(void);
wlh_coproc_ethernet_rx_result_t
wlh_wifi_backend_ethernet_tx(void *context, uint32_t session_id,
                             uint8_t channel, const uint8_t *frame,
                             size_t size);

#endif
