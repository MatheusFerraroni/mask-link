#include "status_led.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define STATUS_LED_GPIO GPIO_NUM_2

static const char *TAG = "status_led";

static volatile status_led_state_t s_state = STATUS_LED_READY;
static volatile bool s_client_event;

static void led_set(bool on)
{
    gpio_set_level(STATUS_LED_GPIO, on ? 1 : 0);
}

static void blink(int on_ms, int off_ms)
{
    led_set(true);
    vTaskDelay(pdMS_TO_TICKS(on_ms));
    led_set(false);
    vTaskDelay(pdMS_TO_TICKS(off_ms));
}

static void client_connected_pattern(void)
{
    s_client_event = false;
    led_set(true);
    vTaskDelay(pdMS_TO_TICKS(1000));
    for (int i = 0; i < 5; i++) {
        blink(120, 120);
    }
}

static void led_task(void *arg)
{
    (void)arg;

    while (true) {
        if (s_client_event) {
            client_connected_pattern();
            continue;
        }

        switch (s_state) {
        case STATUS_LED_READY:
            blink(2000, 300);
            break;
        case STATUS_LED_CONNECTING:
            blink(500, 500);
            break;
        case STATUS_LED_NORMAL:
            led_set(true);
            vTaskDelay(pdMS_TO_TICKS(500));
            break;
        case STATUS_LED_NO_INTERNET:
            for (int i = 0; i < 2; i++) {
                blink(120, 120);
            }
            led_set(false);
            vTaskDelay(pdMS_TO_TICKS(2000));
            break;
        case STATUS_LED_SAVING:
            blink(100, 100);
            break;
        case STATUS_LED_FACTORY_RESET:
            for (int i = 0; i < 15; i++) {
                blink(100, 100);
            }
            for (int i = 0; i < 2; i++) {
                blink(700, 300);
            }
            s_state = STATUS_LED_READY;
            break;
        case STATUS_LED_ERROR:
        default:
            for (int i = 0; i < 3; i++) {
                blink(120, 120);
            }
            led_set(false);
            vTaskDelay(pdMS_TO_TICKS(2000));
            break;
        }
    }
}

void status_led_start(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    led_set(false);
    xTaskCreate(led_task, "status_led", 2048, NULL, 4, NULL);
    ESP_LOGI(TAG, "LED de estado iniciado no GPIO %d", STATUS_LED_GPIO);
}

void status_led_set_state(status_led_state_t state)
{
    s_state = state;
}

void status_led_client_connected_event(void)
{
    s_client_event = true;
}
