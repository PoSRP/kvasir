# STM32 Sensor Capture — System Design

## Overview

A multi-sensor monitoring system, built around STM32F411CEU6 (WeAct Black Pill)
nodes connected to a Yocto Linux embedded PC fleet. Sensors cover electrical measurements
(voltage, current), temperatures, and moisture detection. Data is aggregated centrally,
stored in a time-series database, and visualised via a LAN dashboard.

This document is the authoritative design reference. Consult it after context compaction.

---

## Hardware

### Sensor nodes

- **MCU**: STM32F411CEU6 (WeAct Black Pill). Same firmware on all boards.
- **AFE boards**: Custom hand-made breakout boards per sensor type. Everything is
  conditioned to either a voltage the ADC can read, or presented over SPI/I2C.
  No on-MCU AFE assumptions — the MCU just sees voltages and digital buses.
- **Connectivity**: USB CDC to the host PC. Short cable runs only (within a hub enclosure).
  Long-distance runs use wired Ethernet between hubs, not USB.
- **Pinout**: To be finalised. Will determine the exact set of ADC channels and
  peripheral interfaces (SPI, I2C, possibly UART) exposed by the firmware.
  Pin functions are fixed — they do not change between boards.

### Host (aggregator hub)

- Yocto Linux embedded PC. One hub per physical location on the boat.
- Multiple Black Pills connected via USB to each hub.
- Hubs communicate with each other and with dashboard/database devices over Ethernet.
- OTA firmware updates managed by Mender (for the Yocto OS and applications).
  STM32 firmware updates via USB from the hub (see Bootloader section).

---

## Firmware Architecture

### Current state

- Single ADC channel (ch_id=0, ADC_IN0), DMA circular buffer, SPSC ring buffer, USB CDC streaming.
- `adc_iface()` / `usb_iface()` injected at link time (HAL or test stubs).
- Full session lifecycle: USB connect → DEVICE_INFO → CONFIG → START → STREAM_DATA → STOP.
- Wire protocol: `[0xAD][TYPE:1][LENGTH:2BE][PAYLOAD...]` — unknown types skipped via LENGTH.
- Firmware state machine: UNCONFIGURED → CONFIGURED → RUNNING. DTR drop → UNCONFIGURED.

### Target state

All of the below is planned but not yet implemented.

#### Channel types (fixed enum, compile-time)

Since all boards run identical firmware, the channel layout is static. The type enum
describes the **wire format and cadence**, not the physical peripheral:

| Value  | Name       | Description                                                        |
|--------|------------|--------------------------------------------------------------------|
| `0x00` | `STREAM`   | Continuous high-rate `uint16_t` samples. ADC via DMA.             |
| `0x01` | `PERIODIC` | Autonomous low-rate polling. STM32 polls SPI/I2C/USART and streams results. |
| `0x02` | `EVENT`    | Asynchronous state change. Digital inputs, moisture.               |

SPI and I2C both produce `PERIODIC` frames — the protocol does not distinguish the
physical bus, only the data format. The STM32 polls sensors at a configured interval;
the host does not request individual samples.

#### Channel layout — Black Pill (board-specific, published via DEVICE_INFO)

| ID | Name    | Type     | Hardware                          |
|----|---------|----------|-----------------------------------|
| 0  | ADC_IN0 | STREAM   | PA0                               |
| 1  | ADC_IN1 | STREAM   | PA1                               |
| 2  | ADC_IN2 | STREAM   | PA2                               |
| 3  | ADC_IN3 | STREAM   | PA3                               |
| 4  | ADC_IN5 | STREAM   | PA5 (IN4 used for SPI1_NSS)       |
| 5  | ADC_IN6 | STREAM   | PA6                               |
| 6  | ADC_IN7 | STREAM   | PA7                               |
| 7  | ADC_IN8 | STREAM   | PB0                               |
| 8  | ADC_IN9 | STREAM   | PB1                               |
| 9  | SPI1    | PERIODIC | PB3/4/5, NSS PA4                  |
| 10 | SPI2    | PERIODIC | PB13/14/15, NSS PB12              |
| 11 | I2C1    | PERIODIC | PB6/7                             |
| 12 | I2C2    | PERIODIC | PB10/9                            |
| 13 | I2C3    | PERIODIC | PA8/PB8, SMBA PA9 (SMBus host)   |
| 14 | USART1  | PERIODIC | PA10 RX / PA15 TX                 |

The host discovers this layout at runtime from DEVICE_INFO and never hardcodes channel IDs.
A different board variant swaps `firmware/app/channels.hpp` only.

---

## Wire Protocol

### Frame structure

All frames (both directions):

```
[0xAD] [TYPE:1] [LENGTH:2BE] [PAYLOAD:LENGTH bytes]
```

- `0xAD` — fixed magic byte, enables resync after corruption.
- `TYPE` — frame type byte (see below).
- `LENGTH` — payload length in bytes, big-endian uint16.
- Parsers skip unknown types using LENGTH; no desync on new type additions.

### Frame types — device → host

| Type   | Name          | Payload                                                                    |
|--------|---------------|----------------------------------------------------------------------------|
| `0x00` | DEVICE_INFO   | `fw_major(1) fw_minor(1) ch_count(1) [ch_id(1) ch_type(1) name_len(1) name(N)]...` |
| `0x01` | STREAM_DATA   | `ch_id(1) seq(2BE) count(2BE) samples[](uint16BE)`                        |
| `0x02` | PERIODIC_DATA | `ch_id(1) timestamp_ms(4BE) value_type(1) value(4BE)`                     |
| `0x03` | EVENT_DATA    | `ch_id(1) state(1) timestamp_ms(4BE)`                                     |
| `0x10` | ACK           | `cmd_type(1) status(1) [ch_id(1) actual_cfg_len(1) actual_cfg(N)]...`     |

