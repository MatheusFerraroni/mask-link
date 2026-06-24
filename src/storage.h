#pragma once

#include <stddef.h>

#include "esp_err.h"

#define STORAGE_WIFI_SSID_MAX_LEN 33
#define STORAGE_WIFI_PASSWORD_MAX_LEN 65
#define STORAGE_DNS_SERVER_MAX_LEN 16
#define STORAGE_DEFAULT_DNS_SERVER "8.8.8.8"

esp_err_t storage_init(void);
esp_err_t storage_load_wifi_credentials(char *ssid, size_t ssid_size, char *password, size_t password_size);
esp_err_t storage_save_wifi_credentials(const char *ssid, const char *password);
esp_err_t storage_forget_wifi_credentials(void);
esp_err_t storage_load_dns_server(char *dns_server, size_t dns_server_size);
esp_err_t storage_save_dns_server(const char *dns_server);
esp_err_t storage_reset_dns_server(void);
