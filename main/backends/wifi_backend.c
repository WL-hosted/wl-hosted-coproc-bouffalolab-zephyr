/* SPDX-License-Identifier: Apache-2.0 */
#include "wifi_backend.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/atomic.h>

#include "firmware_config.h"

LOG_MODULE_REGISTER(wlh_wifi, LOG_LEVEL_INF);

#define WLH_WIFI_INTERFACE_STA BIT(0)
#define WLH_WIFI_INTERFACE_AP BIT(1)

#define WLH_WIFI_SECURITY_OPEN 1u
#define WLH_WIFI_SECURITY_WEP 2u
#define WLH_WIFI_SECURITY_WPA_PSK 3u
#define WLH_WIFI_SECURITY_WPA2_PSK 4u
#define WLH_WIFI_SECURITY_WPA_WPA2_PSK 5u
#define WLH_WIFI_SECURITY_WPA3_SAE 6u
#define WLH_WIFI_SECURITY_WPA2_WPA3_PSK 7u

#define WLH_DISCONNECT_REASON_LOCAL_REQUEST 1u
#define WLH_DISCONNECT_REASON_AUTH_FAILED 3u
#define WLH_DISCONNECT_REASON_LINK_LOST 8u

typedef enum wifi_mode {
  WLH_WIFI_MODE_NONE = 0,
  WLH_WIFI_MODE_STA,
  WLH_WIFI_MODE_AP,
} wifi_mode_t;

typedef struct wifi_tx_frame {
  uint32_t session_id;
  uint8_t channel;
  size_t size;
  uint8_t data[WLH_WIFI_MAX_ETHERNET_FRAME_SIZE];
} wifi_tx_frame_t;

typedef struct wifi_backend {
  wlh_coproc_t *coproc;
  struct net_if *iface;
  struct net_mgmt_event_callback event_callback;
  struct k_msgq tx_queue;
  struct k_mutex ingress_lock;
  struct k_thread tx_thread;
  struct k_thread rx_thread;
  int packet_socket;
  uint32_t scan_id;
  uint32_t scan_results;
  atomic_t requested_mode;
  atomic_t active_mode;
  atomic_t connected;
  atomic_t disconnect_locally;
  atomic_t transport_alive;
} wifi_backend_t;

static wifi_backend_t backend;
static uint8_t
    tx_queue_storage[WLH_WIFI_TX_QUEUE_DEPTH * sizeof(wifi_tx_frame_t)];
K_THREAD_STACK_DEFINE(wifi_tx_stack, 3072);
K_THREAD_STACK_DEFINE(wifi_rx_stack, 4096);

static uint32_t map_security_to_wire(enum wifi_security_type security) {
  switch (security) {
  case WIFI_SECURITY_TYPE_NONE:
    return WLH_WIFI_SECURITY_OPEN;
  case WIFI_SECURITY_TYPE_WEP:
  case WIFI_SECURITY_TYPE_WEP_OPEN:
  case WIFI_SECURITY_TYPE_WEP_SHARED:
    return WLH_WIFI_SECURITY_WEP;
  case WIFI_SECURITY_TYPE_WPA_PSK:
    return WLH_WIFI_SECURITY_WPA_PSK;
  case WIFI_SECURITY_TYPE_SAE:
  case WIFI_SECURITY_TYPE_SAE_H2E:
  case WIFI_SECURITY_TYPE_SAE_AUTO:
    return WLH_WIFI_SECURITY_WPA3_SAE;
  case WIFI_SECURITY_TYPE_WPA_AUTO_PERSONAL:
    return WLH_WIFI_SECURITY_WPA_WPA2_PSK;
  default:
    return WLH_WIFI_SECURITY_WPA2_PSK;
  }
}

