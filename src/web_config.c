#include "web_config.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "connectivity.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "event_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "status_led.h"
#include "storage.h"
#include "wifi_manager.h"

static const char *TAG = "web_config";
static httpd_handle_t s_server;

static void html_escape(char *dst, size_t dst_size, const char *src)
{
    size_t out = 0;

    if (dst_size == 0) {
        return;
    }

    while (src != NULL && *src != '\0' && out + 1 < dst_size) {
        const char *replacement = NULL;

        switch (*src) {
        case '&':
            replacement = "&amp;";
            break;
        case '<':
            replacement = "&lt;";
            break;
        case '>':
            replacement = "&gt;";
            break;
        case '"':
            replacement = "&quot;";
            break;
        case '\'':
            replacement = "&#39;";
            break;
        default:
            break;
        }

        if (replacement != NULL) {
            size_t replacement_len = strlen(replacement);
            if (out + replacement_len >= dst_size) {
                break;
            }
            memcpy(dst + out, replacement, replacement_len);
            out += replacement_len;
        } else {
            dst[out++] = *src;
        }

        src++;
    }

    dst[out] = '\0';
}

static void json_escape(char *dst, size_t dst_size, const char *src)
{
    size_t out = 0;

    if (dst_size == 0) {
        return;
    }

    while (src != NULL && *src != '\0' && out + 1 < dst_size) {
        if ((*src == '"' || *src == '\\') && out + 2 < dst_size) {
            dst[out++] = '\\';
            dst[out++] = *src++;
        } else if ((unsigned char)*src >= 0x20) {
            dst[out++] = *src++;
        } else {
            src++;
        }
    }

    dst[out] = '\0';
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void url_decode(char *dst, size_t dst_size, const char *src)
{
    size_t out = 0;

    if (dst_size == 0) {
        return;
    }

    while (src != NULL && *src != '\0' && out + 1 < dst_size) {
        if (*src == '+') {
            dst[out++] = ' ';
            src++;
        } else if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            int high = hex_value(src[1]);
            int low = hex_value(src[2]);
            dst[out++] = (char)((high << 4) | low);
            src += 3;
        } else {
            dst[out++] = *src++;
        }
    }

    dst[out] = '\0';
}

static esp_err_t read_request_body(httpd_req_t *req, size_t max_len, char **body_out)
{
    if (req->content_len <= 0 || req->content_len > max_len) {
        ESP_LOGE(TAG, "POST inválido: tamanho %d", req->content_len);
        return ESP_ERR_INVALID_ARG;
    }

    char *body = calloc(1, req->content_len + 1);
    if (body == NULL) {
        ESP_LOGE(TAG, "Sem memória para receber POST");
        return ESP_ERR_NO_MEM;
    }

    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            free(body);
            ESP_LOGE(TAG, "Falha ao receber POST: %d", ret);
            return ESP_FAIL;
        }
        received += ret;
    }

    *body_out = body;
    return ESP_OK;
}

static bool parse_form_value(char *body, const char *target_key, char *value, size_t value_size)
{
    char *saveptr = NULL;

    if (value_size == 0) {
        return false;
    }
    value[0] = '\0';

    for (char *pair = strtok_r(body, "&", &saveptr); pair != NULL; pair = strtok_r(NULL, "&", &saveptr)) {
        char *equals = strchr(pair, '=');
        if (equals == NULL) {
            continue;
        }

        *equals = '\0';
        char key[24];
        url_decode(key, sizeof(key), pair);
        if (strcmp(key, target_key) == 0) {
            url_decode(value, value_size, equals + 1);
            return value[0] != '\0';
        }
    }

    return false;
}

