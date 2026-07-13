<h1 align="center">Kvasir</h1>

<p align="center">
  Analog voltage DAQ on a Black Pill (STM32F411CEU6) streamed over USB CDC.
</p>

<p align="center">
  <a href="https://github.com/PoSRP/kvasir/actions/workflows/firmware.yaml"><img alt="Firmware" src="https://github.com/PoSRP/kvasir/actions/workflows/firmware.yaml/badge.svg"></a>
  <a href="https://github.com/PoSRP/kvasir/actions/workflows/unit-tests.yaml"><img alt="Unit Tests" src="https://github.com/PoSRP/kvasir/actions/workflows/unit-tests.yaml/badge.svg"></a>
  <a href="https://github.com/PoSRP/kvasir/actions/workflows/tools-tests.yaml"><img alt="Tools Tests" src="https://github.com/PoSRP/kvasir/actions/workflows/tools-tests.yaml/badge.svg"></a>
</p>

---

## Board

STM32F411CEU6 (WeAct Black Pill). Ten ADC channels, fixed pinout. Only the
channels named in the host config are enabled, so the scan rate is spent on
the ones we actually want.

<div align="center">

| ID | Name    | Pin |
|----|---------|-----|
| 0  | ADC_IN0 | PA0 |
| 1  | ADC_IN1 | PA1 |
| 2  | ADC_IN2 | PA2 |
| 3  | ADC_IN3 | PA3 |
| 4  | ADC_IN4 | PA4 |
| 5  | ADC_IN5 | PA5 |
| 6  | ADC_IN6 | PA6 |
| 7  | ADC_IN7 | PA7 |
| 8  | ADC_IN8 | PB0 |
| 9  | ADC_IN9 | PB1 |

</div>

Multiple boards can run in parallel on the same host.
The monitor tool maps each board by USB serial number.

## Wire Protocol

Every frame, both directions:

```
[0xAD] [TYPE:1] [LENGTH:2BE] [PAYLOAD:LENGTH]
```

`0xAD` enables resync after corruption. Parsers skip unknown types via LENGTH.

### Device -> Host

<div align="center">

| Type   | Name        | Payload                                                                |
|--------|-------------|------------------------------------------------------------------------|
| `0x01` | STREAM_DATA | `ch_id(1) seq(2BE) count(2BE) samples[](uint16BE)`                     |
| `0x10` | ACK         | `cmd_type(1) status(1)`                                                |

</div>

- ACK status: `0x00` OK, `0x01` ERROR.

### Host -> Device

<div align="center">

| Type   | Name    | Payload                                        |
|--------|---------|------------------------------------------------|
| `0x80` | CONFIG  | `[ch_id(1) cfg_len(1) enabled(1) sampling_time(1)]...` |
| `0x81` | START   | (empty)                                        |
| `0x82` | STOP    | (empty)                                        |

</div>

`sampling_time` is an ADC SMPR enum 0-7 (3 / 15 / 28 / 56 / 84 / 112 / 144 / 480
cycles) and is set per channel.
The host computes the achieved per-channel sample rate from the ADC clock and the
enabled sampling times (`compute_sample_rate` in `tools/monitor/session.py`).

Bootloader entry is not a frame type -- the bootloader (ymir) watches the raw
byte stream for a magic pattern and reboots into DFU when it sees one.

## Session

State machine: `UNCONFIGURED -> CONFIGURED -> RUNNING`. Any DTR drop resets to
UNCONFIGURED.

```
Host                              Device
  |                                 |
  |--- USB connect, DTR high ------>|
  |--- STOP ----------------------->|  forces UNCONFIGURED regardless of prior state
  |<-- ACK (STOP) ------------------|
  |--- CONFIG --------------------->|
  |<-- ACK (CONFIG) ----------------|
  |--- START ---------------------->|
  |<-- ACK (START) -----------------|
  |<-- STREAM_DATA (continuous)
  |--- STOP ----------------------->|
  |<-- ACK (STOP) ------------------|
  |--- DTR drop / cable pull ------>|  device returns to UNCONFIGURED
```

DTR is a safety net for ungraceful disconnects, not a control signal. Normal
session control is entirely via command frames.

## build.sh

This project is set up to build A and B images and include a USB bootloader.
For the complexity we have, it's a bit silly maybe, but I wanted to test my
bootloader in a real project as well, so why not.

```text
Usage: ./build.sh [command]

Application firmware (slot images for the ymir bootloader):
  build           Build firmware for slot A
                    Output: firmware/build/slot-a/kvasir.elf
  build-b         Build firmware for slot B
                    Output: firmware/build/slot-b/kvasir.elf
  flash [SERIAL]  Build slot-a, send DFU trigger, upload via USB CDC using
                    firmware/ymir/scripts/flash_tool.sh
                    Requires the bootloader to already be on the device.
                    SERIAL (optional) targets a specific board by its USB
                    serial number required when multiple boards are attached.

Bootloader (one-time install with ST-Link):
  bootloader      Build the ymir bootloader from the submodule
                    Output: firmware/ymir/firmware/build/Debug/ymir.elf
  flash-bootloader  Build bootloader, mass-erase flash, program via ST-Link + OpenOCD

Tests:
  test            Build and run Catch2 unit tests (ring buffer, frame packer, pipeline)
  toolstest       Run pytest for the Python monitor/logger tools (no hardware)
                    Tests: tests/tools/

Lint:
  tidy [FILES...] Run clang-tidy across firmware/app.
                    With no args lints every source in the compile DB.
                    With repo-relative source paths (e.g. from pre-commit)
                    lints only those.

  help            Show this message

All build steps run inside Docker. CPU usage is capped at (nproc - 2).
`flash` invokes ymir/scripts/flash_tool.sh on the host (needs bash + python3).
```
