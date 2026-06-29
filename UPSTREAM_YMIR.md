# Upstream ymir wishlist

Features kvasir would like to see in the ymir bootloader project
(`firmware/ymir/`), so kvasir (and other slot apps) don't need local
workarounds.

## Pending

(none)

## Merged

- **`ymir::feed_watchdog()` API** — added in upstream ymir
  (commit `8bd2e86` series, PR #1). Replaces the direct `IWDG_KR` write
  workaround that used to live in `firmware/app/hal/wdg_hal.cpp`.
- **`flash_tool.sh --serial SERIAL` filter** — added in upstream ymir
  (PR #2). `./build.sh flash <SERIAL>` now forwards it through so the
  testbench can target a specific board when multiple kvasir devices are
  attached to the same host.
