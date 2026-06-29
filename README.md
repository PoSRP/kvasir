<h1 align="center">STM32 Sensor Capture</h1>

<p align="center">
  Sensor data capture from an Black Pill (STM32F411CEU6) over USB
</p>

<p align="center">
  <a href="https://github.com/PoSRP/stm32-sensor-capture/actions/workflows/firmware.yaml">
    <img alt="Firmware" src="https://github.com/PoSRP/stm32-sensor-capture/actions/workflows/firmware.yaml/badge.svg">
  </a>
  <a href="https://github.com/PoSRP/stm32-sensor-capture/actions/workflows/unit-tests.yaml">
    <img alt="Unit Tests" src="https://github.com/PoSRP/stm32-sensor-capture/actions/workflows/unit-tests.yaml/badge.svg">
  </a>
  <a href="https://github.com/PoSRP/stm32-sensor-capture/actions/workflows/host.yaml">
    <img alt="Host Build" src="https://github.com/PoSRP/stm32-sensor-capture/actions/workflows/host.yaml/badge.svg">
  </a>
  <a href="https://github.com/PoSRP/stm32-sensor-capture/actions/workflows/capture-tests.yaml">
    <img alt="Capture Tests" src="https://github.com/PoSRP/stm32-sensor-capture/actions/workflows/capture-tests.yaml/badge.svg">
  </a>
</p>

## TODO

- Why provide a sample rate for the ADCs? Just let the host compute it based on how much data it receives
- Clean up a lot of the AI-style variable naming and comments
- Enforce braces on oneline-ifs
- Namespace things for clarity
- Rename app to main, build a clear main-loop
- Integrate bootloader
- Expand setup with I2C and SPI sensors
- Add pulse-counting inputs (on ADC pins?)
- Can we get a ABI input?
- Convert byte-like usage of uint8_t to std::byte