static int map_security_from_wire(uint32_t security,
                                  enum wifi_security_type *zephyr_security,
                                  enum wifi_mfp_options *mfp) {
  *mfp = WIFI_MFP_DISABLE;
  switch (security) {
  case WLH_WIFI_SECURITY_OPEN:
    *zephyr_security = WIFI_SECURITY_TYPE_NONE;
    return 0;
  case WLH_WIFI_SECURITY_WEP:
    *zephyr_security = WIFI_SECURITY_TYPE_WEP;
    return 0;
  case WLH_WIFI_SECURITY_WPA_PSK:
    *zephyr_security = WIFI_SECURITY_TYPE_WPA_PSK;
    return 0;
  case WLH_WIFI_SECURITY_WPA2_PSK:
    *zephyr_security = WIFI_SECURITY_TYPE_PSK;
    return 0;
  case WLH_WIFI_SECURITY_WPA_WPA2_PSK:
  case WLH_WIFI_SECURITY_WPA2_WPA3_PSK:
    *zephyr_security = WIFI_SECURITY_TYPE_WPA_AUTO_PERSONAL;
    *mfp = WIFI_MFP_OPTIONAL;
    return 0;
  case WLH_WIFI_SECURITY_WPA3_SAE:
    *zephyr_security = WIFI_SECURITY_TYPE_SAE;
    *mfp = WIFI_MFP_REQUIRED;
    return 0;
  default:
    return -EINVAL;
  }
}

static void fill_interface_mac(uint8_t mac[6]) {
  const struct net_linkaddr *address = net_if_get_link_addr(backend.iface);
  if (address != NULL && address->len >= 6u)
    memcpy(mac, address->addr, 6u);
}

static int get_iface_status(struct wifi_iface_status *status) {
  memset(status, 0, sizeof(*status));
  return net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, backend.iface, status,
                  sizeof(*status));
}

static void report_connected(void) {
  struct wifi_iface_status status;
  wlh_coproc_bss_t bss;
  if (get_iface_status(&status) != 0)
    return;
  memset(&bss, 0, sizeof(bss));
  bss.ssid = (const uint8_t *)status.ssid;
  bss.ssid_size = MIN(status.ssid_len, WIFI_SSID_MAX_LEN);
  memcpy(bss.bssid, status.bssid, sizeof(bss.bssid));
  fill_interface_mac(bss.interface_mac);
  bss.security = map_security_to_wire(status.security);
  bss.channel = status.channel;
  bss.rssi_dbm = status.rssi;
  atomic_set(&backend.connected, 1);
  atomic_set(&backend.active_mode, WLH_WIFI_MODE_STA);
  (void)wlh_coproc_wifi_connected(backend.coproc, &bss);
}

static void report_ap_started(void) {
  struct wifi_iface_status status;
  wlh_coproc_bss_t bss;
  if (get_iface_status(&status) != 0)
    return;
  memset(&bss, 0, sizeof(bss));
  bss.ssid = (const uint8_t *)status.ssid;
  bss.ssid_size = MIN(status.ssid_len, WIFI_SSID_MAX_LEN);
  memcpy(bss.bssid, status.bssid, sizeof(bss.bssid));
  fill_interface_mac(bss.interface_mac);
  bss.security = map_security_to_wire(status.security);
  bss.channel = status.channel;
  atomic_set(&backend.active_mode, WLH_WIFI_MODE_AP);
  (void)wlh_coproc_wifi_ap_started(backend.coproc, &bss);
}