static bool parse_sta_form(char *body, char *ssid, size_t ssid_size, char *password, size_t password_size)
{
    char *saveptr = NULL;

    ssid[0] = '\0';
    password[0] = '\0';

    for (char *pair = strtok_r(body, "&", &saveptr); pair != NULL; pair = strtok_r(NULL, "&", &saveptr)) {
        char *equals = strchr(pair, '=');
        if (equals == NULL) {
            continue;
        }

        *equals = '\0';
        char key[24];
        char value[STORAGE_WIFI_PASSWORD_MAX_LEN];
        url_decode(key, sizeof(key), pair);
        url_decode(value, sizeof(value), equals + 1);

        if (strcmp(key, "ssid") == 0) {
            strlcpy(ssid, value, ssid_size);
        } else if (strcmp(key, "password") == 0) {
            strlcpy(password, value, password_size);
        }
    }

    return ssid[0] != '\0';
}

static bool parse_ap_form(char *body, app_config_t *config)
{
    char *saveptr = NULL;
    char max_clients[8] = { 0 };

    app_config_defaults(config);

    for (char *pair = strtok_r(body, "&", &saveptr); pair != NULL; pair = strtok_r(NULL, "&", &saveptr)) {
        char *equals = strchr(pair, '=');
        if (equals == NULL) {
            continue;
        }

        *equals = '\0';
        char key[24];
        char value[APP_CONFIG_AP_SSID_MAX_LEN];
        url_decode(key, sizeof(key), pair);
        url_decode(value, sizeof(value), equals + 1);

        if (strcmp(key, "ap_ssid") == 0) {
            strlcpy(config->ap_ssid, value, sizeof(config->ap_ssid));
        } else if (strcmp(key, "ap_max_clients") == 0) {
            strlcpy(max_clients, value, sizeof(max_clients));
        } else if (strcmp(key, "ap_ip") == 0) {
            strlcpy(config->ap_ip, value, sizeof(config->ap_ip));
        } else if (strcmp(key, "dhcp_start") == 0) {
            strlcpy(config->dhcp_start, value, sizeof(config->dhcp_start));
        } else if (strcmp(key, "dhcp_end") == 0) {
            strlcpy(config->dhcp_end, value, sizeof(config->dhcp_end));
        }
    }

    config->ap_max_clients = (uint8_t)atoi(max_clients);
    return app_config_validate_ap(config) == ESP_OK;
}

static void delayed_reboot_task(void *arg)
{
    uint32_t delay_ms = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    ESP_LOGI(TAG, "Reiniciando ESP32 por requisição web");
    esp_restart();
}

static void schedule_reboot(uint32_t delay_ms)
{
    xTaskCreate(delayed_reboot_task, "web_reboot", 2048, (void *)(uintptr_t)delay_ms, 5, NULL);
}

static esp_err_t redirect_home(httpd_req_t *req, const char *message)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_sendstr(req, message);
}

