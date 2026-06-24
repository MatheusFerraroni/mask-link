#include "app_config.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "app_config";
static const char *NVS_NAMESPACE = "wifi_cfg";
static const char *KEY_AP_SSID = "ap_ssid";
static const char *KEY_AP_MAX = "ap_max";
static const char *KEY_AP_IP = "ap_ip";
static const char *KEY_DHCP_START = "dhcp_start";
static const char *KEY_DHCP_END = "dhcp_end";

static bool parse_ipv4(const char *value, uint32_t *out)
{
    unsigned a, b, c, d;
    char tail;

    if (value == NULL || sscanf(value, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) {
        return false;
    }
    if (a > 255 || b > 255 || c > 255 || d > 255) {
        return false;
    }

    *out = (a << 24) | (b << 16) | (c << 8) | d;
    return true;
}

void app_config_defaults(app_config_t *config)
{
    if (config == NULL) {
        return;
    }

    strlcpy(config->ap_ssid, APP_CONFIG_DEFAULT_AP_SSID, sizeof(config->ap_ssid));
    config->ap_max_clients = APP_CONFIG_DEFAULT_MAX_CLIENTS;
    strlcpy(config->ap_ip, APP_CONFIG_DEFAULT_AP_IP, sizeof(config->ap_ip));
    strlcpy(config->dhcp_start, APP_CONFIG_DEFAULT_DHCP_START, sizeof(config->dhcp_start));
    strlcpy(config->dhcp_end, APP_CONFIG_DEFAULT_DHCP_END, sizeof(config->dhcp_end));
}

static void load_string(nvs_handle_t handle, const char *key, char *value, size_t value_size)
{
    size_t len = value_size;
    esp_err_t err = nvs_get_str(handle, key, value, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Falha ao carregar %s: %s", key, esp_err_to_name(err));
    }
}

esp_err_t app_config_load(app_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    app_config_defaults(config);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Config AP nao encontrada; usando padroes");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao abrir NVS para config AP: %s", esp_err_to_name(err));
        return err;
    }

    load_string(handle, KEY_AP_SSID, config->ap_ssid, sizeof(config->ap_ssid));
    load_string(handle, KEY_AP_IP, config->ap_ip, sizeof(config->ap_ip));
    load_string(handle, KEY_DHCP_START, config->dhcp_start, sizeof(config->dhcp_start));
    load_string(handle, KEY_DHCP_END, config->dhcp_end, sizeof(config->dhcp_end));

    uint8_t max_clients = APP_CONFIG_DEFAULT_MAX_CLIENTS;
    err = nvs_get_u8(handle, KEY_AP_MAX, &max_clients);
    if (err == ESP_OK) {
        config->ap_max_clients = max_clients;
    }
    nvs_close(handle);

    err = app_config_validate_ap(config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Config AP salva invalida; usando padroes");
        app_config_defaults(config);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Config AP: ssid='%s' max=%u ip=%s dhcp=%s-%s",
             config->ap_ssid,
             config->ap_max_clients,
             config->ap_ip,
             config->dhcp_start,
             config->dhcp_end);
    return ESP_OK;
}

esp_err_t app_config_validate_ap(const app_config_t *config)
{
    uint32_t ap_ip, dhcp_start, dhcp_end;

    if (config == NULL || config->ap_ssid[0] == '\0' ||
        strlen(config->ap_ssid) >= APP_CONFIG_AP_SSID_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->ap_max_clients < APP_CONFIG_MIN_MAX_CLIENTS ||
        config->ap_max_clients > APP_CONFIG_MAX_MAX_CLIENTS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!parse_ipv4(config->ap_ip, &ap_ip) ||
        !parse_ipv4(config->dhcp_start, &dhcp_start) ||
        !parse_ipv4(config->dhcp_end, &dhcp_end)) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((ap_ip & 0xFFFFFF00) != (dhcp_start & 0xFFFFFF00) ||
        (ap_ip & 0xFFFFFF00) != (dhcp_end & 0xFFFFFF00)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (dhcp_start > dhcp_end || (ap_ip >= dhcp_start && ap_ip <= dhcp_end)) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t app_config_save_ap(const app_config_t *config)
{
    esp_err_t err = app_config_validate_ap(config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Config AP rejeitada");
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao abrir NVS para salvar AP: %s", esp_err_to_name(err));
        return err;
    }

    if ((err = nvs_set_str(handle, KEY_AP_SSID, config->ap_ssid)) == ESP_OK &&
        (err = nvs_set_u8(handle, KEY_AP_MAX, config->ap_max_clients)) == ESP_OK &&
        (err = nvs_set_str(handle, KEY_AP_IP, config->ap_ip)) == ESP_OK &&
        (err = nvs_set_str(handle, KEY_DHCP_START, config->dhcp_start)) == ESP_OK &&
        (err = nvs_set_str(handle, KEY_DHCP_END, config->dhcp_end)) == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Config AP salva; reboot necessario para aplicar");
    } else {
        ESP_LOGE(TAG, "Falha ao salvar config AP: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t app_config_reset_all(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Nada para resetar na NVS");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao abrir NVS para reset geral: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    ESP_LOGI(TAG, "Reset geral de configuracoes: %s", esp_err_to_name(err));
    return err;
}