static void wifi_event_handler(struct net_mgmt_event_callback *callback,
                               uint64_t event, struct net_if *iface) {
  (void)iface;
  k_mutex_lock(&backend.ingress_lock, K_FOREVER);
  if (atomic_get(&backend.transport_alive) == 0) {
    k_mutex_unlock(&backend.ingress_lock);
    return;
  }
  switch (event) {
  case NET_EVENT_WIFI_SCAN_RESULT: {
    const struct wifi_scan_result *result =
        (const struct wifi_scan_result *)callback->info;
    wlh_coproc_bss_t bss;
    if (result == NULL || backend.scan_results >= WLH_WIFI_SCAN_MAX_RESULTS)
      break;
    memset(&bss, 0, sizeof(bss));
    bss.ssid = result->ssid;
    bss.ssid_size = result->ssid_length;
    memcpy(bss.bssid, result->mac, sizeof(bss.bssid));
    bss.security = map_security_to_wire(result->security);
    bss.channel = result->channel;
    bss.rssi_dbm = result->rssi;
    (void)wlh_coproc_wifi_scan_result(backend.coproc, backend.scan_id, &bss);
    backend.scan_results++;
    break;
  }
  case NET_EVENT_WIFI_SCAN_DONE:
    (void)wlh_coproc_wifi_scan_completed(backend.coproc, backend.scan_id,
                                         backend.scan_results, false);
    break;
  case NET_EVENT_WIFI_CONNECT_RESULT: {
    const struct wifi_status *status =
        (const struct wifi_status *)callback->info;
    if (status != NULL && status->status == 0)
      report_connected();
    else
      (void)wlh_coproc_wifi_disconnected(
          backend.coproc, WLH_DISCONNECT_REASON_AUTH_FAILED, false);
    break;
  }
  case NET_EVENT_WIFI_DISCONNECT_RESULT:
  case NET_EVENT_WIFI_DISCONNECT_COMPLETE: {
    bool local = atomic_cas(&backend.disconnect_locally, 1, 0);
    atomic_clear(&backend.connected);
    atomic_set(&backend.active_mode, WLH_WIFI_MODE_NONE);
    (void)wlh_coproc_wifi_disconnected(backend.coproc,
                                       local
                                           ? WLH_DISCONNECT_REASON_LOCAL_REQUEST
                                           : WLH_DISCONNECT_REASON_LINK_LOST,
                                       local);
    break;
  }
  case NET_EVENT_WIFI_AP_ENABLE_RESULT: {
    const struct wifi_status *status =
        (const struct wifi_status *)callback->info;
    if (status != NULL && status->status == 0)
      report_ap_started();
    break;
  }
  case NET_EVENT_WIFI_AP_DISABLE_RESULT:
    atomic_set(&backend.active_mode, WLH_WIFI_MODE_NONE);
    (void)wlh_coproc_wifi_ap_stopped(backend.coproc,
                                     WLH_DISCONNECT_REASON_LOCAL_REQUEST, true);
    break;
  case NET_EVENT_WIFI_AP_STA_CONNECTED: {
    const struct wifi_ap_sta_info *station =
        (const struct wifi_ap_sta_info *)callback->info;
    if (station != NULL)
      (void)wlh_coproc_wifi_ap_client_joined(backend.coproc, station->mac, 0,
                                             0u);
    break;
  }
  case NET_EVENT_WIFI_AP_STA_DISCONNECTED: {
    const struct wifi_ap_sta_info *station =
        (const struct wifi_ap_sta_info *)callback->info;
    if (station != NULL)
      (void)wlh_coproc_wifi_ap_client_left(backend.coproc, station->mac, 0u,
                                           0u);
    break;
  }
  default:
    break;
  }
  k_mutex_unlock(&backend.ingress_lock);
}

static int wifi_initialize(void *context, uint32_t operation_id,
                           uint32_t interface_flags) {
  wifi_mode_t requested;
  (void)context;
  if (interface_flags == WLH_WIFI_INTERFACE_STA)
    requested = WLH_WIFI_MODE_STA;
  else if (interface_flags == WLH_WIFI_INTERFACE_AP)
    requested = WLH_WIFI_MODE_AP;
  else
    return -EINVAL;
  atomic_set(&backend.requested_mode, requested);
  return wlh_coproc_wifi_initialized(backend.coproc, operation_id, 0) ==
                 WLH_COPROC_OK
             ? 0
             : -EIO;
}

static int wifi_scan(void *context, uint32_t scan_id) {
  struct wifi_scan_params params = {0};
  (void)context;
  if (atomic_get(&backend.requested_mode) != WLH_WIFI_MODE_STA)
    return -EPERM;
  backend.scan_id = scan_id;
  backend.scan_results = 0u;
  params.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  params.bands = BIT(WIFI_FREQ_BAND_2_4_GHZ);
  params.max_bss_cnt = WLH_WIFI_SCAN_MAX_RESULTS;
  return net_mgmt(NET_REQUEST_WIFI_SCAN, backend.iface, &params,
                  sizeof(params));
}

static int wifi_connect(void *context,
                        const wlh_coproc_wifi_connect_t *request) {
  struct wifi_connect_req_params params;
  (void)context;
  if (request == NULL || request->ssid_size == 0u ||
      request->ssid_size > sizeof(request->ssid) ||
      request->credential_size > sizeof(request->credential) ||
      atomic_get(&backend.requested_mode) != WLH_WIFI_MODE_STA ||
      atomic_get(&backend.active_mode) == WLH_WIFI_MODE_AP)
    return -EINVAL;
  memset(&params, 0, sizeof(params));
  params.ssid = request->ssid;
  params.ssid_length = (uint8_t)request->ssid_size;
  params.psk = request->credential;
  params.psk_length = (uint8_t)request->credential_size;
  params.band = WIFI_FREQ_BAND_2_4_GHZ;
  params.channel = WIFI_CHANNEL_ANY;
  params.timeout = SYS_FOREVER_MS;
  if (map_security_from_wire(request->security, &params.security,
                             &params.mfp) != 0)
    return -EINVAL;
  return net_mgmt(NET_REQUEST_WIFI_CONNECT, backend.iface, &params,
                  sizeof(params));
}

