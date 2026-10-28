#include "wifi_mgr.h"

#include <stdio.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "wifi";

#define NVS_NS          "wifi_cfg"
#define NVS_KEY_SSID    "ssid"
#define NVS_KEY_PASS    "pass"
#define GOT_IP_BIT      BIT0
#define STA_FAIL_BIT    BIT1
#define BOOT_WAIT_MS    12000
#define SCAN_MAX        24

static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_lock;
static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static wifi_mgr_ready_cb_t s_ready_cb;
static volatile bool s_sta_up;
static volatile bool s_ap_up;
static char s_sta_ip[16];
static char s_ssid[33];
static char s_pass[65];
static int s_retry;

static void lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static void load_creds(void)
{
    nvs_handle_t nvs;
    s_ssid[0] = 0;
    s_pass[0] = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    size_t ssid_len = sizeof(s_ssid);
    size_t pass_len = sizeof(s_pass);
    (void)nvs_get_str(nvs, NVS_KEY_SSID, s_ssid, &ssid_len);
    (void)nvs_get_str(nvs, NVS_KEY_PASS, s_pass, &pass_len);
    nvs_close(nvs);
}

static void save_creds(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed");
        return;
    }
    (void)nvs_set_str(nvs, NVS_KEY_SSID, ssid ? ssid : "");
    (void)nvs_set_str(nvs, NVS_KEY_PASS, pass ? pass : "");
    (void)nvs_commit(nvs);
    nvs_close(nvs);
}

static void apply_radio(void)
{
    esp_err_t err;
#if CONFIG_IDF_TARGET_ESP32C5
    err = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "band AUTO: %s", esp_err_to_name(err));
    }
#endif
    err = esp_wifi_set_country_code("CN", true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "country: %s", esp_err_to_name(err));
    }
#if CONFIG_IDF_TARGET_ESP32C5
    wifi_protocols_t sta_proto = {
        .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX,
        .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX,
    };
    wifi_protocols_t ap_proto = {
        .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX,
        .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX,
    };
    err = esp_wifi_set_protocols(WIFI_IF_STA, &sta_proto);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sta proto: %s", esp_err_to_name(err));
    }
    err = esp_wifi_set_protocols(WIFI_IF_AP, &ap_proto);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ap proto: %s", esp_err_to_name(err));
    }
#else
    err = esp_wifi_set_protocol(WIFI_IF_STA,
                                WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N |
                                WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sta proto: %s", esp_err_to_name(err));
    }
    err = esp_wifi_set_protocol(WIFI_IF_AP,
                                WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N |
                                WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ap proto: %s", esp_err_to_name(err));
    }
#endif
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    (void)esp_wifi_set_max_tx_power(80);
}

static esp_err_t config_ap(void)
{
    wifi_config_t ap = { 0 };
    strncpy((char *)ap.ap.ssid, WIFI_AP_SSID, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len = strlen(WIFI_AP_SSID);
    ap.ap.channel = WIFI_AP_CHANNEL;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.beacon_interval = 100;
    ap.ap.pmf_cfg.required = false;
    return esp_wifi_set_config(WIFI_IF_AP, &ap);
}

static esp_err_t config_sta(const char *ssid, const char *pass)
{
    wifi_config_t sta = { 0 };
    strncpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid) - 1);
    if (pass && pass[0]) {
        strncpy((char *)sta.sta.password, pass, sizeof(sta.sta.password) - 1);
    }
    sta.sta.threshold.authmode = pass && pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;
    sta.sta.pmf_cfg.required = false;
    return esp_wifi_set_config(WIFI_IF_STA, &sta);
}

static esp_err_t start_apsta(void)
{
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) return err;
    err = config_ap();
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) return err;
    apply_radio();
    (void)config_ap();
    s_ap_up = true;
    ESP_LOGI(TAG, "provision AP \"%s\"  open  %s", WIFI_AP_SSID, WIFI_AP_URL);
    return ESP_OK;
}

static void stop_ap_keep_sta(void)
{
    if (!s_ap_up) {
        return;
    }
    (void)esp_wifi_set_mode(WIFI_MODE_STA);
    s_ap_up = false;
    ESP_LOGI(TAG, "SoftAP off, STA only");
}

static void on_got_ip(const char *ip)
{
    lock();
    strncpy(s_sta_ip, ip, sizeof(s_sta_ip) - 1);
    s_sta_up = true;
    s_retry = 0;
    unlock();
    save_creds(s_ssid, s_pass);
    stop_ap_keep_sta();
    if (s_events) {
        xEventGroupSetBits(s_events, GOT_IP_BIT);
    }
    ESP_LOGI(TAG, "STA up  ssid=\"%s\" ip=%s", s_ssid, ip);
    if (s_ready_cb) {
        s_ready_cb();
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_ssid[0]) {
            (void)esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_up = false;
        s_sta_ip[0] = 0;
        if (s_ssid[0] && s_retry < 6) {
            s_retry++;
            ESP_LOGW(TAG, "STA retry %d", s_retry);
            (void)esp_wifi_connect();
        } else if (s_events) {
            xEventGroupSetBits(s_events, STA_FAIL_BIT);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI(TAG, "phone " MACSTR " joined AP", MAC2STR(e->mac));
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&e->ip_info.ip));
        on_got_ip(ip);
    }
}

