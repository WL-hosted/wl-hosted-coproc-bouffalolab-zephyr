/* SPDX-License-Identifier: Apache-2.0 */
#include "transport.h"

#include <stdbool.h>
#include <string.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>

#include "firmware_config.h"
#undef DIV_ROUND_CLOSEST
#include "usbd_core.h"

LOG_MODULE_REGISTER(wlh_usb, LOG_LEVEL_INF);

#define FRAME_HEADER_SIZE 24u
#define FRAME_MAGIC_BYTE0 0x57u
#define FRAME_MAGIC_BYTE1 0x4cu
#define FRAME_PROTOCOL_MAJOR 1u
#define FRAME_FLAGS_MASK 0x03u
#define FRAME_CHANNEL_OFFSET 4u

#define USB_CONFIG_SIZE (9 + 9 + 7 + 7)
#define USB_BASE 0x20072000u
#define USB_CONFIGURED_BIT BIT(0)

typedef struct tx_job {
  uint8_t *frame;
  size_t size;
  wlh_coproc_tx_complete_fn completion;
  void *completion_context;
} tx_job_t;

typedef struct usb_transport {
  wlh_coproc_t *coproc;
  size_t max_frame_size;
  wlh_transport_reset_fn on_reset;
  void *reset_context;
  struct ring_buf rx_ring;
  struct k_msgq tx_control_queue;
  struct k_msgq tx_data_queue;
  struct k_sem tx_done;
  struct k_sem tx_wakeup;
  struct k_sem rx_available;
  struct k_event events;
  struct k_thread tx_thread;
  struct k_thread rx_thread;
  atomic_t stopping;
  atomic_t reset_pending;
  atomic_t out_paused;
  atomic_t initial_configuration_seen;
  uint8_t rx_frame[FRAME_HEADER_SIZE + WLH_COPROC_MAX_FRAME_SIZE];
  size_t rx_frame_length;
} usb_transport_t;

static usb_transport_t transport;
static uint8_t rx_ring_storage[WLH_USB_RX_RING_SIZE];
static uint8_t
    tx_control_storage[WLH_USB_CONTROL_TX_QUEUE_DEPTH * sizeof(tx_job_t)];
static uint8_t tx_data_storage[WLH_USB_DATA_TX_QUEUE_DEPTH * sizeof(tx_job_t)];

K_THREAD_STACK_DEFINE(usb_tx_stack, 3072);
K_THREAD_STACK_DEFINE(usb_rx_stack, 3072);

USB_NOCACHE_RAM_SECTION
__aligned(CONFIG_USB_ALIGN_SIZE) static uint8_t out_chunk[WLH_USB_EP_MPS];
USB_NOCACHE_RAM_SECTION __aligned(CONFIG_USB_ALIGN_SIZE) static uint8_t
    tx_dma_buffer[WLH_COPROC_MAX_FRAME_SIZE];
static uint8_t rx_drain[WLH_USB_EP_MPS];

static char serial_string[17];
static const char langid_string[] = {0x09, 0x04};

extern uint8_t __USB_NOCACHE_start[];
extern uint8_t __USB_NOCACHE_end[];

static const uint8_t device_descriptor[] = {USB_DEVICE_DESCRIPTOR_INIT(
    USB_2_0, 0x00, 0x00, 0x00, WLH_USB_VID, WLH_USB_PID, 0x0100, 0x01)};