static int wifi_disconnect(void *context) {
  (void)context;
  if (atomic_get(&backend.active_mode) != WLH_WIFI_MODE_STA)
    return -EALREADY;
  atomic_set(&backend.disconnect_locally, 1);
  return net_mgmt(NET_REQUEST_WIFI_DISCONNECT, backend.iface, NULL, 0u);
}

static int wifi_start_ap(void *context, const wlh_coproc_wifi_ap_t *request) {
  struct wifi_connect_req_params params;
  (void)context;
  if (request == NULL || request->ssid_size == 0u ||
      request->ssid_size > sizeof(request->ssid) ||
      request->credential_size > sizeof(request->credential) ||
      atomic_get(&backend.requested_mode) != WLH_WIFI_MODE_AP ||
      atomic_get(&backend.active_mode) == WLH_WIFI_MODE_STA)
    return -EINVAL;
  memset(&params, 0, sizeof(params));
  params.ssid = request->ssid;
  params.ssid_length = (uint8_t)request->ssid_size;
  params.psk = request->credential;
  params.psk_length = (uint8_t)request->credential_size;
  params.band = WIFI_FREQ_BAND_2_4_GHZ;
  params.channel = request->channel == 0u ? 1u : (uint8_t)request->channel;
  if (map_security_from_wire(request->security, &params.security,
                             &params.mfp) != 0)
    return -EINVAL;
  return net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, backend.iface, &params,
                  sizeof(params));
}

static int wifi_stop_ap(void *context) {
  (void)context;
  if (atomic_get(&backend.active_mode) != WLH_WIFI_MODE_AP)
    return -EALREADY;
  return net_mgmt(NET_REQUEST_WIFI_AP_DISABLE, backend.iface, NULL, 0u);
}

static void tx_thread_entry(void *first, void *second, void *third) {
  wifi_tx_frame_t frame;
  struct net_sockaddr_ll destination = {0};
  (void)first;
  (void)second;
  (void)third;
  destination.sll_family = NET_AF_PACKET;
  destination.sll_ifindex = net_if_get_by_iface(backend.iface);
  destination.sll_protocol = net_htons(ETH_P_ALL);
  for (;;) {
    int status;
    k_msgq_get(&backend.tx_queue, &frame, K_FOREVER);
    status = zsock_sendto(backend.packet_socket, frame.data, frame.size, 0,
                          (const struct net_sockaddr *)&destination,
                          sizeof(destination));
    k_mutex_lock(&backend.ingress_lock, K_FOREVER);
    if (atomic_get(&backend.transport_alive) != 0) {
      (void)wlh_coproc_ethernet_rx_complete(
          backend.coproc, frame.session_id, frame.channel, 1u,
          status == (int)frame.size ? 0 : -EIO);
    }
    k_mutex_unlock(&backend.ingress_lock);
  }
}

static void rx_thread_entry(void *first, void *second, void *third) {
  uint8_t frame[WLH_WIFI_MAX_ETHERNET_FRAME_SIZE];
  (void)first;
  (void)second;
  (void)third;
  for (;;) {
    int size = zsock_recv(backend.packet_socket, frame, sizeof(frame), 0);
    if (size <= 0)
      continue;
    k_mutex_lock(&backend.ingress_lock, K_FOREVER);
    if (atomic_get(&backend.transport_alive) != 0 &&
        atomic_get(&backend.active_mode) == WLH_WIFI_MODE_STA &&
        atomic_get(&backend.connected) != 0) {
      (void)wlh_coproc_ethernet_sta_send(backend.coproc, frame, (size_t)size);
    } else if (atomic_get(&backend.transport_alive) != 0 &&
               atomic_get(&backend.active_mode) == WLH_WIFI_MODE_AP) {
      (void)wlh_coproc_ethernet_ap_send(backend.coproc, frame, (size_t)size);
    }
    k_mutex_unlock(&backend.ingress_lock);
  }
}

