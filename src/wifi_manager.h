#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_config.h"
#include "esp_err.h"
#include "storage.h"

#define WIFI_MANAGER_AP_CHANNEL 1
#define WIFI_MANAGER_SCAN_MAX_RESULTS 20
#define WIFI_MANAGER_AP_CLIENT_LIMIT APP_CONFIG_MAX_MAX_CLIENTS

typedef enum {
    WIFI_MANAGER_STA_IDLE = 0,
    WIFI_MANAGER_STA_CONNECTING,
    WIFI_MANAGER_STA_CONNECTED,
    WIFI_MANAGER_STA_DISCONNECTED,
} wifi_manager_sta_state_t;

typedef struct {
    wifi_manager_sta_state_t sta_state;
    bool napt_enabled;
    bool sta_configured;
    char ssid[STORAGE_WIFI_SSID_MAX_LEN];
    char ip[16];
    char gateway[16];
    char sta_dns[16];
    char dns_server[STORAGE_DNS_SERVER_MAX_LEN];
    int rssi;
    app_config_t ap_config;
} wifi_manager_status_t;

typedef struct {
    char mac[18];
    char ip[16];
} wifi_manager_ap_client_t;

typedef struct {
    char ssid[33];
    int rssi;
    uint8_t channel;
    char authmode[24];
} wifi_manager_scan_result_t;

esp_err_t wifi_manager_start(void);
esp_err_t wifi_manager_connect_to(const char *ssid, const char *password);
esp_err_t wifi_manager_forget_sta(void);
esp_err_t wifi_manager_set_dns_server(const char *dns_server);
esp_err_t wifi_manager_reset_dns_server(void);
void wifi_manager_get_status(wifi_manager_status_t *status);
size_t wifi_manager_get_ap_clients(wifi_manager_ap_client_t *clients, size_t max_clients);
esp_err_t wifi_manager_get_ap_config(app_config_t *config);
esp_err_t wifi_manager_scan_networks(wifi_manager_scan_result_t *results, uint16_t max_results, uint16_t *count);
const char *wifi_manager_state_to_text(wifi_manager_sta_state_t state);
