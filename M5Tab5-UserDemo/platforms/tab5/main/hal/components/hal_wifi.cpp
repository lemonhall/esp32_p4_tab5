/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal/hal_esp32.h"
#include <mooncake_log.h>
#include <bsp/m5stack_tab5.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_wifi.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_http_server.h>
#include <lwip/ip4_addr.h>
#include <algorithm>
#include <string.h>

#define TAG "wifi"

#define WIFI_AP_SSID    "M5Tab5-UserDemo-WiFi"
#define WIFI_AP_PASS    ""
#define WIFI_AP_MAX_STA 4

static constexpr const char* kNvsNamespace = "wifi";
static constexpr const char* kNvsKeySsid   = "ssid";
static constexpr const char* kNvsKeyPass   = "pass";

static constexpr uint32_t kStaReconnectBackoffMs = 1000;
static constexpr uint32_t kStaFailToApTimeoutMs  = 30 * 1000;

static HalEsp32* s_hal = nullptr;
static bool s_stack_inited = false;
static bool s_wifi_inited  = false;
static httpd_handle_t s_httpd = nullptr;
static esp_netif_t* s_netif_ap  = nullptr;
static esp_netif_t* s_netif_sta = nullptr;
static EventGroupHandle_t s_wifi_evt = nullptr;
static esp_event_handler_instance_t s_wifi_any_id = nullptr;
static esp_event_handler_instance_t s_ip_got_ip   = nullptr;

static volatile bool s_force_ap = false;
static volatile bool s_apply_sta_from_nvs = false;
static bool s_has_sta_cfg = false;

static uint32_t s_last_sta_connected_ms  = 0;
static uint32_t s_last_sta_disconnected_ms = 0;
static uint32_t s_last_reconnect_attempt_ms = 0;

enum {
    WIFI_EVT_STA_GOT_IP      = BIT0,
    WIFI_EVT_STA_DISCONNECTED = BIT1,
};

static uint32_t ms_now()
{
    if (!s_hal) {
        return 0;
    }
    return s_hal->millis();
}

static void url_decode_inplace(char* s)
{
    char* src = s;
    char* dst = s;
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
            continue;
        }
        if (*src == '%' && src[1] && src[2]) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                return -1;
            };
            int hi = hex(src[1]);
            int lo = hex(src[2]);
            if (hi >= 0 && lo >= 0) {
                *dst++ = (char)((hi << 4) | lo);
                src += 3;
                continue;
            }
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

static bool nvs_read_string(const char* key, char* out, size_t out_len)
{
    if (!out || out_len == 0) {
        return false;
    }
    out[0] = '\0';

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return false;
    }
    size_t required = 0;
    err = nvs_get_str(nvs, key, nullptr, &required);
    if (err != ESP_OK || required == 0 || required > out_len) {
        nvs_close(nvs);
        return false;
    }
    err = nvs_get_str(nvs, key, out, &required);
    nvs_close(nvs);
    return err == ESP_OK;
}

static bool nvs_write_string(const char* key, const char* value)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return false;
    }
    err = nvs_set_str(nvs, key, value ? value : "");
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err == ESP_OK;
}

static void wifi_set_state(hal::HalBase::WifiState_t state, const char* ip = nullptr)
{
    if (!s_hal) {
        return;
    }
    s_hal->setWifiState(state, ip ? std::string(ip) : std::string());
}

