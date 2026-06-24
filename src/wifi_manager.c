#include "wifi_manager.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "apps/dhcpserver/dhcpserver.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "esp_wifi_ap_get_sta_list.h"
#include "event_log.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "status_led.h"
#include "storage.h"

static const char *TAG = "wifi_manager";

static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static app_config_t s_app_config;
static char s_captive_portal_url[40] = "http://192.168.4.1/";
static bool s_sta_configured;
static bool s_napt_enabled;
static wifi_manager_sta_state_t s_sta_state = WIFI_MANAGER_STA_IDLE;
static char s_sta_ssid[STORAGE_WIFI_SSID_MAX_LEN];
static char s_sta_ip[16];
static char s_sta_gateway[16];
static char s_sta_dns[16];
static char s_dns_server[STORAGE_DNS_SERVER_MAX_LEN] = STORAGE_DEFAULT_DNS_SERVER;
static int s_last_ap_client_count = -1;

static const char *authmode_to_text(wifi_auth_mode_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN:
        return "OPEN";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return "WPA2-ENT";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2/WPA3";
    default:
        return "UNKNOWN";
    }
}

static esp_err_t apply_ap_dns_server(const char *dns_server, bool restart_dhcp)
{
    esp_netif_dns_info_t dns_info = { 0 };
    dhcps_offer_t dns_offer = OFFER_DNS;
    esp_err_t err = esp_netif_str_to_ip4(dns_server, &dns_info.ip.u_addr.ip4);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DNS invalido '%s': %s", dns_server, esp_err_to_name(err));
        event_log_add("DNS invalido recebido: %s", dns_server);
        return err;
    }

    dns_info.ip.type = ESP_IPADDR_TYPE_V4;

    if (restart_dhcp) {
        err = esp_netif_dhcps_stop(s_ap_netif);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
            ESP_LOGE(TAG, "Falha ao parar DHCP antes de aplicar DNS %s: %s",
                     dns_server,
                     esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "DHCP do AP parado para atualizar DNS");
    }

    err = esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns_info);
    if (err == ESP_OK) {
        err = esp_netif_dhcps_option(s_ap_netif,
                                     ESP_NETIF_OP_SET,
                                     ESP_NETIF_DOMAIN_NAME_SERVER,
                                     &dns_offer,
                                     sizeof(dns_offer));
    }

    if (restart_dhcp) {
        esp_err_t start_err = esp_netif_dhcps_start(s_ap_netif);
        if (start_err == ESP_OK) {
            ESP_LOGI(TAG, "DHCP do AP reiniciado apos atualizar DNS");
        } else {
            ESP_LOGE(TAG, "Falha ao reiniciar DHCP do AP: %s", esp_err_to_name(start_err));
            return start_err;
        }
    }

    if (err == ESP_OK) {
        strlcpy(s_dns_server, dns_server, sizeof(s_dns_server));
        ESP_LOGI(TAG, "DNS DHCP do SoftAP configurado para %s", s_dns_server);
        event_log_add("DNS do AP configurado para %s", s_dns_server);
    } else {
        ESP_LOGE(TAG, "Falha ao aplicar DNS DHCP %s: %s", dns_server, esp_err_to_name(err));
        event_log_add("Falha ao aplicar DNS %s", dns_server);
    }

    return err;
}

static void disable_napt(void)
{
    if (!s_napt_enabled || s_ap_netif == NULL) {
        return;
    }

    esp_err_t err = esp_netif_napt_disable(s_ap_netif);
    if (err == ESP_OK) {
        s_napt_enabled = false;
        ESP_LOGI(TAG, "NAPT desabilitado na interface AP");
        event_log_add("NAT desabilitado");
    } else {
        ESP_LOGE(TAG, "Falha ao desabilitar NAPT: %s", esp_err_to_name(err));
        event_log_add("Falha ao desabilitar NAT");
    }
}

