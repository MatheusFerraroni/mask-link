#include "connectivity.h"
#include "event_log.h"
#include "esp_log.h"
#include "status_led.h"
#include "storage.h"
#include "web_config.h"
#include "wifi_manager.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "Iniciando roteador Wi-Fi NAT ESP32");

    status_led_start();
    event_log_add("Boot do ESP32 iniciado");
    ESP_ERROR_CHECK(storage_init());
    ESP_ERROR_CHECK(wifi_manager_start());
    ESP_ERROR_CHECK(web_config_start());
    connectivity_start();

    wifi_manager_status_t status;
    wifi_manager_get_status(&status);
    ESP_LOGI(TAG,
             "Sistema pronto: conecte no AP '%s' e acesse http://%s",
             status.ap_config.ap_ssid,
             status.ap_config.ap_ip);
    event_log_add("Servidor web pronto em http://%s", status.ap_config.ap_ip);
}
