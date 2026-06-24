#pragma once

typedef enum {
    STATUS_LED_READY = 0,
    STATUS_LED_CONNECTING,
    STATUS_LED_NORMAL,
    STATUS_LED_NO_INTERNET,
    STATUS_LED_SAVING,
    STATUS_LED_FACTORY_RESET,
    STATUS_LED_ERROR,
} status_led_state_t;

void status_led_start(void);
void status_led_set_state(status_led_state_t state);
void status_led_client_connected_event(void);