esp_err_t wifi_mgr_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_lock = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    if (s_lock == NULL || s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    load_creds();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    if (s_ssid[0]) {
        ESP_LOGI(TAG, "saved SSID \"%s\", try STA", s_ssid);
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) return err;
        err = config_sta(s_ssid, s_pass);
        if (err != ESP_OK) return err;
        err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) return err;
        apply_radio();
        EventBits_t bits = xEventGroupWaitBits(s_events, GOT_IP_BIT | STA_FAIL_BIT,
                                               pdFALSE, pdFALSE, pdMS_TO_TICKS(BOOT_WAIT_MS));
        if (bits & GOT_IP_BIT) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "STA timeout, fall back to provision AP");
        xEventGroupClearBits(s_events, GOT_IP_BIT | STA_FAIL_BIT);
        s_retry = 0;
    }

    return start_apsta();
}

wifi_mgr_mode_t wifi_mgr_mode(void)
{
    return s_sta_up ? WIFI_MGR_MODE_STA : WIFI_MGR_MODE_AP;
}

bool wifi_mgr_is_sta_up(void)
{
    return s_sta_up;
}

esp_err_t wifi_mgr_get_ip(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    if (s_sta_up && s_sta_ip[0]) {
        strncpy(buf, s_sta_ip, len - 1);
        buf[len - 1] = 0;
    } else {
        strncpy(buf, "192.168.4.1", len - 1);
        buf[len - 1] = 0;
    }
    unlock();
    return ESP_OK;
}

esp_err_t wifi_mgr_get_ssid(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    strncpy(buf, s_sta_up && s_ssid[0] ? s_ssid : WIFI_AP_SSID, len - 1);
    buf[len - 1] = 0;
    unlock();
    return ESP_OK;
}

esp_err_t wifi_mgr_service_url(char *buf, size_t len)
{
    char ip[16];
    wifi_mgr_get_ip(ip, sizeof(ip));
    snprintf(buf, len, "http://%s", ip);
    return ESP_OK;
}

esp_err_t wifi_mgr_scan_json(char *buf, size_t len)
{
    wifi_scan_config_t scan = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    uint16_t n = SCAN_MAX;
    wifi_ap_record_t rec[SCAN_MAX];

    if (buf == NULL || len < 32) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ap_up && !s_sta_up) {
        if (start_apsta() != ESP_OK) {
            snprintf(buf, len, "{\"ok\":false,\"err\":\"wifi_start\"}");
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) {
        snprintf(buf, len, "{\"ok\":false,\"err\":\"%s\"}", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_scan_get_ap_records(&n, rec);
    if (err != ESP_OK) {
        snprintf(buf, len, "{\"ok\":false,\"err\":\"scan\"}");
        return err;
    }

    size_t off = (size_t)snprintf(buf, len, "{\"ok\":true,\"aps\":[");
    for (uint16_t i = 0; i < n && off + 80 < len; i++) {
        char ssid[33] = { 0 };
        memcpy(ssid, rec[i].ssid, sizeof(rec[i].ssid));
        for (char *p = ssid; *p; p++) {
            if (*p == '"' || *p == '\\') {
                *p = ' ';
            }
        }
        const char *band = rec[i].primary >= 36 ? "5G" : "2.4G";
        off += (size_t)snprintf(buf + off, len - off,
                                "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d,\"band\":\"%s\"}",
                                i ? "," : "", ssid, rec[i].rssi, (int)rec[i].authmode, band);
    }
    if (off + 3 < len) {
        memcpy(buf + off, "]}", 3);
    }
    return ESP_OK;
}

esp_err_t wifi_mgr_connect(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = 0;
    strncpy(s_pass, password ? password : "", sizeof(s_pass) - 1);
    s_pass[sizeof(s_pass) - 1] = 0;
    s_sta_up = false;
    s_retry = 0;
    unlock();

    if (!s_ap_up) {
        esp_err_t start_err = start_apsta();
        if (start_err != ESP_OK) return start_err;
    } else {
        esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (mode_err != ESP_OK) return mode_err;
    }
    esp_err_t err = config_sta(s_ssid, s_pass);
    if (err != ESP_OK) return err;
    (void)esp_wifi_disconnect();
    return esp_wifi_connect();
}

esp_err_t wifi_mgr_clear_creds(void)
{
    s_ssid[0] = 0;
    s_pass[0] = 0;
    s_sta_up = false;
    save_creds("", "");
    return start_apsta();
}

void wifi_mgr_set_ready_cb(wifi_mgr_ready_cb_t cb)
{
    s_ready_cb = cb;
}