static void enable_napt(void)
{
    if (s_ap_netif == NULL || s_sta_netif == NULL) {
        ESP_LOGE(TAG, "Interfaces AP/STA nao estao prontas para NAPT");
        event_log_add("Interfaces AP/STA nao prontas para NAT");
        return;
    }

    esp_err_t err = esp_netif_set_default_netif(s_sta_netif);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao definir STA como interface default: %s", esp_err_to_name(err));
        event_log_add("Falha ao definir STA como rota default");
        return;
    }

    if (s_napt_enabled) {
        ESP_LOGI(TAG, "NAPT ja esta ativo na interface AP");
        return;
    }

    err = esp_netif_napt_enable(s_ap_netif);
    if (err == ESP_OK) {
        s_napt_enabled = true;
        ESP_LOGI(TAG, "NAPT habilitado na interface AP");
        event_log_add("NAT habilitado na interface AP");
    } else {
        ESP_LOGE(TAG, "Falha ao habilitar NAPT na interface AP: %s", esp_err_to_name(err));
        event_log_add("Falha ao habilitar NAT");
    }
}

static esp_err_t configure_ap_ip(void)
{
    esp_netif_ip_info_t ip_info = { 0 };
    dhcps_lease_t lease = { 0 };

    esp_err_t err = esp_netif_dhcps_stop(s_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "Falha ao parar DHCP do AP: %s", esp_err_to_name(err));
        return err;
    }

    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(s_app_config.ap_ip, &ip_info.ip), TAG, "AP IP invalido");
    ip_info.gw = ip_info.ip;
    ip_info.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_ap_netif, &ip_info), TAG, "Falha ao aplicar IP do AP");

    lease.enable = true;
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(s_app_config.dhcp_start, (esp_ip4_addr_t *)&lease.start_ip),
                        TAG,
                        "DHCP start invalido");
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(s_app_config.dhcp_end, (esp_ip4_addr_t *)&lease.end_ip),
                        TAG,
                        "DHCP end invalido");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_option(s_ap_netif,
                                               ESP_NETIF_OP_SET,
                                               ESP_NETIF_REQUESTED_IP_ADDRESS,
                                               &lease,
                                               sizeof(lease)),
                        TAG,
                        "Falha ao configurar range DHCP");

    ESP_RETURN_ON_ERROR(apply_ap_dns_server(s_dns_server, false), TAG, "Falha ao aplicar DNS do AP");

    snprintf(s_captive_portal_url, sizeof(s_captive_portal_url), "http://%s/", s_app_config.ap_ip);
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_option(s_ap_netif,
                                               ESP_NETIF_OP_SET,
                                               ESP_NETIF_CAPTIVEPORTAL_URI,
                                               (void *)s_captive_portal_url,
                                               strlen(s_captive_portal_url)),
                        TAG,
                        "Falha ao configurar captive portal");

    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(s_ap_netif), TAG, "Falha ao iniciar DHCP do AP");

    ESP_LOGI(TAG,
             "SoftAP configurado em %s, DHCP %s-%s, DNS %s",
             s_app_config.ap_ip,
             s_app_config.dhcp_start,
             s_app_config.dhcp_end,
             s_dns_server);
    event_log_add("AP em %s com DHCP %s-%s", s_app_config.ap_ip, s_app_config.dhcp_start, s_app_config.dhcp_end);
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;

    switch (event_id) {
    case WIFI_EVENT_AP_START:
        ESP_LOGI(TAG, "SoftAP iniciado: SSID '%s'", s_app_config.ap_ssid);
        event_log_add("AP iniciado: %s", s_app_config.ap_ssid);
        break;
    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "Cliente conectado ao AP: " MACSTR, MAC2STR(event->mac));
        event_log_add("Cliente entrou no AP: " MACSTR, MAC2STR(event->mac));
        status_led_client_connected_event();
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "Cliente saiu do AP: " MACSTR, MAC2STR(event->mac));
        event_log_add("Cliente saiu do AP: " MACSTR, MAC2STR(event->mac));
        break;
    }
    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "Interface STA iniciada");
        event_log_add("Interface STA iniciada");
        break;
    case WIFI_EVENT_STA_CONNECTED:
        s_sta_state = WIFI_MANAGER_STA_CONNECTING;
        ESP_LOGI(TAG, "STA associada ao AP externo; aguardando IP");
        event_log_add("STA associada; aguardando IP");
        status_led_set_state(STATUS_LED_CONNECTING);
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGE(TAG, "STA desconectada da rede externa, reason=%d", event->reason);
        event_log_add("STA desconectada, motivo %d", event->reason);
        disable_napt();
        s_sta_state = s_sta_configured ? WIFI_MANAGER_STA_DISCONNECTED : WIFI_MANAGER_STA_IDLE;
        s_sta_ip[0] = '\0';
        s_sta_gateway[0] = '\0';
        s_sta_dns[0] = '\0';
        status_led_set_state(s_sta_configured ? STATUS_LED_CONNECTING : STATUS_LED_READY);
        if (s_sta_configured) {
            esp_err_t err = esp_wifi_connect();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Tentando reconectar STA automaticamente");
                event_log_add("Reconexao STA iniciada");
            } else {
                ESP_LOGE(TAG, "Falha ao iniciar reconexao STA: %s", esp_err_to_name(err));
                event_log_add("Falha ao iniciar reconexao STA");
            }
        }
        break;
    }
    default:
        break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;

    if (event_id != IP_EVENT_STA_GOT_IP) {
        return;
    }

    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
    snprintf(s_sta_gateway, sizeof(s_sta_gateway), IPSTR, IP2STR(&event->ip_info.gw));
    s_sta_state = WIFI_MANAGER_STA_CONNECTED;
    ESP_LOGI(TAG, "STA recebeu IP: %s, gateway: %s", s_sta_ip, s_sta_gateway);
    event_log_add("STA recebeu IP %s", s_sta_ip);
    enable_napt();
}