static esp_err_t index_get_handler(httpd_req_t *req)
{
    char ssid[STORAGE_WIFI_SSID_MAX_LEN];
    char password[STORAGE_WIFI_PASSWORD_MAX_LEN];
    char ssid_html[STORAGE_WIFI_SSID_MAX_LEN * 6];
    char password_html[STORAGE_WIFI_PASSWORD_MAX_LEN * 6];
    char ap_ssid_html[APP_CONFIG_AP_SSID_MAX_LEN * 6];
    char ap_ip_html[APP_CONFIG_IPV4_STR_MAX_LEN * 6];
    char dhcp_start_html[APP_CONFIG_IPV4_STR_MAX_LEN * 6];
    char dhcp_end_html[APP_CONFIG_IPV4_STR_MAX_LEN * 6];
    char dns_html[STORAGE_DNS_SERVER_MAX_LEN * 6];
    wifi_manager_status_t status;

    wifi_manager_get_status(&status);
    esp_err_t storage_err = storage_load_wifi_credentials(ssid, sizeof(ssid), password, sizeof(password));
    bool has_saved_credentials = storage_err == ESP_OK && ssid[0] != '\0';

    html_escape(ssid_html, sizeof(ssid_html), has_saved_credentials ? ssid : "");
    html_escape(password_html, sizeof(password_html), has_saved_credentials ? password : "");
    html_escape(ap_ssid_html, sizeof(ap_ssid_html), status.ap_config.ap_ssid);
    html_escape(ap_ip_html, sizeof(ap_ip_html), status.ap_config.ap_ip);
    html_escape(dhcp_start_html, sizeof(dhcp_start_html), status.ap_config.dhcp_start);
    html_escape(dhcp_end_html, sizeof(dhcp_end_html), status.ap_config.dhcp_end);
    html_escape(dns_html, sizeof(dns_html), status.dns_server);

    const size_t page_size = 15000;
    char *page = calloc(1, page_size);
    if (page == NULL) {
        ESP_LOGE(TAG, "Sem memoria para montar pagina");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Sem memoria");
        return ESP_FAIL;
    }

    int written = snprintf(
        page,
        page_size,
        "<!doctype html><html lang=\"pt-BR\"><head>"
        "<meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>MaskLink ESP32</title>"
        "<style>"
        ":root{color-scheme:light;--b:#d8dee4;--p:#1463ff;--d:#b42318;--bg:#f5f7fa;--fg:#17202a}"
        "body{font-family:Arial,sans-serif;margin:0;background:var(--bg);color:var(--fg)}"
        "main{max-width:980px;margin:0 auto;padding:20px}"
        "h1{font-size:24px;margin:6px 0 18px}h2{font-size:18px;margin:0 0 12px}"
        "section{background:white;border:1px solid var(--b);border-radius:8px;padding:16px;margin:0 0 14px}"
        ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:10px}.kv{padding:8px;border-bottom:1px solid #edf1f5}"
        ".kv b{display:block;font-size:12px;color:#536471}.kv span{font-size:15px}"
        "label{display:block;margin-top:10px;font-weight:700;font-size:14px}"
        "input{width:100%%;box-sizing:border-box;margin-top:5px;padding:10px;border:1px solid #b8c2cc;border-radius:4px;font-size:15px}"
        "button{margin-top:12px;padding:10px 12px;border:0;border-radius:4px;background:var(--p);color:white;font-size:15px;font-weight:700;cursor:pointer}"
        "button.secondary{background:#475569}button.danger{background:var(--d)}button.inline{width:auto;margin-right:8px}"
        "table{width:100%%;border-collapse:collapse}th,td{text-align:left;border-bottom:1px solid #edf1f5;padding:8px;font-size:14px}"
        ".actions form{display:inline}.muted{color:#536471;font-size:13px}.ok{color:#067647}.bad{color:#b42318}"
        "#scan-results button{background:#eef4ff;color:#123a7a;border:1px solid #b8cdf8;margin:4px 4px 0 0}"
        "pre{white-space:pre-wrap;background:#101828;color:#f2f4f7;border-radius:6px;padding:10px;max-height:260px;overflow:auto}"
        "</style>"
        "<script>"
        "function t(v){return v===undefined||v===null||v===''?'-':v}"
        "async function refreshStatus(){try{const r=await fetch('/status');const s=await r.json();"
        "document.getElementById('state').textContent=s.state;"
        "document.getElementById('internet').textContent=s.internet?'OK':'sem internet';"
        "document.getElementById('internet').className=s.internet?'ok':'bad';"
        "document.getElementById('sta-ssid').textContent=t(s.ssid);document.getElementById('sta-ip').textContent=t(s.sta_ip);"
        "document.getElementById('sta-gw').textContent=t(s.gateway);document.getElementById('sta-dns').textContent=t(s.sta_dns);"
        "document.getElementById('rssi').textContent=s.rssi?String(s.rssi)+' dBm':'-';document.getElementById('nat').textContent=s.napt?'ativo':'inativo';"
        "document.getElementById('uptime').textContent=s.uptime+' s';document.getElementById('heap').textContent=s.heap_free+' bytes';"
        "document.getElementById('ap-current').textContent=s.ap_ssid+' / '+s.ap_ip;document.getElementById('clients-count').textContent=s.client_count;"
        "const cb=document.getElementById('clients-body');cb.innerHTML='';"
        "if(!s.clients.length){cb.innerHTML='<tr><td colspan=\"2\">Nenhum dispositivo conectado</td></tr>'}else{s.clients.forEach(c=>{const tr=document.createElement('tr');tr.innerHTML='<td>'+c.mac+'</td><td>'+t(c.ip)+'</td>';cb.appendChild(tr)})}"
        "const lb=document.getElementById('logs');lb.textContent=s.events.map(e=>'['+e.uptime+'s] '+e.message).join('\\n')||'Sem eventos';"
        "}catch(e){}}"
        "async function scanWifi(){const box=document.getElementById('scan-results');box.textContent='Buscando redes...';try{const r=await fetch('/scan');const s=await r.json();"
        "if(!s.ok){box.textContent=s.message||'Falha no scan';return}box.innerHTML='';s.networks.forEach(n=>{const b=document.createElement('button');b.type='button';b.textContent=(n.ssid||'<oculta>')+' ('+n.rssi+' dBm, ch '+n.channel+', '+n.auth+')';b.onclick=()=>{document.getElementById('ssid').value=n.ssid};box.appendChild(b)})"
        "}catch(e){box.textContent='Falha no scan'}}"
        "async function testInternet(){const box=document.getElementById('test-result');box.textContent='Testando...';try{const r=await fetch('/connectivity/test',{method:'POST'});const s=await r.json();"
        "box.textContent='STA: '+(s.sta_connected?'OK':'falhou')+' | Ping: '+(s.ping_ok?'OK':'falhou')+' | DNS: '+(s.dns_ok?'OK':'falhou')+' | HTTP: '+(s.http_ok?'OK':'falhou')+'\\n'+s.message;"
        "}catch(e){box.textContent='Falha ao testar'}}"
        "setInterval(refreshStatus,3000);window.addEventListener('load',refreshStatus);"
        "</script></head><body><main>"
        "<h1>MaskLink ESP32</h1>"
        "<section><h2>Status</h2><div class=\"grid\">"
        "<div class=\"kv\"><b>Estado STA</b><span id=\"state\">-</span></div>"
        "<div class=\"kv\"><b>Internet</b><span id=\"internet\">-</span></div>"
        "<div class=\"kv\"><b>Rede externa</b><span id=\"sta-ssid\">-</span></div>"
        "<div class=\"kv\"><b>IP STA</b><span id=\"sta-ip\">-</span></div>"
        "<div class=\"kv\"><b>Gateway STA</b><span id=\"sta-gw\">-</span></div>"
        "<div class=\"kv\"><b>DNS STA</b><span id=\"sta-dns\">-</span></div>"
        "<div class=\"kv\"><b>RSSI</b><span id=\"rssi\">-</span></div>"
        "<div class=\"kv\"><b>NAT</b><span id=\"nat\">-</span></div>"
        "<div class=\"kv\"><b>Uptime</b><span id=\"uptime\">-</span></div>"
        "<div class=\"kv\"><b>Heap livre</b><span id=\"heap\">-</span></div>"
        "<div class=\"kv\"><b>AP atual</b><span id=\"ap-current\">-</span></div>"
        "<div class=\"kv\"><b>Clientes AP</b><span id=\"clients-count\">-</span></div>"
        "</div></section>"
        "<section><h2>Rede externa</h2>"
        "<form method=\"post\" action=\"/save\">"
        "<label for=\"ssid\">SSID</label><input id=\"ssid\" name=\"ssid\" maxlength=\"32\" value=\"%s\" required>"
        "<label for=\"password\">Senha</label><input id=\"password\" name=\"password\" type=\"text\" maxlength=\"64\" value=\"%s\">"
        "<button type=\"submit\">Salvar e conectar</button>"
        "</form>"
        "<button type=\"button\" class=\"secondary inline\" onclick=\"scanWifi()\">Buscar redes</button><div id=\"scan-results\" class=\"muted\"></div>"
        "</section>"
        "<section><h2>AP do ESP32</h2>"
        "<form method=\"post\" action=\"/ap-config\">"
        "<label for=\"ap_ssid\">SSID do AP</label><input id=\"ap_ssid\" name=\"ap_ssid\" maxlength=\"32\" value=\"%s\" required>"
        "<label for=\"ap_max_clients\">Máximo de clientes</label><input id=\"ap_max_clients\" name=\"ap_max_clients\" type=\"number\" min=\"1\" max=\"10\" value=\"%u\" required>"
        "<label for=\"ap_ip\">IP do AP</label><input id=\"ap_ip\" name=\"ap_ip\" maxlength=\"15\" value=\"%s\" required>"
        "<label for=\"dhcp_start\">DHCP inicio</label><input id=\"dhcp_start\" name=\"dhcp_start\" maxlength=\"15\" value=\"%s\" required>"
        "<label for=\"dhcp_end\">DHCP fim</label><input id=\"dhcp_end\" name=\"dhcp_end\" maxlength=\"15\" value=\"%s\" required>"
        "<button type=\"submit\">Salvar AP e reiniciar</button>"
        "</form><p class=\"muted\">Senha do AP fixa: 12345678. Mudancas de AP/IP sao aplicadas apos reiniciar.</p>"
        "</section>"
        "<section><h2>DNS dos clientes do AP</h2>"
        "<form method=\"post\" action=\"/dns\"><label for=\"dns\">DNS</label><input id=\"dns\" name=\"dns\" maxlength=\"15\" value=\"%s\" required><button type=\"submit\">Salvar DNS</button></form>"
        "<form method=\"post\" action=\"/dns/reset\"><button class=\"secondary\" type=\"submit\">Resetar DNS para 8.8.8.8</button></form>"
        "</section>"
        "<section><h2>Dispositivos conectados</h2><table><thead><tr><th>MAC</th><th>IP</th></tr></thead><tbody id=\"clients-body\"><tr><td colspan=\"2\">Carregando...</td></tr></tbody></table></section>"
        "<section><h2>Teste de internet</h2><button type=\"button\" onclick=\"testInternet()\">Testar conexao</button><pre id=\"test-result\">Aguardando teste</pre></section>"
        "<section><h2>Logs</h2><pre id=\"logs\">Carregando...</pre></section>"
        "<section class=\"actions\"><h2>Ações</h2>"
        "%s"
        "<form method=\"post\" action=\"/reboot\"><button class=\"secondary inline\" type=\"submit\">Reiniciar ESP32</button></form>"
        "<form method=\"post\" action=\"/factory-reset\"><button class=\"danger inline\" type=\"submit\">Resetar tudo para padrão</button></form>"
        "</section>"
        "</main></body></html>",
        ssid_html,
        password_html,
        ap_ssid_html,
        status.ap_config.ap_max_clients,
        ap_ip_html,
        dhcp_start_html,
        dhcp_end_html,
        dns_html,
        has_saved_credentials
            ? "<form method=\"post\" action=\"/forget\"><button class=\"danger inline\" type=\"submit\">Esquecer Wi-Fi externo</button></form>"
            : "");

    if (written < 0 || written >= (int)page_size) {
        free(page);
        ESP_LOGE(TAG, "Página excedeu o buffer");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Página muito grande");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
    free(page);
    return err;
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    wifi_manager_status_t status;
    wifi_manager_ap_client_t clients[WIFI_MANAGER_AP_CLIENT_LIMIT];
    event_log_entry_t events[EVENT_LOG_MAX_ENTRIES];
    char ssid_json[STORAGE_WIFI_SSID_MAX_LEN * 2];
    char dns_json[STORAGE_DNS_SERVER_MAX_LEN * 2];
    char ap_ssid_json[APP_CONFIG_AP_SSID_MAX_LEN * 2];
    size_t offset = 0;

    wifi_manager_get_status(&status);
    size_t client_count = wifi_manager_get_ap_clients(clients, WIFI_MANAGER_AP_CLIENT_LIMIT);
    size_t event_count = event_log_get_entries(events, EVENT_LOG_MAX_ENTRIES);
    json_escape(ssid_json, sizeof(ssid_json), status.ssid);
    json_escape(dns_json, sizeof(dns_json), status.dns_server);
    json_escape(ap_ssid_json, sizeof(ap_ssid_json), status.ap_config.ap_ssid);

    char *response = calloc(1, 9000);
    if (response == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Sem memória");
        return ESP_FAIL;
    }
    size_t response_size = 9000;

#define APPEND_JSON(...)                                                                            \
    do {                                                                                            \
        if (offset < response_size) {                                                               \
            int n = snprintf(response + offset, response_size - offset, __VA_ARGS__);                \
            if (n > 0) {                                                                            \
                offset += (size_t)n;                                                                \
            }                                                                                       \
        }                                                                                           \
    } while (0)

    APPEND_JSON("{\"state\":\"%s\",\"sta_configured\":%s,\"ssid\":\"%s\",\"sta_ip\":\"%s\",",
                wifi_manager_state_to_text(status.sta_state),
                status.sta_configured ? "true" : "false",
                ssid_json,
                status.ip);
    APPEND_JSON("\"gateway\":\"%s\",\"sta_dns\":\"%s\",\"rssi\":%d,\"napt\":%s,\"internet\":%s,",
                status.gateway,
                status.sta_dns,
                status.rssi,
                status.napt_enabled ? "true" : "false",
                connectivity_is_internet_ok() ? "true" : "false");
    APPEND_JSON("\"dns\":\"%s\",\"uptime\":%llu,\"heap_free\":%u,",
                dns_json,
                (unsigned long long)(esp_timer_get_time() / 1000000ULL),
                (unsigned)esp_get_free_heap_size());
    APPEND_JSON("\"ap_ssid\":\"%s\",\"ap_ip\":\"%s\",\"dhcp_start\":\"%s\",\"dhcp_end\":\"%s\",\"ap_max_clients\":%u,",
                ap_ssid_json,
                status.ap_config.ap_ip,
                status.ap_config.dhcp_start,
                status.ap_config.dhcp_end,
                status.ap_config.ap_max_clients);
    APPEND_JSON("\"client_count\":%u,\"clients\":[", (unsigned)client_count);
    for (size_t i = 0; i < client_count; i++) {
        APPEND_JSON("%s{\"mac\":\"%s\",\"ip\":\"%s\"}", i == 0 ? "" : ",", clients[i].mac, clients[i].ip);
    }
    APPEND_JSON("],\"events\":[");
    for (size_t i = 0; i < event_count; i++) {
        char message_json[EVENT_LOG_MESSAGE_MAX_LEN * 2];
        json_escape(message_json, sizeof(message_json), events[i].message);
        APPEND_JSON("%s{\"uptime\":%lu,\"message\":\"%s\"}",
                    i == 0 ? "" : ",",
                    events[i].uptime_s,
                    message_json);
    }
    APPEND_JSON("]}");

#undef APPEND_JSON

    if (offset >= response_size) {
        response[response_size - 1] = '\0';
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    free(response);
    return err;
}

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    wifi_manager_scan_result_t networks[WIFI_MANAGER_SCAN_MAX_RESULTS];
    uint16_t count = 0;
    char *response = calloc(1, 4200);
    if (response == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Sem memoria");
        return ESP_FAIL;
    }

    esp_err_t scan_err = wifi_manager_scan_networks(networks, WIFI_MANAGER_SCAN_MAX_RESULTS, &count);
    size_t offset = 0;
    if (scan_err != ESP_OK) {
        snprintf(response,
                 4200,
                 "{\"ok\":false,\"message\":\"Falha no scan: %s\",\"networks\":[]}",
                 esp_err_to_name(scan_err));
    } else {
        offset += snprintf(response + offset, 4200 - offset, "{\"ok\":true,\"networks\":[");
        for (uint16_t i = 0; i < count && offset < 4200; i++) {
            char ssid_json[70];
            json_escape(ssid_json, sizeof(ssid_json), networks[i].ssid);
            offset += snprintf(response + offset,
                               4200 - offset,
                               "%s{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%u,\"auth\":\"%s\"}",
                               i == 0 ? "" : ",",
                               ssid_json,
                               networks[i].rssi,
                               networks[i].channel,
                               networks[i].authmode);
        }
        snprintf(response + offset, 4200 - offset, "]}");
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    free(response);
    return err;
}

static esp_err_t connectivity_test_post_handler(httpd_req_t *req)
{
    (void)req;
    connectivity_test_result_t result;
    esp_err_t err = connectivity_run_manual_test(&result);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Falha ao testar");
        return ESP_FAIL;
    }

    char message_json[160];
    char response[360];
    json_escape(message_json, sizeof(message_json), result.message);
    snprintf(response,
             sizeof(response),
             "{\"sta_connected\":%s,\"ping_ok\":%s,\"dns_ok\":%s,\"http_ok\":%s,\"internet_ok\":%s,\"message\":\"%s\"}",
             result.sta_connected ? "true" : "false",
             result.ping_ok ? "true" : "false",
             result.dns_ok ? "true" : "false",
             result.http_ok ? "true" : "false",
             result.internet_ok ? "true" : "false",
             message_json);

    event_log_add("Teste manual de internet: %s", result.message);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, response);
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    char *body = NULL;
    esp_err_t err = read_request_body(req, 512, &body);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Formulario invalido");
        return ESP_FAIL;
    }

    char ssid[STORAGE_WIFI_SSID_MAX_LEN];
    char password[STORAGE_WIFI_PASSWORD_MAX_LEN];
    bool valid = parse_sta_form(body, ssid, sizeof(ssid), password, sizeof(password));
    free(body);

    if (!valid) {
        ESP_LOGE(TAG, "Formulario sem SSID valido");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID obrigatorio");
        return ESP_FAIL;
    }

    status_led_set_state(STATUS_LED_SAVING);
    ESP_LOGI(TAG, "Recebido Wi-Fi externo: ssid='%s', senha_len=%u", ssid, (unsigned)strlen(password));
    event_log_add("Wi-Fi externo salvo via web: %s", ssid);
    err = storage_save_wifi_credentials(ssid, password);
    if (err == ESP_OK) {
        err = wifi_manager_connect_to(ssid, password);
    }
    if (err != ESP_OK) {
        status_led_set_state(STATUS_LED_ERROR);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Falha ao salvar ou conectar");
        return ESP_FAIL;
    }

    return redirect_home(req, "Credenciais salvas");
}

