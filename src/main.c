/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "firmware_config.h"
#include "transport.h"
#include "wifi_backend.h"
#include "wlh/coproc.h"
#include "wlh/zephyr_osal.h"

LOG_MODULE_REGISTER(wlh_coproc_app, LOG_LEVEL_INF);

static wlh_coproc_t coproc;
static wlh_zephyr_osal_t zephyr_osal;
static struct k_sem reset_requested;
static struct k_thread link_control_thread;
K_THREAD_STACK_DEFINE(link_control_stack, 2048);

static uint8_t *buffer_alloc(void *context, size_t size) {
  (void)context;
  return k_malloc(size);
}

static void buffer_free(void *context, uint8_t *buffer) {
  (void)context;
  k_free(buffer);
}

static int get_device_info(void *context, wlh_coproc_device_info_t *info) {
  (void)context;
  memset(info, 0, sizeof(*info));
  strncpy(info->vendor, "Bouffalo Lab", sizeof(info->vendor) - 1u);
  strncpy(info->mcu_model, WLH_MCU_NAME, sizeof(info->mcu_model) - 1u);
  strncpy(info->board_profile, WLH_BOARD_PROFILE,
          sizeof(info->board_profile) - 1u);
  return 0;
}

static void on_transport_reset(void *context) {
  (void)context;
  wlh_wifi_backend_transport_dead();
  k_sem_give(&reset_requested);
}

static void link_control_entry(void *first, void *second, void *third) {
  (void)first;
  (void)second;
  (void)third;
  for (;;) {
    k_sem_take(&reset_requested, K_FOREVER);
    k_msleep(100);
    LOG_WRN("USB reset: restarting Core session");
    if (wlh_coproc_stop(&coproc) != WLH_COPROC_OK)
      LOG_WRN("Core stop failed during reset");
    if (wlh_coproc_start(&coproc) != WLH_COPROC_OK) {
      LOG_ERR("Core restart failed");
    } else {
      wlh_wifi_backend_transport_alive();
    }
  }
}

int main(void) {
  wlh_coproc_config_t config;
  wlh_transport_config_t transport_config;

  LOG_INF("wl-hosted %s coprocessor (%s)", WLH_MCU_NAME, WLH_BOARD_PROFILE);
  k_sem_init(&reset_requested, 0u, 1u);
  wlh_zephyr_osal_init(&zephyr_osal);

  memset(&config, 0, sizeof(config));
  config.port.submit_tx = wlh_transport_submit_tx;
  config.port.ethernet_sta_rx = wlh_wifi_backend_ethernet_tx;
  config.port.ethernet_ap_rx = wlh_wifi_backend_ethernet_tx;
  config.buffers = (wlh_coproc_buffer_ops_t){NULL, buffer_alloc, buffer_free};
  config.osal = wlh_zephyr_osal_ops(&zephyr_osal);
  config.wifi = wlh_wifi_backend_ops();
  config.device_info = (wlh_coproc_device_info_ops_t){NULL, get_device_info};
  strncpy(config.implementation_version, WLH_IMPLEMENTATION_VERSION,
          sizeof(config.implementation_version) - 1u);
  config.max_frame_size = wlh_transport_max_frame_size();
  config.heartbeat_interval_ms = 1000u;
  config.initial_credit = wlh_wifi_backend_rx_capacity();
  config.core_queue_depth = WLH_CORE_QUEUE_DEPTH;
  config.ethernet_tx_depth = (uint8_t)wlh_transport_tx_capacity();
  config.ethernet_tx_aggregation_limit = 4u;
  config.stop_timeout_ms = WLH_STOP_TIMEOUT_MS;
  config.core_task = (wlh_osal_task_attributes_t){
      "wlh-core", WLH_CORE_TASK_STACK, WLH_CORE_TASK_OSAL_PRIORITY};

  if (wlh_coproc_init(&coproc, &config) != WLH_COPROC_OK) {
    LOG_ERR("Core initialization failed");
    return -1;
  }
  if (wlh_wifi_backend_init(&coproc) != 0) {
    LOG_ERR("Wi-Fi backend initialization failed");
    return -1;
  }
  if (wlh_coproc_start(&coproc) != WLH_COPROC_OK) {
    LOG_ERR("Core start failed");
    return -1;
  }

  transport_config = (wlh_transport_config_t){&coproc, config.max_frame_size,
                                              on_transport_reset, NULL};
  if (wlh_transport_start(&transport_config) != 0) {
    LOG_ERR("CherryUSB transport start failed");
    return -1;
  }

  k_thread_create(&link_control_thread, link_control_stack,
                  K_THREAD_STACK_SIZEOF(link_control_stack), link_control_entry,
                  NULL, NULL, NULL, K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
  k_thread_name_set(&link_control_thread, "wlh-link-control");
  LOG_INF("coprocessor ready, waiting for USB host");
  return 0;
}