static esp_err_t http_get_root(httpd_req_t* req)
{
    const char* html = R"rawliteral(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Tab5 Wi-Fi Setup</title>
  <style>
    body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial;margin:0;padding:24px;background:#f6f7fb}
    .card{max-width:520px;margin:0 auto;background:#fff;border-radius:12px;padding:18px;box-shadow:0 8px 24px rgba(0,0,0,.08)}
    h1{font-size:18px;margin:0 0 12px}
    label{display:block;font-size:14px;margin:12px 0 6px;color:#333}
    input{width:100%;padding:12px 10px;border:1px solid #ddd;border-radius:10px;font-size:15px}
    button{margin-top:14px;width:100%;padding:12px 10px;border:0;border-radius:10px;background:#2563eb;color:#fff;font-size:15px}
    .hint{margin-top:10px;font-size:12px;color:#666;line-height:1.5}
  </style>
</head>
<body>
  <div class="card">
    <h1>Tab5 Wi‑Fi 配置</h1>
    <form method="post" action="/wifi">
      <label>SSID</label>
      <input name="ssid" autocomplete="off" required>
      <label>Password（可留空）</label>
      <input name="pass" type="password" autocomplete="off">
      <button type="submit">保存并连接</button>
    </form>
    <div class="hint">
      保存后设备会尝试连接路由器；成功后将关闭此热点。<br>
      若路由器不可用/密码错误，约 30 秒后会回到此页面。
    </div>
  </div>
</body>
</html>
)rawliteral";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void wifi_request_apply_sta_from_nvs()
{
    s_apply_sta_from_nvs = true;
}

static esp_err_t http_post_wifi(httpd_req_t* req)
{
    char body[512];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char ssid[64] = {0};
    char pass[64] = {0};

    const char* ssid_key = "ssid=";
    const char* pass_key = "pass=";

    char* ssid_pos = strstr(body, ssid_key);
    if (ssid_pos) {
        ssid_pos += strlen(ssid_key);
        char* end = strchr(ssid_pos, '&');
        size_t len = end ? (size_t)(end - ssid_pos) : strlen(ssid_pos);
        len = std::min(len, sizeof(ssid) - 1);
        memcpy(ssid, ssid_pos, len);
        ssid[len] = '\0';
        url_decode_inplace(ssid);
    }

    char* pass_pos = strstr(body, pass_key);
    if (pass_pos) {
        pass_pos += strlen(pass_key);
        char* end = strchr(pass_pos, '&');
        size_t len = end ? (size_t)(end - pass_pos) : strlen(pass_pos);
        len = std::min(len, sizeof(pass) - 1);
        memcpy(pass, pass_pos, len);
        pass[len] = '\0';
        url_decode_inplace(pass);
    }

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }

    bool ok1 = nvs_write_string(kNvsKeySsid, ssid);
    bool ok2 = nvs_write_string(kNvsKeyPass, pass);
    mclog::tagInfo(TAG, "wifi saved ssid len={} pass len={}", (int)strlen(ssid), (int)strlen(pass));

    const char* resp_ok = R"rawliteral(
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Saved</title></head><body style="font-family:sans-serif;padding:24px">
<h2>已保存</h2>
<p>设备正在连接路由器…</p>
</body></html>
)rawliteral";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, (ok1 && ok2) ? resp_ok : "save failed", HTTPD_RESP_USE_STRLEN);

    wifi_request_apply_sta_from_nvs();
    return ESP_OK;
}

static httpd_handle_t http_start()
{
    if (s_httpd) {
        return s_httpd;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        return nullptr;
    }

    httpd_uri_t uri_root = {.uri = "/", .method = HTTP_GET, .handler = http_get_root, .user_ctx = nullptr};
    httpd_uri_t uri_wifi = {.uri = "/wifi", .method = HTTP_POST, .handler = http_post_wifi, .user_ctx = nullptr};
    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_wifi);
    s_httpd = server;
    return server;
}

static void http_stop()
{
    if (!s_httpd) {
        return;
    }
    httpd_stop(s_httpd);
    s_httpd = nullptr;
}

static void wifi_stack_init_once()
{
    if (s_stack_inited) {
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t loop_ret = esp_event_loop_create_default();
    if (loop_ret != ESP_OK && loop_ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(loop_ret);
    }

    if (!s_netif_ap) {
        s_netif_ap = esp_netif_create_default_wifi_ap();
    }
    if (!s_netif_sta) {
        s_netif_sta = esp_netif_create_default_wifi_sta();
    }

    if (!s_wifi_evt) {
        s_wifi_evt = xEventGroupCreate();
    }

    s_stack_inited = true;
}

static void wifi_init_once()
{
    if (s_wifi_inited) {
        return;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    s_wifi_inited = true;
}

static void wifi_start_ap_only()
{
    wifi_set_state(hal::HalBase::WIFI_PROVISIONING_AP);

    esp_wifi_stop();

    wifi_config_t ap_cfg = {};
    strlcpy((char*)ap_cfg.ap.ssid, WIFI_AP_SSID, sizeof(ap_cfg.ap.ssid));
    strlcpy((char*)ap_cfg.ap.password, WIFI_AP_PASS, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.ssid_len = strlen(WIFI_AP_SSID);
    ap_cfg.ap.max_connection = WIFI_AP_MAX_STA;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    esp_err_t start_ret = esp_wifi_start();
    if (start_ret != ESP_OK && start_ret != ESP_ERR_WIFI_STATE) {
        ESP_ERROR_CHECK(start_ret);
    }

    http_start();
    mclog::tagInfo(TAG, "ap started ssid={} url=http://192.168.4.1", WIFI_AP_SSID);
}

static bool wifi_apply_sta_from_nvs()
{
    char ssid[64] = {0};
    char pass[64] = {0};
    bool has_ssid = nvs_read_string(kNvsKeySsid, ssid, sizeof(ssid));
    if (!has_ssid || ssid[0] == '\0') {
        return false;
    }
    nvs_read_string(kNvsKeyPass, pass, sizeof(pass));
    s_has_sta_cfg = true;

    wifi_set_state(hal::HalBase::WIFI_STA_CONNECTING);
    mclog::tagInfo(TAG, "sta connect ssid_len={} pass_len={}", (int)strlen(ssid), (int)strlen(pass));

    esp_wifi_stop();

    wifi_config_t ap_cfg = {};
    strlcpy((char*)ap_cfg.ap.ssid, WIFI_AP_SSID, sizeof(ap_cfg.ap.ssid));
    strlcpy((char*)ap_cfg.ap.password, WIFI_AP_PASS, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.ssid_len       = strlen(WIFI_AP_SSID);
    ap_cfg.ap.max_connection = WIFI_AP_MAX_STA;
    ap_cfg.ap.authmode       = WIFI_AUTH_OPEN;

    wifi_config_t sta_cfg = {};
    strlcpy((char*)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char*)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.threshold.authmode = (pass[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    esp_err_t start_ret = esp_wifi_start();
    if (start_ret != ESP_OK && start_ret != ESP_ERR_WIFI_STATE) {
        ESP_ERROR_CHECK(start_ret);
    }
    ESP_ERROR_CHECK(esp_wifi_connect());

    http_stop();
    return true;
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            xEventGroupClearBits(s_wifi_evt, WIFI_EVT_STA_GOT_IP);
            xEventGroupSetBits(s_wifi_evt, WIFI_EVT_STA_DISCONNECTED);
            s_last_sta_disconnected_ms = ms_now();
            wifi_set_state(hal::HalBase::WIFI_STA_CONNECTING);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        char ip_str[16] = {0};
        ip4addr_ntoa_r((const ip4_addr_t*)&event->ip_info.ip, ip_str, sizeof(ip_str));
        xEventGroupClearBits(s_wifi_evt, WIFI_EVT_STA_DISCONNECTED);
        xEventGroupSetBits(s_wifi_evt, WIFI_EVT_STA_GOT_IP);
        s_last_sta_connected_ms = ms_now();
        wifi_set_state(hal::HalBase::WIFI_STA_CONNECTED, ip_str);

        mclog::tagInfo(TAG, "sta got ip {}", ip_str);

        esp_err_t mode_ret = esp_wifi_set_mode(WIFI_MODE_STA);
        if (mode_ret == ESP_OK) {
            mclog::tagInfo(TAG, "ap disabled (sta only)");
        } else {
            mclog::tagWarn(TAG, "failed to disable ap: {}", esp_err_to_name(mode_ret));
        }
    }
}

static void wifi_register_handlers_once()
{
    if (s_wifi_any_id && s_ip_got_ip) {
        return;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr,
                                                       &s_wifi_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr,
                                                       &s_ip_got_ip));
}

static void wifi_manager_task(void*)
{
    wifi_set_state(hal::HalBase::WIFI_STOPPED);

    while (1) {
        if (s_force_ap) {
            s_force_ap = false;
            wifi_start_ap_only();
        }

        if (s_apply_sta_from_nvs) {
            s_apply_sta_from_nvs = false;
            if (!wifi_apply_sta_from_nvs()) {
                wifi_start_ap_only();
            }
        }

        bool sta_connected = xEventGroupGetBits(s_wifi_evt) & WIFI_EVT_STA_GOT_IP;
        if (!sta_connected) {
            uint32_t now = ms_now();
            if (s_has_sta_cfg && s_last_sta_disconnected_ms != 0 &&
                (now - s_last_sta_disconnected_ms) > kStaFailToApTimeoutMs &&
                s_httpd == nullptr) {
                mclog::tagWarn(TAG, "sta down >{}ms, fallback to ap", (int)kStaFailToApTimeoutMs);
                wifi_start_ap_only();
            } else if (s_has_sta_cfg) {
                if (now - s_last_reconnect_attempt_ms > kStaReconnectBackoffMs) {
                    esp_wifi_connect();
                    s_last_reconnect_attempt_ms = now;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

bool HalEsp32::wifi_init()
{
    mclog::tagInfo(TAG, "wifi init");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_stack_init_once();
    wifi_init_once();
    wifi_register_handlers_once();

    if (!s_hal) {
        s_hal = this;
    }

    static bool task_started = false;
    if (!task_started) {
        task_started = true;
        xTaskCreate(wifi_manager_task, "wifi_mgr", 6144, nullptr, 5, nullptr);
    }
    return true;
}

void HalEsp32::setExtAntennaEnable(bool enable)
{
    _ext_antenna_enable = enable;
    mclog::tagInfo(TAG, "set ext antenna enable: {}", _ext_antenna_enable);
    bsp_set_ext_antenna_enable(_ext_antenna_enable);
}

bool HalEsp32::getExtAntennaEnable()
{
    return _ext_antenna_enable;
}

void HalEsp32::startWifiAp()
{
    startWifiManager();
    s_force_ap = true;
}

void HalEsp32::startWifiManager()
{
    wifi_init();
    if (!wifi_apply_sta_from_nvs()) {
        wifi_start_ap_only();
    }
}

hal::HalBase::WifiState_t HalEsp32::getWifiState()
{
    return _wifi_state;
}

bool HalEsp32::isWifiStaConnected()
{
    return _wifi_state == WIFI_STA_CONNECTED;
}

std::string HalEsp32::getWifiStaIp()
{
    return _wifi_sta_ip;
}