static esp_err_t dns_post_handler(httpd_req_t *req)
{
    char *body = NULL;
    esp_err_t err = read_request_body(req, 128, &body);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Formulario DNS invalido");
        return ESP_FAIL;
    }

    char dns_server[STORAGE_DNS_SERVER_MAX_LEN];
    bool valid = parse_form_value(body, "dns", dns_server, sizeof(dns_server));
    free(body);
    if (!valid) {
        ESP_LOGE(TAG, "Formulario DNS sem valor valido");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "DNS obrigatorio");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Recebido novo DNS via web: %s", dns_server);
    event_log_add("Novo DNS recebido via web: %s", dns_server);
    err = wifi_manager_set_dns_server(dns_server);
    if (err == ESP_OK) {
        err = storage_save_dns_server(dns_server);
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "DNS invalido ou falha ao salvar");
        return ESP_FAIL;
    }

    return redirect_home(req, "DNS salvo");
}

static esp_err_t dns_reset_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Requisicao para resetar DNS");
    event_log_add("Reset de DNS solicitado");

    esp_err_t err = storage_reset_dns_server();
    if (err == ESP_OK) {
        err = wifi_manager_reset_dns_server();
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Falha ao resetar DNS");
        return ESP_FAIL;
    }

    return redirect_home(req, "DNS resetado");
}