static esp_err_t configure_softap(void)
{
    wifi_config_t ap_config = { 0 };

    strlcpy((char *)ap_config.ap.ssid, s_app_config.ap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(s_app_config.ap_ssid);
    strlcpy((char *)ap_config.ap.password, APP_CONFIG_AP_PASSWORD, sizeof(ap_config.ap.password));
    ap_config.ap.channel = WIFI_MANAGER_AP_CHANNEL;
    ap_config.ap.max_connection = s_app_config.ap_max_clients;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_config.ap.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), TAG, "Falha ao aplicar config SoftAP");
    ESP_LOGI(TAG,
             "SoftAP configurado: SSID '%s', max_clientes=%u",
             s_app_config.ap_ssid,
             s_app_config.ap_max_clients);
    return ESP_OK;
}

esp_err_t wifi_manager_connect_to(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) >= STORAGE_WIFI_SSID_MAX_LEN) {
        ESP_LOGE(TAG, "SSID invalido para conexao STA");
        event_log_add("SSID STA invalido recebido");
        return ESP_ERR_INVALID_ARG;
    }

    if (password == NULL || strlen(password) >= STORAGE_WIFI_PASSWORD_MAX_LEN) {
        ESP_LOGE(TAG, "Senha invalida para conexao STA");
        event_log_add("Senha STA invalida recebida");
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t sta_config = { 0 };
    strlcpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password));

    s_sta_configured = true;
    s_sta_state = WIFI_MANAGER_STA_CONNECTING;
    s_sta_ip[0] = '\0';
    s_sta_gateway[0] = '\0';
    s_sta_dns[0] = '\0';
    strlcpy(s_sta_ssid, ssid, sizeof(s_sta_ssid));
    disable_napt();
    status_led_set_state(STATUS_LED_CONNECTING);

    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGE(TAG, "Falha ao desconectar STA antes de reconfigurar: %s", esp_err_to_name(err));
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao aplicar configuracao STA: %s", esp_err_to_name(err));
        event_log_add("Falha ao aplicar config STA");
        return err;
    }

    ESP_LOGI(TAG, "Conectando STA ao SSID externo '%s'", ssid);
    event_log_add("Conectando STA em %s", ssid);
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar conexao STA: %s", esp_err_to_name(err));
        event_log_add("Falha ao iniciar conexao STA");
    }

    return err;
}

