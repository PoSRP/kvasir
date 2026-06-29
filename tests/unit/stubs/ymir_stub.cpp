// Host-side stubs for the ymir bootloader API. Linked into unit tests so app
// code that calls ymir::confirm_boot / ymir::enter_update / etc. links and
// behaves harmlessly off-target (no flash writes, no MCU reset).
#include <stdint.h>

extern "C" int  ymir_current_slot(void) { return 0; }
extern "C" void ymir_confirm_boot(void) {}
extern "C" void ymir_request_rollback(void) {}
extern "C" void ymir_enter_update(void) {}
extern "C" void ymir_feed_watchdog(void) {}
extern "C" int  ymir_is_enter_update_request(const uint8_t*) { return 0; }