static const uint8_t config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x01, 0x01,
                               USB_CONFIG_BUS_POWERED,
                               WLH_USB_MAX_POWER_MA / 2u),
    USB_INTERFACE_DESCRIPTOR_INIT(0x00, 0x00, 0x02, 0xff, 0x00, 0x00, 0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(WLH_USB_EP_OUT, USB_ENDPOINT_TYPE_BULK,
                                 WLH_USB_EP_MPS, 0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(WLH_USB_EP_IN, USB_ENDPOINT_TYPE_BULK,
                                 WLH_USB_EP_MPS, 0x00),
};

static const uint8_t *device_descriptor_callback(uint8_t speed) {
  (void)speed;
  return device_descriptor;
}

static const uint8_t *config_descriptor_callback(uint8_t speed) {
  (void)speed;
  return config_descriptor;
}

static const char *string_descriptor_callback(uint8_t speed, uint8_t index) {
  static const char *strings[] = {
      langid_string,
      "WL-hosted",
      "WL-hosted BL616 Coprocessor",
      serial_string,
  };
  (void)speed;
  return index < ARRAY_SIZE(strings) ? strings[index] : NULL;
}

static const struct usb_descriptor usb_descriptors = {
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = config_descriptor_callback,
    .string_descriptor_callback = string_descriptor_callback,
};

static bool is_control_frame(const uint8_t *frame, size_t size) {
  return frame != NULL && size >= FRAME_HEADER_SIZE &&
         (frame[FRAME_CHANNEL_OFFSET] == WLH_CHANNEL_LINK_CONTROL ||
          frame[FRAME_CHANNEL_OFFSET] == WLH_CHANNEL_CONTROL_RPC);
}

static bool frame_header_plausible(const uint8_t *header) {
  return header[0] == FRAME_MAGIC_BYTE0 && header[1] == FRAME_MAGIC_BYTE1 &&
         header[2] == FRAME_PROTOCOL_MAJOR && header[3] == FRAME_HEADER_SIZE &&
         (header[5] & (uint8_t)~FRAME_FLAGS_MASK) == 0u;
}

static void rx_consume(size_t count) {
  transport.rx_frame_length -= count;
  if (transport.rx_frame_length != 0u) {
    memmove(transport.rx_frame, transport.rx_frame + count,
            transport.rx_frame_length);
  }
}

static void rx_feed(const uint8_t *data, size_t size) {
  if (transport.rx_frame_length + size > sizeof(transport.rx_frame)) {
    LOG_WRN("RX accumulator overflow, resynchronizing");
    transport.rx_frame_length = 0u;
    return;
  }
  memcpy(transport.rx_frame + transport.rx_frame_length, data, size);
  transport.rx_frame_length += size;

  while (transport.rx_frame_length >= FRAME_HEADER_SIZE) {
    size_t frame_size;
    if (!frame_header_plausible(transport.rx_frame)) {
      rx_consume(1u);
      continue;
    }
    frame_size = FRAME_HEADER_SIZE + (size_t)transport.rx_frame[6] +
                 ((size_t)transport.rx_frame[7] << 8);
    if (frame_size > transport.max_frame_size) {
      rx_consume(1u);
      continue;
    }
    if (transport.rx_frame_length < frame_size)
      break;
    if (wlh_coproc_on_frame(transport.coproc, transport.rx_frame, frame_size) !=
        WLH_COPROC_OK) {
      LOG_WRN("RX frame rejected (%u bytes)", (unsigned int)frame_size);
    }
    rx_consume(frame_size);
  }
}

static void arm_out_read(uint8_t busid) {
  if (usbd_ep_start_read(busid, WLH_USB_EP_OUT, out_chunk, sizeof(out_chunk)) !=
      0) {
    LOG_WRN("failed to arm bulk OUT");
  }
}

static void out_endpoint_callback(uint8_t busid, uint8_t ep, uint32_t nbytes) {
  (void)ep;
  if (nbytes != 0u && atomic_get(&transport.stopping) == 0) {
    uint32_t written = ring_buf_put(&transport.rx_ring, out_chunk, nbytes);
    if (written != nbytes) {
      LOG_ERR("RX ring invariant violated");
      atomic_set(&transport.reset_pending, 1);
      k_sem_give(&transport.tx_wakeup);
    } else {
      k_sem_give(&transport.rx_available);
    }
  }
  if (atomic_get(&transport.stopping) == 0 &&
      ring_buf_space_get(&transport.rx_ring) >= WLH_USB_EP_MPS) {
    arm_out_read(busid);
  } else {
    atomic_set(&transport.out_paused, 1);
  }
}

static void in_endpoint_callback(uint8_t busid, uint8_t ep, uint32_t nbytes) {
  (void)busid;
  (void)ep;
  (void)nbytes;
  k_sem_give(&transport.tx_done);
}

static struct usbd_endpoint out_endpoint = {
    .ep_addr = WLH_USB_EP_OUT,
    .ep_cb = out_endpoint_callback,
};
static struct usbd_endpoint in_endpoint = {
    .ep_addr = WLH_USB_EP_IN,
    .ep_cb = in_endpoint_callback,
};
static struct usbd_interface vendor_interface;

static void usb_event_handler(uint8_t busid, uint8_t event) {
  switch (event) {
  case USBD_EVENT_RESET:
    k_event_clear(&transport.events, USB_CONFIGURED_BIT);
    /* The first bus reset is part of normal enumeration and there is no
     * Host session to tear down yet. Every later reset invalidates the
     * negotiated Core session and its outstanding transfers. */
    if (atomic_get(&transport.initial_configuration_seen) != 0)
      atomic_set(&transport.reset_pending, 1);
    atomic_clear(&transport.out_paused);
    k_sem_give(&transport.tx_done);
    k_sem_give(&transport.tx_wakeup);
    break;
  case USBD_EVENT_CONFIGURED:
    if (atomic_cas(&transport.initial_configuration_seen, 0, 1))
      atomic_clear(&transport.reset_pending);
    arm_out_read(busid);
    k_event_post(&transport.events, USB_CONFIGURED_BIT);
    break;
  default:
    break;
  }
}

static void usb_stack_register(void) {
  usbd_desc_register(WLH_USB_BUS_ID, &usb_descriptors);
  usbd_add_interface(WLH_USB_BUS_ID, &vendor_interface);
  usbd_add_endpoint(WLH_USB_BUS_ID, &out_endpoint);
  usbd_add_endpoint(WLH_USB_BUS_ID, &in_endpoint);
}

static void flush_tx_queue(int status) {
  tx_job_t job;
  struct k_msgq *queues[] = {
      &transport.tx_control_queue,
      &transport.tx_data_queue,
  };
  for (size_t index = 0u; index < ARRAY_SIZE(queues); ++index) {
    while (k_msgq_get(queues[index], &job, K_NO_WAIT) == 0) {
      if (job.frame != NULL) {
        job.completion(job.completion_context, job.frame, job.size, status);
      }
    }
  }
}

static void handle_reset(void) {
  if (!atomic_cas(&transport.reset_pending, 1, 0))
    return;
  flush_tx_queue(WLH_COPROC_TX_CANCELLED);
  transport.rx_frame_length = 0u;
  ring_buf_reset(&transport.rx_ring);
  if (transport.on_reset != NULL)
    transport.on_reset(transport.reset_context);
}

int wlh_transport_submit_tx(void *context, uint8_t *frame, size_t size,
                            wlh_coproc_tx_complete_fn completion,
                            void *completion_context) {
  tx_job_t job = {frame, size, completion, completion_context};
  struct k_msgq *queue;
  (void)context;
  if (frame == NULL || completion == NULL || size > sizeof(tx_dma_buffer) ||
      atomic_get(&transport.stopping) != 0)
    return -1;
  queue = is_control_frame(frame, size) ? &transport.tx_control_queue
                                        : &transport.tx_data_queue;
  if (k_msgq_put(queue, &job, K_NO_WAIT) != 0)
    return -1;
  k_sem_give(&transport.tx_wakeup);
  return 0;
}

static bool next_tx_job(tx_job_t *job) {
  return k_msgq_get(&transport.tx_control_queue, job, K_NO_WAIT) == 0 ||
         k_msgq_get(&transport.tx_data_queue, job, K_NO_WAIT) == 0;
}

static void tx_thread_entry(void *first, void *second, void *third) {
  tx_job_t job;
  (void)first;
  (void)second;
  (void)third;
  for (;;) {
    k_sem_take(&transport.tx_wakeup, K_FOREVER);
    handle_reset();
    while (next_tx_job(&job)) {
      int status = 0;
      while ((k_event_wait(&transport.events, USB_CONFIGURED_BIT, false,
                           K_MSEC(100)) &
              USB_CONFIGURED_BIT) == 0u) {
        if (atomic_get(&transport.reset_pending) != 0)
          break;
      }
      if (atomic_get(&transport.reset_pending) != 0) {
        status = WLH_COPROC_TX_CANCELLED;
      } else {
        memcpy(tx_dma_buffer, job.frame, job.size);
        k_sem_reset(&transport.tx_done);
        if (usbd_ep_start_write(WLH_USB_BUS_ID, WLH_USB_EP_IN, tx_dma_buffer,
                                job.size) != 0 ||
            k_sem_take(&transport.tx_done, K_MSEC(WLH_USB_TX_TIMEOUT_MS)) !=
                0 ||
            atomic_get(&transport.reset_pending) != 0) {
          status = WLH_COPROC_TX_CANCELLED;
        }
      }
      job.completion(job.completion_context, job.frame, job.size, status);
      if (status != 0) {
        atomic_set(&transport.reset_pending, 1);
        handle_reset();
        break;
      }
    }
  }
}

static void rx_thread_entry(void *first, void *second, void *third) {
  (void)first;
  (void)second;
  (void)third;
  for (;;) {
    k_sem_take(&transport.rx_available, K_FOREVER);
    for (;;) {
      uint32_t size =
          ring_buf_get(&transport.rx_ring, rx_drain, sizeof(rx_drain));
      if (size == 0u)
        break;
      rx_feed(rx_drain, size);
    }
    if (atomic_cas(&transport.out_paused, 1, 0) &&
        atomic_get(&transport.stopping) == 0 &&
        ring_buf_space_get(&transport.rx_ring) >= WLH_USB_EP_MPS) {
      arm_out_read(WLH_USB_BUS_ID);
    }
  }
}

static void fill_serial_string(void) {
  uint8_t id[8] = {0};
  static const char hex[] = "0123456789abcdef";
  ssize_t count = hwinfo_get_device_id(id, sizeof(id));
  if (count < 0)
    count = 0;
  for (size_t index = 0u; index < sizeof(id); ++index) {
    uint8_t value = index < (size_t)count ? id[index] : 0u;
    serial_string[index * 2u] = hex[value >> 4];
    serial_string[index * 2u + 1u] = hex[value & 0x0fu];
  }
  serial_string[sizeof(serial_string) - 1u] = '\0';
}

int wlh_transport_start(const wlh_transport_config_t *config) {
  if (config == NULL || config->coproc == NULL ||
      config->max_frame_size > WLH_COPROC_MAX_FRAME_SIZE)
    return -1;
  memset(&transport, 0, sizeof(transport));
  transport.coproc = config->coproc;
  transport.max_frame_size = config->max_frame_size;
  transport.on_reset = config->on_reset;
  transport.reset_context = config->reset_context;
  ring_buf_init(&transport.rx_ring, sizeof(rx_ring_storage), rx_ring_storage);
  k_msgq_init(&transport.tx_control_queue, tx_control_storage, sizeof(tx_job_t),
              WLH_USB_CONTROL_TX_QUEUE_DEPTH);
  k_msgq_init(&transport.tx_data_queue, tx_data_storage, sizeof(tx_job_t),
              WLH_USB_DATA_TX_QUEUE_DEPTH);
  k_sem_init(&transport.tx_done, 0u, 1u);
  k_sem_init(&transport.tx_wakeup, 0u, 1u);
  k_sem_init(&transport.rx_available, 0u, 1u);
  k_event_init(&transport.events);
  fill_serial_string();

  /* Devicetree memory regions are NOLOAD. Clear CherryUSB state and DMA
   * buffers before registering descriptors on every cold boot. */
  memset(__USB_NOCACHE_start, 0,
         (size_t)(__USB_NOCACHE_end - __USB_NOCACHE_start));

  k_thread_create(&transport.tx_thread, usb_tx_stack,
                  K_THREAD_STACK_SIZEOF(usb_tx_stack), tx_thread_entry, NULL,
                  NULL, NULL, K_PRIO_PREEMPT(3), 0, K_NO_WAIT);
  k_thread_name_set(&transport.tx_thread, "wlh-usb-tx");
  k_thread_create(&transport.rx_thread, usb_rx_stack,
                  K_THREAD_STACK_SIZEOF(usb_rx_stack), rx_thread_entry, NULL,
                  NULL, NULL, K_PRIO_PREEMPT(3), 0, K_NO_WAIT);
  k_thread_name_set(&transport.rx_thread, "wlh-usb-rx");

  usb_stack_register();
  if (usbd_initialize(WLH_USB_BUS_ID, USB_BASE, usb_event_handler) != 0)
    return -1;
  LOG_INF("CherryUSB ready: %04x:%04x HS bulk", WLH_USB_VID, WLH_USB_PID);
  return 0;
}

size_t wlh_transport_max_frame_size(void) { return WLH_COPROC_MAX_FRAME_SIZE; }

size_t wlh_transport_tx_capacity(void) { return WLH_USB_DATA_TX_QUEUE_DEPTH; }