esp_err_t wifi_manager_set_dns_server(const char *dns_server)
{
    if (dns_server == NULL || dns_server[0] == '\0' || strlen(dns_server) >= STORAGE_DNS_SERVER_MAX_LEN) {
        ESP_LOGE(TAG, "DNS invalido recebido para configuracao");
        event_log_add("DNS invalido recebido na web");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Atualizando servidor DNS do SoftAP para %s", dns_server);
    return apply_ap_dns_server(dns_server, true);
}

esp_err_t wifi_manager_reset_dns_server(void)
{
    ESP_LOGI(TAG, "Resetando servidor DNS do SoftAP para o padrao %s", STORAGE_DEFAULT_DNS_SERVER);
    return apply_ap_dns_server(STORAGE_DEFAULT_DNS_SERVER, true);
}

esp_err_t wifi_manager_forget_sta(void)
{
    ESP_LOGI(TAG, "Esquecendo configuracao STA em RAM e desconectando da rede externa");
    event_log_add("Credenciais STA esquecidas");
    s_sta_configured = false;
    s_sta_state = WIFI_MANAGER_STA_IDLE;
    s_sta_ssid[0] = '\0';
    s_sta_ip[0] = '\0';
    s_sta_gateway[0] = '\0';
    s_sta_dns[0] = '\0';
    disable_napt();
    status_led_set_state(STATUS_LED_READY);

    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGE(TAG, "Falha ao desconectar STA ao esquecer credenciais: %s", esp_err_to_name(err));
        return err;
    }

    wifi_config_t sta_config = { 0 };
    err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao limpar configuracao STA: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Configuracao STA limpa; SoftAP permanece ativo");
    return ESP_OK;
}

void wifi_manager_get_status(wifi_manager_status_t *status)
{
    if (status == NULL) {
        return;
    }

    memset(status, 0, sizeof(*status));
    status->sta_state = s_sta_state;
    status->napt_enabled = s_napt_enabled;
    status->sta_configured = s_sta_configured;
    strlcpy(status->ssid, s_sta_ssid, sizeof(status->ssid));
    strlcpy(status->ip, s_sta_ip, sizeof(status->ip));
    strlcpy(status->gateway, s_sta_gateway, sizeof(status->gateway));
    strlcpy(status->dns_server, s_dns_server, sizeof(status->dns_server));
    status->ap_config = s_app_config;

    if (s_sta_netif != NULL && s_sta_state == WIFI_MANAGER_STA_CONNECTED) {
        esp_netif_dns_info_t dns_info = { 0 };
        if (esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK &&
            dns_info.ip.type == ESP_IPADDR_TYPE_V4) {
            snprintf(status->sta_dns, sizeof(status->sta_dns), IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
            strlcpy(s_sta_dns, status->sta_dns, sizeof(s_sta_dns));
        } else {
            strlcpy(status->sta_dns, s_sta_dns, sizeof(status->sta_dns));
        }

        wifi_ap_record_t ap_info = { 0 };
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            status->rssi = ap_info.rssi;
        }
    } else {
        strlcpy(status->sta_dns, s_sta_dns, sizeof(status->sta_dns));
    }
}

size_t wifi_manager_get_ap_clients(wifi_manager_ap_client_t *clients, size_t max_clients)
{
    if (clients == NULL || max_clients == 0) {
        return 0;
    }

    wifi_sta_list_t wifi_sta_list = { 0 };
    wifi_sta_mac_ip_list_t ip_mac_list = { 0 };
    esp_err_t err = esp_wifi_ap_get_sta_list(&wifi_sta_list);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao obter lista de clientes Wi-Fi: %s", esp_err_to_name(err));
        return 0;
    }

    err = esp_wifi_ap_get_sta_list_with_ip(&wifi_sta_list, &ip_mac_list);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao obter IPs dos clientes AP: %s", esp_err_to_name(err));
        return 0;
    }

    size_t count = ip_mac_list.num < (int)max_clients ? ip_mac_list.num : max_clients;
    for (size_t i = 0; i < count; i++) {
        snprintf(clients[i].mac, sizeof(clients[i].mac), MACSTR, MAC2STR(ip_mac_list.sta[i].mac));
        snprintf(clients[i].ip, sizeof(clients[i].ip), IPSTR, IP2STR(&ip_mac_list.sta[i].ip));
    }

    if (s_last_ap_client_count != ip_mac_list.num) {
        s_last_ap_client_count = ip_mac_list.num;
        ESP_LOGI(TAG, "Clientes conectados ao SoftAP: %d", ip_mac_list.num);
        event_log_add("Clientes conectados ao AP: %d", ip_mac_list.num);
    }

    return count;
}

esp_err_t wifi_manager_get_ap_config(app_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *config = s_app_config;
    return ESP_OK;
}

