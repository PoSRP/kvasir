#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void app_main(void);
void app_init(void);
void app_reset(void);
void app_process(void);
void app_adc_push(uint16_t sample);
void app_adc_push_ch(uint8_t ch_id, uint16_t sample);
void app_usb_rx(const uint8_t* buf, uint32_t len);
void app_on_connect(void);
void app_on_disconnect(void);
void app_on_usb_init(void);

#ifdef __cplusplus
}
#endif
