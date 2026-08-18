# WL-hosted BL616 Zephyr Coprocessor

This repository adapts the portable WL-hosted Coprocessor Core to the
Bouffalo Lab BL616 using Zephyr. The MVP targets the
`ai_m62_12f_kit/bl616c50q2i` board definition and the electrically compatible
WL-hosted `bl616-spi-usb` hardware.

The firmware uses the Zephyr BL616 Wi-Fi driver and a CherryUSB vendor bulk
device. Zephyr's native BL616 USB device support is not used.

| Capability | MVP status |
|---|---|
| Wi-Fi station: initialize, scan, connect, disconnect | Supported |
| Wi-Fi SoftAP: start, stop, client events | Supported |
| Raw Ethernet data plane over STA or AP | Supported |
| USB High Speed transport with CherryUSB | Supported |
| Simultaneous STA + AP | Not supported; modes are mutually exclusive |
| BLE controller / HCI channel | Planned |
| SPI transport | Planned |

```text
USB host
  <-> CherryUSB vendor bulk adapter
  <-> core/coproc-core (wire, session, credit, RPC)
  <-> Zephyr Wi-Fi backend
  <-> BL616 radio
```

USB carries unmodified WL-hosted wire frames. Simulator IPC and sideband
messages are not part of this firmware.

## Dependencies

The repository pins all inputs needed by local builds and CI:

- `west.yml` pins Zephyr and imports only the BL616 HAL, hostap, and Mbed TLS
  modules required by the Wi-Fi build;
- `core/` pins `wl-hosted-core`, including its Zephyr OSAL adapter;
- `third_party/CherryUSB/` pins CherryUSB;
- `SUBMODULE.lock` records both submodule gitlinks as full commit IDs.

Initialize the recursive Git submodules before building:

```sh
git submodule update --init --recursive
git submodule status --recursive
```

The first BL616 Wi-Fi build also needs the redistributable BL61x HAL blobs.
They are fetched through Zephyr's blob command and are not committed here.

## Build in a new west workspace

Python 3.12, west, CMake, Ninja, and the RISC-V Zephyr SDK toolchain are
required. A clean workspace can be created as follows:

```sh
mkdir wlh-bl616-workspace
cd wlh-bl616-workspace
git clone --recurse-submodules \
  https://github.com/WL-hosted/wl-hosted-coproc-bouffalolab-zephyr.git app
west init -l app
west update
west blobs fetch --allow-regex '^lib/bl61x/' hal_bouffalolab
west build -p always -b ai_m62_12f_kit/bl616c50q2i app
```

The main outputs are `build/zephyr/zephyr.bin`, `zephyr.elf`, and
`zephyr.map`. Flashing uses the board runner configured by Zephyr:

```sh
west flash
```

## Build with the existing Zephyr checkout

For the checkout at `/Volumes/aigo_1t/DevPkgs/zephyrproject`, activate its
Python environment and make the Bouffalo HAL visible if the workspace filters
that module out:

```sh
source /Volumes/aigo_1t/DevPkgs/zephyrproject/.venv/bin/activate
export ZEPHYR_BASE=/Volumes/aigo_1t/DevPkgs/zephyrproject/zephyr
west blobs fetch --allow-regex '^lib/bl61x/' hal_bouffalolab
west build -p always \
  -b ai_m62_12f_kit/bl616c50q2i \
  /Volumes/aigo_1t/Github/wl-hosted/wl-hosted-coproc-bouffalolab-zephyr \
  -- -DEXTRA_ZEPHYR_MODULES=/Volumes/aigo_1t/DevPkgs/zephyrproject/modules/hal/bouffalolab
```

## USB profile

| Field | Value |
|---|---|
| VID:PID | `303A:8201` |
| Interface | vendor-specific (`0xff`) |
| Bulk OUT | `0x01`, host to BL616 |
| Bulk IN | `0x81`, BL616 to host |
| Max packet | 512 bytes, High Speed |
| Maximum WL-hosted frame | 4096 bytes |

USB is treated as a byte stream: the receive worker reconstructs frames from
the 24-byte WL-hosted wire header and applies bounded buffering. Control frames
have a dedicated TX queue so Ethernet traffic cannot starve link/session
traffic. Bulk OUT is not rearmed while the bounded receive ring lacks space,
providing endpoint-level backpressure. A bus reset cancels outstanding work and
starts a fresh Core session after the initial enumeration.

The compatibility headers in `include/` bridge CherryUSB's Bouffalo port to
Zephyr 4.x without modifying the pinned upstream submodule. `app.overlay`
reserves the final 8 KiB of OCRAM through BL616's non-cacheable bus alias for
CherryUSB state and VDMA buffers; ordinary Zephyr SRAM cannot be used for these
buffers while the BL616 data cache is enabled.

## Runtime model

- `wlh-core`: portable Coprocessor Core state machine and RPC handling;
- `wlh-usb-tx` / `wlh-usb-rx`: CherryUSB transfer and stream reassembly;
- `wlh-wifi-tx` / `wlh-wifi-rx`: bounded raw Ethernet bridge;
- `wlh-link-control`: coalesced Core restart after USB session loss;
- Zephyr network-management callbacks: asynchronous scan/connect/AP events.

The adapter keeps hardware operations asynchronous, owns fixed-depth queues,
and returns Core TX buffers only from an explicit completion path. The Wi-Fi
backend and transport are separate modules so a later SPI adapter can replace
USB without entering Core, while a future BLE backend can attach through the
existing Core Bluetooth operations and HCI channel.

## CI

GitHub Actions creates a clean west workspace, fetches only the required BL61x
binary blobs, builds the exact pinned board/SoC target, verifies submodule locks,
and uploads the BIN, ELF, and map files. This mirrors the clean build commands
above rather than relying on a developer's existing Zephyr installation.
