#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "storage.h"

#define APP_CONFIG_DEFAULT_AP_SSID "ESP32-Config"
#define APP_CONFIG_AP_PASSWORD "12345678"
#define APP_CONFIG_DEFAULT_AP_IP "192.168.4.1"
#define APP_CONFIG_DEFAULT_DHCP_START "192.168.4.2"
#define APP_CONFIG_DEFAULT_DHCP_END "192.168.4.50"
#define APP_CONFIG_DEFAULT_MAX_CLIENTS 4
#define APP_CONFIG_MIN_MAX_CLIENTS 1
#define APP_CONFIG_MAX_MAX_CLIENTS 10
#define APP_CONFIG_AP_SSID_MAX_LEN 33
#define APP_CONFIG_IPV4_STR_MAX_LEN 16

typedef struct {
    char ap_ssid[APP_CONFIG_AP_SSID_MAX_LEN];
    uint8_t ap_max_clients;
    char ap_ip[APP_CONFIG_IPV4_STR_MAX_LEN];
    char dhcp_start[APP_CONFIG_IPV4_STR_MAX_LEN];
    char dhcp_end[APP_CONFIG_IPV4_STR_MAX_LEN];
} app_config_t;

void app_config_defaults(app_config_t *config);
esp_err_t app_config_load(app_config_t *config);
esp_err_t app_config_save_ap(const app_config_t *config);
esp_err_t app_config_reset_all(void);
esp_err_t app_config_validate_ap(const app_config_t *config);
