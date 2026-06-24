#include "connectivity.h"

#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "event_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "ping/ping_sock.h"
#include "status_led.h"
#include "wifi_manager.h"

static const char *TAG = "connectivity";
static bool s_internet_ok;
static bool s_has_last_state;

#define PING_DONE_BIT BIT0
#define PING_OK_BIT BIT1

static void ping_success(esp_ping_handle_t hdl, void *args)
{
    (void)hdl;
    xEventGroupSetBits((EventGroupHandle_t)args, PING_OK_BIT);
}

static void ping_end(esp_ping_handle_t hdl, void *args)
{
    (void)hdl;
    xEventGroupSetBits((EventGroupHandle_t)args, PING_DONE_BIT);
}

static bool ping_8_8_8_8(void)
{
    EventGroupHandle_t group = xEventGroupCreate();
    if (group == NULL) {
        return false;
    }

    ip_addr_t target;
    ipaddr_aton("8.8.8.8", &target);

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.target_addr = target;
    config.count = 1;
    config.timeout_ms = 1500;
    config.interval_ms = 100;
    config.task_stack_size = 3072;

    esp_ping_callbacks_t callbacks = {
        .cb_args = group,
        .on_ping_success = ping_success,
        .on_ping_timeout = NULL,
        .on_ping_end = ping_end,
    };

    esp_ping_handle_t ping;
    esp_err_t err = esp_ping_new_session(&config, &callbacks, &ping);
    if (err != ESP_OK) {
        vEventGroupDelete(group);
        ESP_LOGE(TAG, "Falha ao criar sessao ping: %s", esp_err_to_name(err));
        return false;
    }

    esp_ping_start(ping);
    EventBits_t bits = xEventGroupWaitBits(group,
                                           PING_DONE_BIT,
                                           pdTRUE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(3500));
    bool ok = (bits & PING_DONE_BIT) && (xEventGroupGetBits(group) & PING_OK_BIT);
    esp_ping_delete_session(ping);
    vEventGroupDelete(group);
    return ok;
}

static bool resolve_example(void)
{
    struct addrinfo hints = { 0 };
    struct addrinfo *res = NULL;
    hints.ai_family = AF_INET;
    int ret = getaddrinfo("example.com", NULL, &hints, &res);
    if (res != NULL) {
        freeaddrinfo(res);
    }
    return ret == 0;
}

static bool http_example(void)
{
    esp_http_client_config_t config = {
        .url = "http://example.com",
        .timeout_ms = 3000,
        .method = HTTP_METHOD_GET,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return false;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err == ESP_OK && status > 0 && status < 500;
}

bool connectivity_is_internet_ok(void)
{
    return s_internet_ok;
}

void connectivity_set_internet_ok(bool ok)
{
    s_internet_ok = ok;
}

esp_err_t connectivity_run_manual_test(connectivity_test_result_t *result)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    wifi_manager_status_t status;
    wifi_manager_get_status(&status);
    result->sta_connected = status.sta_state == WIFI_MANAGER_STA_CONNECTED;
    if (!result->sta_connected) {
        strlcpy(result->message, "STA ainda nao esta conectada", sizeof(result->message));
        return ESP_OK;
    }

    result->ping_ok = ping_8_8_8_8();
    if (!result->ping_ok) {
        strlcpy(result->message, "Falhou no ping para 8.8.8.8", sizeof(result->message));
        return ESP_OK;
    }

    result->dns_ok = resolve_example();
    if (!result->dns_ok) {
        strlcpy(result->message, "Falhou na resolucao DNS de example.com", sizeof(result->message));
        return ESP_OK;
    }

    result->http_ok = http_example();
    if (!result->http_ok) {
        strlcpy(result->message, "Falhou no HTTP GET para example.com", sizeof(result->message));
        return ESP_OK;
    }

    result->internet_ok = true;
    strlcpy(result->message, "Internet OK", sizeof(result->message));
    return ESP_OK;
}

static void connectivity_task(void *arg)
{
    (void)arg;
    while (true) {
        wifi_manager_status_t status;
        wifi_manager_get_status(&status);
        bool ok = false;

        if (status.sta_state == WIFI_MANAGER_STA_CONNECTED) {
            ok = ping_8_8_8_8();
        }

        if (!s_has_last_state || ok != s_internet_ok) {
            s_has_last_state = true;
            s_internet_ok = ok;
            event_log_add("Internet %s", ok ? "OK" : "indisponivel");
        }

        if (status.sta_state == WIFI_MANAGER_STA_IDLE) {
            status_led_set_state(STATUS_LED_READY);
        } else if (status.sta_state == WIFI_MANAGER_STA_CONNECTING ||
                   status.sta_state == WIFI_MANAGER_STA_DISCONNECTED) {
            status_led_set_state(STATUS_LED_CONNECTING);
        } else if (ok && status.napt_enabled) {
            status_led_set_state(STATUS_LED_NORMAL);
        } else {
            status_led_set_state(STATUS_LED_NO_INTERNET);
        }

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void connectivity_start(void)
{
    xTaskCreate(connectivity_task, "connectivity", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "Monitor de conectividade iniciado");
}