ACK status codes: `0x00` OK, `0x01` ERR_INVALID, `0x02` ERR_BAD_STATE.

CONFIG ACK includes per-channel actual achieved config after the status byte.
For START/STOP/DFU ACKs, the payload is just the 2-byte cmd+status.

PERIODIC_DATA `value_type`: `0x00` = IEEE 754 float big-endian. Others TBD per sensor driver.

### Frame types — host → device

| Type   | Name    | Payload                                                               |
|--------|---------|-----------------------------------------------------------------------|
| `0x80` | CONFIG  | `[ch_id(1) cfg_len(1) cfg_data(N)]...`                               |
| `0x81` | START   | (empty) — begin capture on all channels                               |
| `0x82` | STOP    | (empty) — halt capture                                                |
| `0xF0` | DFU     | Reserved. Triggers reboot to bootloader. Payload TBD (bootloader project). |

**CONFIG per channel type:**
- STREAM: `enabled(1) sampling_time(1)` = 2 bytes. `sampling_time` is an ADC SMPR enum value 0–7
  (→ 3/15/28/56/84/112/144/480 cycles). Per-channel; different channels may use different values.
  Actual achieved sample rate is returned in the CONFIG ACK (computed from total scan timing).
- PERIODIC: `enabled(1) poll_interval_ms(2BE) sensor_type(1)` = 4 bytes
- EVENT: `enabled(1) debounce_ms(2BE)` = 3 bytes

### Sequence numbers

Sequence numbers apply to `STREAM_DATA` frames only (high-rate ADC). `PERIODIC_DATA`
and `EVENT_DATA` frames carry timestamps instead; gaps are expected and not an error.

---

## Session Lifecycle

```
Host                                Device
  |                                   |
  |--- (USB connect, DTR asserted) -->|  device sends DEVICE_INFO
  |<-- DEVICE_INFO -------------------|
  |--- CONFIG ------------------------>|  device applies interface config
  |<-- ACK (CONFIG) ------------------|
  |--- START ------------------------->|  device begins capture
  |<-- ACK (START) -------------------|
  |<-- STREAM_DATA / PERIODIC_DATA / EVENT_DATA (continuous)
  |--- STOP -------------------------->|
  |<-- ACK (STOP) --------------------|
  |                                   |
  |--- (DTR drop / cable pull) ------->|  device resets to unconfigured
```

**DTR** is a safety net, not a control signal. Ungraceful disconnect (host crash, cable
pull) is detected via DTR low and causes the device to stop capture and return to the
unconfigured state, ready for a fresh CONFIG+START sequence.

Normal session control is entirely via command frames.

**Firmware state machine**: `UNCONFIGURED → CONFIGURED → RUNNING`
- UNCONFIGURED: waiting for CONFIG command. Ignores START.
- CONFIGURED: ready to capture. Peripherals initialised per received config.
- RUNNING: actively capturing. Streaming frames to host.
- Any DTR drop → back to UNCONFIGURED.

---

## Host Software

### Current state

- `tools/protocol.py`: Python mirror of all protocol enums and constants.
- `tools/frame_parser.py`: streaming parser for the new `[0xAD][TYPE][LEN][PAYLOAD]` protocol. Returns typed dataclasses: `DeviceInfoFrame`, `StreamDataFrame`, `PeriodicDataFrame`, `EventDataFrame`, `AckFrame`. Unknown types skipped via LENGTH.
- `tools/session.py`: manages the connect → DEVICE_INFO → CONFIG → START → stream → STOP lifecycle.
- `tools/config.py`: YAML config loader, per-channel scale/offset/unit/sample_rate/sampling_time; `channel` field matches hardware names from DEVICE_INFO.
- `tools/monitor.py`: real-time pyqtgraph display + HDF5 logging. Routes by channel_id from DEVICE_INFO.

### Target architecture

```
[Black Pill] --USB--> [Collector process]
[Black Pill] --USB--> [Collector process]  -->  [InfluxDB]  -->  [Grafana]
[Black Pill] --USB--> [Collector process]
```

- One collector process per USB device (or one process managing all devices on a hub).
- Collector implements the session lifecycle: connect → DEVICE_INFO → CONFIG → START → stream.
- Per-channel decimation configured per channel — a temperature channel might store 1 Sa/s,
  a battery voltage channel might store 100 Sa/s, a high-rate ADC channel stores 1 kSa/s.
  Full-rate data optionally archived to local HDF5 before decimation.
- Config source: YAML now (development), central database later. The CONFIG command frame
  is the canonical schema — YAML and DB are just sources that populate it.
- pyqtgraph monitor retained as a development/debug tool, not a production component.

---

## Bootloader

A/B firmware layout. Bootloader triggered via the `DFU` command frame (`0xF0`) over the
same USB CDC connection used for data. The bootloader is a **separate project** — only the
frame type is reserved here to avoid future protocol conflicts.

The bootloader owns all flash write operations. Firmware never writes to flash.

---

## CI / Build

- `./build.sh build` — firmware ELF (ARM cross-compiler, Docker)
- `./build.sh test` — Catch2 unit tests (native, Docker)
- `./build.sh toolstest` — pytest for Python tools (Docker)
- `./build.sh host` — Linux capture utility (native, Docker)
- `./build.sh functest` — host utility + pytest against it (Docker)

GitHub Actions on self-hosted runner (`selfhosted-stm32-sensor-capture`).

---
