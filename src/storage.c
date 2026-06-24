#include "storage.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "storage";
static const char *NVS_NAMESPACE = "wifi_cfg";
static const char *KEY_SSID = "ssid";
static const char *KEY_PASSWORD = "password";
static const char *KEY_DNS_SERVER = "dns";

esp_err_t storage_init(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "NVS precisa ser reinicializada: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NVS inicializada");
    } else {
        ESP_LOGE(TAG, "Falha ao inicializar NVS: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t storage_load_wifi_credentials(char *ssid, size_t ssid_size, char *password, size_t password_size)
{
    if (ssid == NULL || password == NULL || ssid_size == 0 || password_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ssid[0] = '\0';
    password[0] = '\0';

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Credenciais Wi-Fi ainda nao foram salvas: %s", esp_err_to_name(err));
        return err;
    }

    size_t ssid_len = ssid_size;
    err = nvs_get_str(handle, KEY_SSID, ssid, &ssid_len);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "SSID salvo nao encontrado: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    size_t password_len = password_size;
    err = nvs_get_str(handle, KEY_PASSWORD, password, &password_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        password[0] = '\0';
        err = ESP_OK;
    }

    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Credenciais carregadas da NVS para SSID '%s'", ssid);
    } else {
        ESP_LOGE(TAG, "Falha ao carregar senha Wi-Fi: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t storage_save_wifi_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) >= STORAGE_WIFI_SSID_MAX_LEN) {
        ESP_LOGE(TAG, "SSID invalido para salvar na NVS");
        return ESP_ERR_INVALID_ARG;
    }

    if (password == NULL || strlen(password) >= STORAGE_WIFI_PASSWORD_MAX_LEN) {
        ESP_LOGE(TAG, "Senha invalida para salvar na NVS");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao abrir NVS para escrita: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_PASSWORD, password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Credenciais Wi-Fi salvas para SSID '%s'", ssid);
    } else {
        ESP_LOGE(TAG, "Falha ao salvar credenciais Wi-Fi: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t storage_load_dns_server(char *dns_server, size_t dns_server_size)
{
    if (dns_server == NULL || dns_server_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(dns_server, STORAGE_DEFAULT_DNS_SERVER, dns_server_size);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "DNS nao salvo; usando padrao %s", STORAGE_DEFAULT_DNS_SERVER);
        return ESP_OK;
    }

    size_t dns_len = dns_server_size;
    err = nvs_get_str(handle, KEY_DNS_SERVER, dns_server, &dns_len);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        strlcpy(dns_server, STORAGE_DEFAULT_DNS_SERVER, dns_server_size);
        ESP_LOGI(TAG, "DNS nao encontrado na NVS; usando padrao %s", STORAGE_DEFAULT_DNS_SERVER);
        return ESP_OK;
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "DNS carregado da NVS: %s", dns_server);
    } else {
        strlcpy(dns_server, STORAGE_DEFAULT_DNS_SERVER, dns_server_size);
        ESP_LOGE(TAG, "Falha ao carregar DNS; usando padrao %s: %s",
                 STORAGE_DEFAULT_DNS_SERVER,
                 esp_err_to_name(err));
    }

    return err == ESP_OK ? ESP_OK : err;
}

esp_err_t storage_save_dns_server(const char *dns_server)
{
    if (dns_server == NULL || dns_server[0] == '\0' || strlen(dns_server) >= STORAGE_DNS_SERVER_MAX_LEN) {
        ESP_LOGE(TAG, "DNS invalido para salvar na NVS");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao abrir NVS para salvar DNS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, KEY_DNS_SERVER, dns_server);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "DNS salvo na NVS: %s", dns_server);
    } else {
        ESP_LOGE(TAG, "Falha ao salvar DNS na NVS: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t storage_reset_dns_server(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "DNS ja esta no padrao; namespace ainda nao existe");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao abrir NVS para resetar DNS: %s", esp_err_to_name(err));
        return err;
    }

    esp_err_t erase_err = nvs_erase_key(handle, KEY_DNS_SERVER);
    err = nvs_commit(handle);
    nvs_close(handle);

    if ((erase_err == ESP_OK || erase_err == ESP_ERR_NVS_NOT_FOUND) && err == ESP_OK) {
        ESP_LOGI(TAG, "DNS resetado para o padrao %s", STORAGE_DEFAULT_DNS_SERVER);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Falha ao resetar DNS: erase=%s commit=%s",
             esp_err_to_name(erase_err),
             esp_err_to_name(err));
    return err != ESP_OK ? err : ESP_FAIL;
}

esp_err_t storage_forget_wifi_credentials(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Nenhuma credencial Wi-Fi para apagar");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao abrir NVS para apagar credenciais: %s", esp_err_to_name(err));
        return err;
    }

    esp_err_t ssid_err = nvs_erase_key(handle, KEY_SSID);
    esp_err_t password_err = nvs_erase_key(handle, KEY_PASSWORD);
    err = nvs_commit(handle);
    nvs_close(handle);

    if ((ssid_err == ESP_OK || ssid_err == ESP_ERR_NVS_NOT_FOUND) &&
        (password_err == ESP_OK || password_err == ESP_ERR_NVS_NOT_FOUND) &&
        err == ESP_OK) {
        ESP_LOGI(TAG, "Credenciais Wi-Fi apagadas da NVS");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Falha ao apagar credenciais Wi-Fi: ssid=%s password=%s commit=%s",
             esp_err_to_name(ssid_err),
             esp_err_to_name(password_err),
             esp_err_to_name(err));
    return err != ESP_OK ? err : ESP_FAIL;
}