static esp_err_t forget_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Requisicao para esquecer credenciais Wi-Fi");
    event_log_add("Esquecer Wi-Fi externo solicitado");

    esp_err_t err = storage_forget_wifi_credentials();
    if (err == ESP_OK) {
        err = wifi_manager_forget_sta();
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Falha ao esquecer credenciais");
        return ESP_FAIL;
    }

    return redirect_home(req, "Credenciais apagadas");
}

static esp_err_t ap_config_post_handler(httpd_req_t *req)
{
    char *body = NULL;
    esp_err_t err = read_request_body(req, 512, &body);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Formulario AP invalido");
        return ESP_FAIL;
    }

    app_config_t config;
    bool valid = parse_ap_form(body, &config);
    free(body);
    if (!valid) {
        ESP_LOGE(TAG, "Config AP invalida recebida");
        event_log_add("Config AP invalida rejeitada");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Config AP invalida");
        return ESP_FAIL;
    }

    status_led_set_state(STATUS_LED_SAVING);
    err = app_config_save_ap(&config);
    if (err != ESP_OK) {
        status_led_set_state(STATUS_LED_ERROR);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Falha ao salvar config AP");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Config AP salva via web; reinicio agendado");
    event_log_add("Config AP salva; reiniciando para aplicar");
    schedule_reboot(1000);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, "<!doctype html><html><body><h1>Reiniciando</h1><p>Config AP salva.</p></body></html>");
}

