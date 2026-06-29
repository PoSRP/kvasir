#!/usr/bin/env bash
set -euo pipefail

CMD="${1:-build}"
CPUS=$(( $(nproc) > 2 ? $(nproc) - 2 : 1 ))
FW_IMAGE="kvasir-firmware-builder"
TEST_IMAGE="kvasir-test-builder"

case "$CMD" in
help|--help|-h)
    cat <<'EOF'
Usage: ./build.sh [command]

Application firmware (slot images for the ymir bootloader):
  build           Build firmware for slot A (default)
                    Output: firmware/build/slot-a/sensor-capture.elf
  build-b         Build firmware for slot B
                    Output: firmware/build/slot-b/sensor-capture.elf
  flash [SERIAL]  Build slot-a, send DFU trigger, upload via USB CDC using
                    firmware/ymir/scripts/flash_tool.sh
                    Requires the bootloader to already be on the device.
                    SERIAL (optional) targets a specific board by its USB
                    serial number — required when multiple boards are attached.

Bootloader (one-time install with ST-Link):
  bootloader      Build the ymir bootloader from the submodule
                    Output: firmware/ymir/firmware/build/Debug/ymir.elf
  flash-bootloader  Build bootloader, mass-erase flash, program via ST-Link + OpenOCD

Host / tests:
  test            Build and run Catch2 unit tests (ring buffer, frame packer, pipeline)
  host            Build the Linux capture tool
                    Output: host/build/capture
  functest        Build the capture tool and run pytest against it (no hardware)
                    Tests: tests/capture/
  toolstest       Run pytest for the Python monitor/logger tools (no hardware)
                    Tests: tests/tools/

  help            Show this message

All build steps run inside Docker. CPU usage is capped at (nproc - 2).
`flash` invokes ymir/scripts/flash_tool.sh on the host (needs bash + python3).
EOF
    exit 0
    ;;

build)
    docker build -t "$FW_IMAGE" docker/firmware
    docker run --rm --cpus "$CPUS" \
        -v "$(pwd):/workspace" \
        -w /workspace/firmware \
        "$FW_IMAGE" \
        bash -c "cmake --fresh --preset slot-a && cmake --build build/slot-a -j ${CPUS}"
    ;;

build-b)
    docker build -t "$FW_IMAGE" docker/firmware
    docker run --rm --cpus "$CPUS" \
        -v "$(pwd):/workspace" \
        -w /workspace/firmware \
        "$FW_IMAGE" \
        bash -c "cmake --fresh --preset slot-b && cmake --build build/slot-b -j ${CPUS}"
    ;;

bootloader)
    docker build -t "$FW_IMAGE" docker/firmware
    docker run --rm --cpus "$CPUS" \
        -v "$(pwd):/workspace" \
        -w /workspace/firmware/ymir/firmware/ymir \
        "$FW_IMAGE" \
        bash -c "cmake --fresh --preset Debug && cmake --build --preset Debug -j ${CPUS}"
    ;;

flash-bootloader)
    docker build -t "$FW_IMAGE" docker/firmware
    docker run --rm --cpus "$CPUS" \
        -v "$(pwd):/workspace" \
        -w /workspace/firmware/ymir/firmware/ymir \
        "$FW_IMAGE" \
        bash -c "cmake --fresh --preset Debug && cmake --build --preset Debug -j ${CPUS}"
    docker run --rm \
        -v "$(pwd):/workspace" \
        --privileged -v /dev/bus/usb:/dev/bus/usb \
        "$FW_IMAGE" \
        openocd \
            -f interface/stlink.cfg \
            -f target/stm32f4x.cfg \
            -c 'init' \
            -c 'reset halt' \
            -c 'stm32f4x mass_erase 0' \
            -c 'program /workspace/firmware/ymir/firmware/build/Debug/ymir.elf verify' \
            -c 'reset run' \
            -c 'exit'
    ;;

flash)
    docker build -t "$FW_IMAGE" docker/firmware
    docker run --rm --cpus "$CPUS" \
        -v "$(pwd):/workspace" \
        -w /workspace/firmware \
        "$FW_IMAGE" \
        bash -c "cmake --fresh --preset slot-a && cmake --build build/slot-a -j ${CPUS}"
    # flash_tool talks to /dev/ttyACM* and is just bash + python3 stdlib; running
    # it on the host avoids container serial-tty quirks (RESUME_QUERY timeouts).
    # --pid 0x4B56 matches kvasir's USBD_PID_FS override
    # (firmware/cubemx/USB_DEVICE/App/usbd_desc.c USER CODE BEGIN PRIVATE_DEFINES).
    SERIAL_ARGS=()
    if [[ -n "${2:-}" ]]; then
        SERIAL_ARGS=(--serial "$2")
    fi
    bash firmware/ymir/scripts/flash_tool.sh --pid 0x4B56 "${SERIAL_ARGS[@]}" \
        update firmware/build/slot-a/sensor-capture.elf
    ;;

test)
    docker build -t "$TEST_IMAGE" docker/tests
    docker run --rm --cpus "$CPUS" \
        -v "$(pwd):/workspace" \
        "$TEST_IMAGE" \
        bash -c "cmake --fresh -B tests/unit/build -S tests/unit -G Ninja \
                 && cmake --build tests/unit/build -j ${CPUS} \
                 && ctest --test-dir tests/unit/build --output-on-failure"
    ;;

host)
    docker build -t "$TEST_IMAGE" docker/tests
    docker run --rm --cpus "$CPUS" \
        -v "$(pwd):/workspace" \
        "$TEST_IMAGE" \
        bash -c "cmake --fresh -B host/build -S host -G Ninja \
                 && cmake --build host/build -j ${CPUS}"
    ;;

functest)
    docker build -t "$TEST_IMAGE" docker/tests
    docker run --rm --cpus "$CPUS" \
        -v "$(pwd):/workspace" \
        "$TEST_IMAGE" \
        bash -c "cmake --fresh -B host/build -S host -G Ninja \
                 && cmake --build host/build -j ${CPUS} \
                 && pytest tests/capture -v"
    ;;

toolstest)
    docker build -t "$TEST_IMAGE" docker/tests
    docker run --rm --cpus "$CPUS" \
        -v "$(pwd):/workspace" \
        "$TEST_IMAGE" \
        bash -c "pytest tests/tools -v"
    ;;

*)
    echo "Unknown command: ${CMD}" >&2
    echo "Run ./build.sh help for usage." >&2
    exit 1
    ;;
esac
