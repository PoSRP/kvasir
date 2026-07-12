#include <cstdint>

extern "C" int  ymir_current_slot(void) { return 0; }
extern "C" void ymir_confirm_boot(void) {}
extern "C" void ymir_request_rollback(void) {}
extern "C" void ymir_enter_update(void) {}
extern "C" void ymir_feed_watchdog(void) {}
extern "C" int  ymir_is_enter_update_request(const uint8_t*) { return 0; }