static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Reboot solicitado pela interface web");
    event_log_add("Reboot solicitado pela web");
    status_led_set_state(STATUS_LED_SAVING);
    schedule_reboot(800);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, "<!doctype html><html><body><h1>Reiniciando</h1></body></html>");
}

static esp_err_t factory_reset_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Reset geral solicitado pela interface web");
    event_log_add("Reset geral solicitado");
    status_led_set_state(STATUS_LED_FACTORY_RESET);

    esp_err_t err = app_config_reset_all();
    if (err != ESP_OK) {
        status_led_set_state(STATUS_LED_ERROR);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Falha ao resetar configuracoes");
        return ESP_FAIL;
    }

    event_log_clear();
    event_log_add("Configurações resetadas; reiniciando");
    schedule_reboot(5500);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, "<!doctype html><html><body><h1>Resetando</h1><p>Voltando para os padroes.</p></body></html>");
}

static esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    wifi_manager_status_t status;
    wifi_manager_get_status(&status);
    char location[48];
    snprintf(location, sizeof(location), "http://%s/", status.ap_config.ap_ip);

    ESP_LOGI(TAG, "Redirecionando captive portal: %s -> %s", req->uri, location);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    return httpd_resp_sendstr(req, "Redirecionando para configuracao");
}

esp_err_t web_config_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192;
    config.max_uri_handlers = 16;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar servidor HTTP: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t handlers[] = {
        { .uri = "/", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = NULL },
        { .uri = "/status", .method = HTTP_GET, .handler = status_get_handler, .user_ctx = NULL },
        { .uri = "/scan", .method = HTTP_GET, .handler = scan_get_handler, .user_ctx = NULL },
        { .uri = "/connectivity/test", .method = HTTP_POST, .handler = connectivity_test_post_handler, .user_ctx = NULL },
        { .uri = "/save", .method = HTTP_POST, .handler = save_post_handler, .user_ctx = NULL },
        { .uri = "/forget", .method = HTTP_POST, .handler = forget_post_handler, .user_ctx = NULL },
        { .uri = "/dns", .method = HTTP_POST, .handler = dns_post_handler, .user_ctx = NULL },
        { .uri = "/dns/reset", .method = HTTP_POST, .handler = dns_reset_post_handler, .user_ctx = NULL },
        { .uri = "/ap-config", .method = HTTP_POST, .handler = ap_config_post_handler, .user_ctx = NULL },
        { .uri = "/reboot", .method = HTTP_POST, .handler = reboot_post_handler, .user_ctx = NULL },
        { .uri = "/factory-reset", .method = HTTP_POST, .handler = factory_reset_post_handler, .user_ctx = NULL },
        { .uri = "/*", .method = HTTP_GET, .handler = captive_redirect_handler, .user_ctx = NULL },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &handlers[i]));
    }

    wifi_manager_status_t status;
    wifi_manager_get_status(&status);
    ESP_LOGI(TAG,
             "Servidor web iniciado em http://%s com endpoints /status, /scan e /connectivity/test",
             status.ap_config.ap_ip);
    event_log_add("Servidor web iniciado");
    return ESP_OK;
}