wlh_coproc_ethernet_rx_result_t
wlh_wifi_backend_ethernet_tx(void *context, uint32_t session_id,
                             uint8_t channel, const uint8_t *frame,
                             size_t size) {
  wifi_tx_frame_t queued;
  (void)context;
  if (frame == NULL || size == 0u || size > sizeof(queued.data) ||
      atomic_get(&backend.active_mode) == WLH_WIFI_MODE_NONE ||
      atomic_get(&backend.transport_alive) == 0)
    return WLH_COPROC_ETHERNET_RX_REJECTED;
  queued.session_id = session_id;
  queued.channel = channel;
  queued.size = size;
  memcpy(queued.data, frame, size);
  return k_msgq_put(&backend.tx_queue, &queued, K_NO_WAIT) == 0
             ? WLH_COPROC_ETHERNET_RX_PENDING
             : WLH_COPROC_ETHERNET_RX_REJECTED;
}

int wlh_wifi_backend_init(wlh_coproc_t *coproc) {
  struct net_sockaddr_ll address = {0};
  uint64_t events =
      NET_EVENT_WIFI_SCAN_RESULT | NET_EVENT_WIFI_SCAN_DONE |
      NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT |
      NET_EVENT_WIFI_DISCONNECT_COMPLETE | NET_EVENT_WIFI_AP_ENABLE_RESULT |
      NET_EVENT_WIFI_AP_DISABLE_RESULT | NET_EVENT_WIFI_AP_STA_CONNECTED |
      NET_EVENT_WIFI_AP_STA_DISCONNECTED;
  if (coproc == NULL)
    return -EINVAL;
  memset(&backend, 0, sizeof(backend));
  backend.coproc = coproc;
  backend.iface = net_if_get_default();
  if (backend.iface == NULL)
    return -ENODEV;
  k_msgq_init(&backend.tx_queue, tx_queue_storage, sizeof(wifi_tx_frame_t),
              WLH_WIFI_TX_QUEUE_DEPTH);
  k_mutex_init(&backend.ingress_lock);
  atomic_set(&backend.transport_alive, 1);
  net_mgmt_init_event_callback(&backend.event_callback, wifi_event_handler,
                               events);
  net_mgmt_add_event_callback(&backend.event_callback);

  backend.packet_socket =
      zsock_socket(NET_AF_PACKET, NET_SOCK_RAW, net_htons(ETH_P_ALL));
  if (backend.packet_socket < 0)
    return -errno;
  address.sll_family = NET_AF_PACKET;
  address.sll_ifindex = net_if_get_by_iface(backend.iface);
  address.sll_protocol = net_htons(ETH_P_ALL);
  if (zsock_bind(backend.packet_socket, (const struct net_sockaddr *)&address,
                 sizeof(address)) < 0)
    return -errno;

  k_thread_create(&backend.tx_thread, wifi_tx_stack,
                  K_THREAD_STACK_SIZEOF(wifi_tx_stack), tx_thread_entry, NULL,
                  NULL, NULL, K_PRIO_PREEMPT(4), 0, K_NO_WAIT);
  k_thread_name_set(&backend.tx_thread, "wlh-wifi-tx");
  k_thread_create(&backend.rx_thread, wifi_rx_stack,
                  K_THREAD_STACK_SIZEOF(wifi_rx_stack), rx_thread_entry, NULL,
                  NULL, NULL, K_PRIO_PREEMPT(4), 0, K_NO_WAIT);
  k_thread_name_set(&backend.rx_thread, "wlh-wifi-rx");
  return 0;
}

wlh_coproc_wifi_ops_t wlh_wifi_backend_ops(void) {
  return (wlh_coproc_wifi_ops_t){
      NULL,          wifi_initialize, wifi_scan, wifi_connect, wifi_disconnect,
      wifi_start_ap, wifi_stop_ap,
  };
}

uint32_t wlh_wifi_backend_rx_capacity(void) { return WLH_WIFI_TX_QUEUE_DEPTH; }

void wlh_wifi_backend_transport_dead(void) {
  wifi_tx_frame_t frame;
  k_mutex_lock(&backend.ingress_lock, K_FOREVER);
  atomic_clear(&backend.transport_alive);
  while (k_msgq_get(&backend.tx_queue, &frame, K_NO_WAIT) == 0) {
    (void)wlh_coproc_ethernet_rx_complete(backend.coproc, frame.session_id,
                                          frame.channel, 1u,
                                          WLH_COPROC_TX_CANCELLED);
  }
  k_mutex_unlock(&backend.ingress_lock);
}

void wlh_wifi_backend_transport_alive(void) {
  k_mutex_lock(&backend.ingress_lock, K_FOREVER);
  atomic_set(&backend.transport_alive, 1);
  k_mutex_unlock(&backend.ingress_lock);
}