esp_err_t wifi_manager_scan_networks(wifi_manager_scan_result_t *results, uint16_t max_results, uint16_t *count)
{
    if (results == NULL || count == NULL || max_results == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *count = 0;
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
    };

    ESP_LOGI(TAG, "Iniciando scan Wi-Fi sob demanda");
    event_log_add("Scan Wi-Fi iniciado");
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha no scan Wi-Fi: %s", esp_err_to_name(err));
        event_log_add("Falha no scan Wi-Fi");
        return err;
    }

    uint16_t ap_count = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&ap_count), TAG, "Falha ao obter quantidade de redes");
    if (ap_count > max_results) {
        ap_count = max_results;
    }

    wifi_ap_record_t records[WIFI_MANAGER_SCAN_MAX_RESULTS] = { 0 };
    uint16_t records_to_read = ap_count < WIFI_MANAGER_SCAN_MAX_RESULTS ? ap_count : WIFI_MANAGER_SCAN_MAX_RESULTS;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_records(&records_to_read, records), TAG, "Falha ao obter redes");

    for (uint16_t i = 0; i < records_to_read; i++) {
        strlcpy(results[i].ssid, (const char *)records[i].ssid, sizeof(results[i].ssid));
        results[i].rssi = records[i].rssi;
        results[i].channel = records[i].primary;
        strlcpy(results[i].authmode, authmode_to_text(records[i].authmode), sizeof(results[i].authmode));
    }

    *count = records_to_read;
    ESP_LOGI(TAG, "Scan Wi-Fi concluido com %u redes", records_to_read);
    event_log_add("Scan Wi-Fi encontrou %u redes", records_to_read);
    return ESP_OK;
}

const char *wifi_manager_state_to_text(wifi_manager_sta_state_t state)
{
    switch (state) {
    case WIFI_MANAGER_STA_IDLE:
        return "Aguardando configuracao";
    case WIFI_MANAGER_STA_CONNECTING:
        return "Conectando";
    case WIFI_MANAGER_STA_CONNECTED:
        return "Conectado";
    case WIFI_MANAGER_STA_DISCONNECTED:
        return "Desconectado; tentando reconectar";
    default:
        return "Estado desconhecido";
    }
}

esp_err_t wifi_manager_start(void)
{
    ESP_LOGI(TAG, "Inicializando Wi-Fi em modo APSTA");
    ESP_RETURN_ON_ERROR(app_config_load(&s_app_config), TAG, "Falha ao carregar config AP");
    ESP_RETURN_ON_ERROR(storage_load_dns_server(s_dns_server, sizeof(s_dns_server)), TAG, "Falha ao carregar DNS");
    ESP_LOGI(TAG, "DNS inicial do SoftAP: %s", s_dns_server);

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "Falha ao inicializar esp_netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "Falha ao criar event loop");

    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_ap_netif == NULL || s_sta_netif == NULL) {
        ESP_LOGE(TAG, "Falha ao criar interfaces AP/STA");
        event_log_add("Falha ao criar interfaces AP/STA");
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(configure_ap_ip(), TAG, "Falha ao configurar IP/DHCP do AP");

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "Falha ao inicializar Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL),
                        TAG,
                        "Falha ao registrar handler Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL),
                        TAG,
                        "Falha ao registrar handler IP");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Falha ao configurar storage Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "Falha ao configurar modo APSTA");

    ESP_RETURN_ON_ERROR(configure_softap(), TAG, "Falha ao configurar SoftAP");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Falha ao iniciar Wi-Fi");

    char ssid[STORAGE_WIFI_SSID_MAX_LEN];
    char password[STORAGE_WIFI_PASSWORD_MAX_LEN];
    esp_err_t err = storage_load_wifi_credentials(ssid, sizeof(ssid), password, sizeof(password));
    if (err == ESP_OK && ssid[0] != '\0') {
        ESP_LOGI(TAG, "Credenciais salvas encontradas; iniciando conexao STA");
        event_log_add("Credenciais STA salvas encontradas");
        return wifi_manager_connect_to(ssid, password);
    }

    status_led_set_state(STATUS_LED_READY);
    ESP_LOGI(TAG, "Sem credenciais STA salvas; aguardando configuracao via web");
    event_log_add("Pronto para configurar Wi-Fi externo");
    return ESP_OK;
}
