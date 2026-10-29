#include "cJSON.h"
#include "web_server.h"
#include "pin_map.h"
#include "screen_rotation.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "axp2101.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "screen_driver.h"
#include "wifi_mgr.h"
#include "lwip/sockets.h"

static const char *TAG = "web";

typedef enum { JOB_IDLE, JOB_REFRESH, JOB_RECEIVING, JOB_ERROR } job_phase_t;
typedef enum { JOB_FRAME, JOB_CLEAR_SCREEN } job_kind_t;
#define FRAME_FORMAT "otp6-v1"
#define FRAME_CHUNK_BYTES 2048

static portMUX_TYPE s_job_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile job_phase_t s_job_phase = JOB_IDLE;
static esp_err_t s_job_err = ESP_OK;
static TaskHandle_t s_job_task;
static uint32_t s_job_id;
static job_kind_t s_job_kind;
static screen_upload_report_t s_upload_report;
static const char *s_job_step = "idle";
static size_t s_job_bytes;
typedef struct {
    httpd_req_t *req;
    size_t bytes;
    TickType_t started;
    uint32_t hash, expected;
    uint32_t id, base_generation, rotation_epoch;
} frame_rx_t;
static frame_rx_t s_pending;

static void log_heap_stats(const char *event, uint32_t id)
{
    const uint32_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const uint32_t min8 = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    const uint32_t largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    const uint32_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "heap %s id=%u free8=%u min8=%u largest8=%u internal=%u internal_largest=%u",
             event, (unsigned)id, (unsigned)free8, (unsigned)min8, (unsigned)largest8,
             (unsigned)free_internal, (unsigned)largest_internal);
}


static esp_err_t send_json(httpd_req_t *req, const char *json);
static void frame_job_task(void *arg);
static const char *job_kind_name(job_kind_t kind)
{
    return kind == JOB_CLEAR_SCREEN ? "clear-screen" : "frame";
}
static const char *job_phase_name(job_phase_t phase)
{
    switch (phase) {
    case JOB_REFRESH: return "refresh";
    case JOB_RECEIVING: return "receiving";
    case JOB_ERROR: return "error";
    default: return "idle";
    }
}

bool web_server_screen_busy(void){return s_job_phase == JOB_RECEIVING || s_job_phase == JOB_REFRESH;}
static bool begin_job(job_kind_t kind)
{
    portENTER_CRITICAL(&s_job_lock);
    bool available = !screen_rotation_busy() && (s_job_phase == JOB_IDLE || s_job_phase == JOB_ERROR);
    if (available) {
        if (++s_job_id == 0) ++s_job_id;
        s_job_kind = kind;
        s_job_err = ESP_OK;
        s_job_phase = JOB_RECEIVING;
        s_job_step = "queued";
        s_job_bytes = 0;
        memset(&s_upload_report, 0, sizeof(s_upload_report));
    }
    portEXIT_CRITICAL(&s_job_lock);
    return available;
}
static void finish_job(esp_err_t err, const screen_upload_report_t *report)
{
    portENTER_CRITICAL(&s_job_lock);
    s_upload_report = *report;
    s_job_err = err;
    s_job_phase = err == ESP_OK ? JOB_IDLE : JOB_ERROR;
    s_job_step = err == ESP_OK ? "done" : "error";
    portEXIT_CRITICAL(&s_job_lock);
}

/* Append one contract to both job polling and the asynchronous OTP result.
 * Zero hashes mean no complete successful source transmission was recorded. */
static esp_err_t send_report_json(httpd_req_t *req, char *json, size_t capacity, int used,
                                  const screen_upload_report_t *report, const char *sequence)
{
    if (used < 0 || (size_t)used >= capacity) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "job JSON overflow");
        return ESP_FAIL;
    }
    const int written = snprintf(json + used, capacity - (size_t)used,
        "\"sequence\":\"%s\",\"plane_fnv\":[\"%08x\"],\"completed_mask\":%u}",
        sequence, (unsigned)report->frame_fnv, report->completed ? 1U : 0U);
    if (written < 0 || (size_t)written >= capacity - (size_t)used) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "job JSON overflow");
        return ESP_FAIL;
    }
    return send_json(req, json);
}
static esp_err_t send_job_json(httpd_req_t *req)
{
    char json[512];
    portENTER_CRITICAL(&s_job_lock);
    const uint32_t id = s_job_id;
    const job_phase_t phase = s_job_phase;
    const esp_err_t err = s_job_err;
    const job_kind_t kind = s_job_kind;
    const char *step = s_job_step;
    const size_t bytes = s_job_bytes;
    const screen_upload_report_t report = s_upload_report;
    portEXIT_CRITICAL(&s_job_lock);
    const int used = snprintf(json, sizeof(json),
        "{\"ok\":true,\"id\":%u,\"phase\":\"%s\",\"err\":%d,\"kind\":\"%s\","
        "\"step\":\"%s\",\"bytes\":%u,\"total_ms\":%u,\"refresh_ms\":%u,"
        "\"display_generation\":%u,\"current_display_generation\":%u,",
        (unsigned)id, job_phase_name(phase), (int)err, job_kind_name(kind), step,
        (unsigned)bytes, (unsigned)report.total_ms, (unsigned)report.refresh_ms,
        (unsigned)report.display_generation, (unsigned)screen_driver_generation());
    return send_report_json(req, json, sizeof(json), used, &report,
        kind == JOB_CLEAR_SCREEN ? "otp6-clear-v1" : FRAME_FORMAT);
}

extern const uint8_t index_html_start[]        asm("_binary_index_html_start");
extern const uint8_t index_html_end[]          asm("_binary_index_html_end");
extern const uint8_t pipeline_js_start[]       asm("_binary_eink_pipeline_js_start");
extern const uint8_t pipeline_js_end[]         asm("_binary_eink_pipeline_js_end");
extern const uint8_t vue_js_start[]            asm("_binary_vue_global_prod_js_start");
extern const uint8_t vue_js_end[]              asm("_binary_vue_global_prod_js_end");

static esp_err_t send_embedded(httpd_req_t *req, const uint8_t *start, const uint8_t *end,
                               const char *type, const char *cache)
{
    const size_t len = (size_t)(end - start);
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", cache ? cache : "no-cache");
    /* A failed response must propagate so IDF can retire the bad session. */
    return httpd_resp_send(req, (const char *)start, len);
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP response failed uri=%s fd=%d err=%s", req->uri,
                 httpd_req_to_sockfd(req), esp_err_to_name(err));
    }
    return err;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    return send_embedded(req, index_html_start, index_html_end, "text/html; charset=utf-8", "no-cache");
}

static esp_err_t vue_get_handler(httpd_req_t *req)
{
    return send_embedded(req, vue_js_start, vue_js_end,
                         "application/javascript; charset=utf-8", "public, max-age=86400");
}

static esp_err_t pipeline_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send_chunk(req, (const char *)pipeline_js_start,
                                        pipeline_js_end - pipeline_js_start);
    if (err != ESP_OK) return err;
    char config[64];
    int n = snprintf(config, sizeof(config), ";EinkPipeline.setPanelRotation(%u);", screen_rotation_get());
    if (n < 0 || (size_t)n >= sizeof(config)) return ESP_FAIL;
    err = httpd_resp_send_chunk(req, config, n);
    if (err != ESP_OK) return err;
    return httpd_resp_send_chunk(req, NULL, 0);
}

static void escape_ssid(const char *src, char *dst)
{
    static const char hex[] = "0123456789abcdef";
    while (*src) {
        unsigned char c = (unsigned char)*src++;
        if (c < 32 || c == 34 || c == 92) {
            *dst++ = 92; *dst++ = 'u'; *dst++ = '0'; *dst++ = '0';
            *dst++ = hex[c >> 4]; *dst++ = hex[c & 15];
        } else *dst++ = (char)c;
    }
    *dst = 0;
}

static size_t append_charge_json(char *json, size_t capacity, size_t used);

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char ip[16] = { 0 };
    char ssid[33] = { 0 };
    char escaped_ssid[193];
    char url[48] = { 0 };
    char json[768];
    wifi_mgr_get_ip(ip, sizeof(ip));
    wifi_mgr_get_ssid(ssid, sizeof(ssid));
    wifi_mgr_service_url(url, sizeof(url));
    escape_ssid(ssid, escaped_ssid);
    int n = snprintf(json, sizeof(json),
        "{\"ok\":true,\"format\":\"otp6-v1\",\"w\":480,\"h\":720,\"frame\":172800,"
        "\"mode\":\"%s\",\"ip\":\"%s\",\"ssid\":\"%s\",\"url\":\"%s\"",
        wifi_mgr_is_sta_up() ? "sta" : "ap", ip, escaped_ssid, url);
    if (n < 0 || (size_t)n >= sizeof(json)) return ESP_FAIL;
    size_t used = append_charge_json(json, sizeof(json), (size_t)n);
    if (used + 48 >= sizeof(json)) return ESP_FAIL;
    int tail = snprintf(json + used, sizeof(json) - used,
        ",\"job\":\"%s\",\"job_id\":%u,\"rotation\":%u,\"rotation_epoch\":%u,\"rotation_busy\":%s}",
        job_phase_name(s_job_phase), (unsigned)s_job_id, screen_rotation_get(), (unsigned)screen_rotation_epoch(), screen_rotation_busy() ? "true" : "false");
    if (tail < 0 || (size_t)tail >= sizeof(json) - used) return ESP_FAIL;
    return send_json(req, json);
}




static size_t append_charge_json(char *json, size_t capacity, size_t used)
{
    axp2101_chg_pwr_t pwr = axp2101_get_charge_power();
    bool enabled = false, present = false;
    int soc = -1;
    bool valid = axp2101_gauge_status(&enabled, &present, &soc) == ESP_OK;
    int n = snprintf(json + used, capacity - used,
        ",\"charge\":\"%s\",\"name\":\"%s\",\"capacity_mah\":%u,\"target_ma\":%u,\"icc_ma\":%u,\"vbus_lim_ma\":%u,\"gauge_read_ok\":%s,\"gauge_enabled\":%s,\"battery_present\":%s,\"soc\":%d,\"learning_completion_available\":false",
        axp2101_charge_power_id(pwr), axp2101_charge_power_name(pwr),
        (unsigned)axp2101_capacity_mah(), (unsigned)axp2101_charge_target_ma(pwr),
        (unsigned)axp2101_charge_icc_ma(pwr), (unsigned)axp2101_charge_vbus_lim_ma(pwr),
        valid ? "true" : "false", enabled ? "true" : "false", present ? "true" : "false", soc);
    if (n < 0 || (size_t)n >= capacity - used) return capacity;
    return used + (size_t)n;
}

static void send_charge_json(httpd_req_t *req)
{
    char json[384] = "{\"ok\":true";
    size_t used = append_charge_json(json, sizeof(json), strlen(json));
    if (used + 2 >= sizeof(json)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "charge response overflow");
        return;
    }
    json[used++] = '}';
    json[used] = 0;
    (void)send_json(req, json);
}

static esp_err_t charge_get_handler(httpd_req_t *req)
{
    send_charge_json(req);
    return ESP_OK;
}

static esp_err_t charge_post_handler(httpd_req_t *req)
{
    char body[128];
    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body size");
        return ESP_FAIL;
    }
    size_t used = 0;
    while (used < req->content_len) {
        int n = httpd_req_recv(req, body + used, req->content_len - used);
        if (n <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "incomplete charge request");
            return ESP_FAIL;
        }
        used += (size_t)n;
    }
    body[used] = 0;
    cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "power");
    const cJSON *cap = cJSON_GetObjectItemCaseSensitive(root, "capacity_mah");
    int pwr = -1;
    if (cJSON_IsString(mode)) {
        if (!strcmp(mode->valuestring, "slow") || !strcmp(mode->valuestring, "1w") || !strcmp(mode->valuestring, "1")) pwr = 0;
        if (!strcmp(mode->valuestring, "balanced") || !strcmp(mode->valuestring, "2.5w") || !strcmp(mode->valuestring, "2.5")) pwr = 1;
        if (!strcmp(mode->valuestring, "fast") || !strcmp(mode->valuestring, "5w") || !strcmp(mode->valuestring, "5")) pwr = 2;
    }
    double capacity = cap && cJSON_IsNumber(cap) ? cap->valuedouble : axp2101_capacity_mah();
    bool valid = cJSON_IsObject(root) && pwr >= 0 && (!cap || cJSON_IsNumber(cap)) &&
                 capacity >= 500 && capacity <= 10000 && capacity == (uint16_t)capacity;
    cJSON_Delete(root);
    if (!valid) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "power or capacity_mah (integer 500..10000)");
        return ESP_FAIL;
    }
    esp_err_t err = axp2101_set_charge_config((axp2101_chg_pwr_t)pwr, (uint16_t)capacity);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    send_charge_json(req);
    return ESP_OK;
}

static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    static char json[2048];
    (void)wifi_mgr_scan_json(json, sizeof(json));
    return send_json(req, json);
}

static esp_err_t wifi_connect_handler(httpd_req_t *req)
{
    char body[256];
    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "wifi request size");
        return ESP_FAIL;
    }
    size_t received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "incomplete wifi request");
            return ESP_FAIL;
        }
        received += (size_t)n;
    }
    body[received] = 0;
    cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
    const cJSON *ssid_json = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *pass_json = cJSON_GetObjectItemCaseSensitive(root, "password");
    const bool valid = cJSON_IsObject(root) && cJSON_IsString(ssid_json) &&
        ssid_json->valuestring[0] && strlen(ssid_json->valuestring) <= 32 &&
        (!pass_json || (cJSON_IsString(pass_json) && strlen(pass_json->valuestring) <= 64));
    if (!valid) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid ssid or password");
        return ESP_FAIL;
    }
    char ssid[33], pass[65] = {0};
    memcpy(ssid, ssid_json->valuestring, strlen(ssid_json->valuestring) + 1);
    if (pass_json) memcpy(pass, pass_json->valuestring, strlen(pass_json->valuestring) + 1);
    cJSON_Delete(root);

    esp_err_t err = wifi_mgr_connect(ssid, pass);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    send_json(req, "{\"ok\":true,\"msg\":\"connecting\"}");
    return ESP_OK;
}

/* Validate each six-ink chunk before writing it to controller RAM. */
static esp_err_t receive_frame_chunk(void *context, uint8_t *buffer,
                                     size_t capacity, size_t *received)
{
    frame_rx_t *rx = context;
    *received = 0;
    if (rx->bytes >= SCREEN_FRAME_BYTES) return ESP_ERR_INVALID_SIZE;
    if (xTaskGetTickCount() - rx->started > pdMS_TO_TICKS(60000)) return ESP_ERR_TIMEOUT;
    size_t remaining = SCREEN_FRAME_BYTES - rx->bytes;
    if (capacity > remaining) capacity = remaining;
    int ret = HTTPD_SOCK_ERR_TIMEOUT;
    for (unsigned attempt = 0; attempt < 3 && ret == HTTPD_SOCK_ERR_TIMEOUT; ++attempt) {
        ret = httpd_req_recv(rx->req, (char *)buffer, capacity);
    }
    if (ret == HTTPD_SOCK_ERR_TIMEOUT) return ESP_ERR_TIMEOUT;
    if (ret <= 0) return ESP_FAIL;
    for (int i = 0; i < ret; ++i) {
        uint8_t hi = buffer[i] >> 4, lo = buffer[i] & 15;
        bool valid = ((hi <= 3 || hi == 5 || hi == 6) && (lo <= 3 || lo == 5 || lo == 6));
        if (!valid) return ESP_ERR_INVALID_ARG;
        rx->hash = (rx->hash ^ buffer[i]) * 16777619U;
    }
    (void)screen_rotation_capture_write(rx->bytes, buffer, ret);
    rx->bytes += ret;
    portENTER_CRITICAL(&s_job_lock);
    s_job_bytes = rx->bytes;
    portEXIT_CRITICAL(&s_job_lock);
    if (rx->bytes == SCREEN_FRAME_BYTES) {
        ESP_LOGI(TAG, "frame check id=%u bytes=%u hash=%08x expected=%08x",
                 (unsigned)rx->id, (unsigned)rx->bytes, (unsigned)rx->hash,
                 (unsigned)rx->expected);
        if (rx->hash != rx->expected) return ESP_ERR_INVALID_CRC;
    }
    *received = ret;
    return ESP_OK;
}
static esp_err_t upload_progress(void *context, screen_upload_phase_t phase)
{
    frame_rx_t *rx = context;
    const bool receiving = phase == SCREEN_UPLOAD_RECEIVING;
    if (receiving) rx->started = xTaskGetTickCount();
    portENTER_CRITICAL(&s_job_lock);
    s_job_phase = receiving ? JOB_RECEIVING : JOB_REFRESH;
    s_job_step = receiving ? "receiving" : "refresh";
    portEXIT_CRITICAL(&s_job_lock);
    ESP_LOGI(TAG, "frame id=%u step=%s", (unsigned)rx->id, receiving ? "receiving" : "refresh");
    return ESP_OK;
}

static void frame_job_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const job_kind_t kind = s_job_kind;
        frame_rx_t rx = s_pending;
        screen_upload_report_t report = {0};
        esp_err_t err;
        screen_rotation_lock();
        if (kind == JOB_FRAME) {
            const uint8_t blank[3] = {1,1,1};
            (void)screen_rotation_capture_begin(SCREEN_CACHE_OTP6, blank);
            uint8_t *buffer = malloc(FRAME_CHUNK_BYTES);
            err = !screen_rotation_matches(rx.rotation_epoch) ? ESP_ERR_INVALID_STATE :
                !buffer ? ESP_ERR_NO_MEM : screen_driver_upload(rx.base_generation,
                buffer, FRAME_CHUNK_BYTES, receive_frame_chunk, &rx, upload_progress, &report);
            free(buffer);
            screen_rotation_capture_end(err == ESP_OK, report.display_generation);
            ESP_LOGI(TAG, "frame id=%u end %s bytes=%u",
                     (unsigned)rx.id, esp_err_to_name(err), (unsigned)rx.bytes);
            log_heap_stats("frame-end", rx.id);
            /* Publish the completed baseline before the browser receives its
             * receipt and can immediately verify or reuse that baseline. */
            finish_job(err, &report);
            if (err == ESP_OK) {
                char json[384];
                const int used = snprintf(json, sizeof(json),
                    "{\"ok\":true,\"id\":%u,\"phase\":\"idle\",\"err\":0,\"kind\":\"frame\","
                    "\"total_ms\":%u,\"refresh_ms\":%u,\"display_generation\":%u,",
                    (unsigned)rx.id, (unsigned)report.total_ms,
                    (unsigned)report.refresh_ms, (unsigned)report.display_generation);
                (void)send_report_json(rx.req, json, sizeof(json), used, &report, FRAME_FORMAT);
            } else {
                httpd_resp_send_err(rx.req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
                /* Discard any unread body before the async request releases its socket. */
                shutdown(httpd_req_to_sockfd(rx.req), SHUT_RDWR);
            }
            const esp_err_t complete = httpd_req_async_handler_complete(rx.req);
            if (complete != ESP_OK) ESP_LOGW(TAG, "async complete: %s", esp_err_to_name(complete));
        } else {
            screen_rotation_note_external();
            err = screen_driver_clear(&report);
            if (err == ESP_OK) screen_rotation_note_blank();
            finish_job(err, &report);
        }
        screen_rotation_unlock();
        ESP_LOGI(TAG, "job %s", err == ESP_OK ? "idle" : "error");
    }
}
static esp_err_t job_get_handler(httpd_req_t *req)
{
    return send_job_json(req);
}
static bool hex_header(httpd_req_t *req, const char *key, uint32_t *out)
{
    char value[9];
    if (httpd_req_get_hdr_value_str(req, key, value, sizeof(value)) != ESP_OK ||
        strlen(value) != 8 || strspn(value, "0123456789abcdefABCDEF") != 8) return false;
    *out = (uint32_t)strtoul(value, NULL, 16);
    return true;
}

static bool decimal_header(httpd_req_t *req, const char *key, uint32_t *out)
{
    char text[11];
    if (httpd_req_get_hdr_value_str(req, key, text, sizeof(text)) != ESP_OK || !text[0] ||
        strspn(text, "0123456789") != strlen(text)) return false;
    uint64_t value = strtoull(text, NULL, 10);
    if (value > UINT32_MAX) return false;
    *out = (uint32_t)value;
    return true;
}

static esp_err_t frame_post_handler(httpd_req_t *req)
{
    char format[16], pass[8], quality[12];
    frame_rx_t rx = { .hash = 2166136261U };
    if (httpd_req_get_hdr_value_str(req, "X-Eink-Format", format, sizeof(format)) != ESP_OK ||
        strcmp(format, FRAME_FORMAT) != 0 ||
        httpd_req_get_hdr_value_str(req, "X-Eink-Pass", pass, sizeof(pass)) != ESP_OK ||
        strcmp(pass, "0") != 0 ||
        httpd_req_get_hdr_value_str(req, "X-Eink-Quality", quality, sizeof(quality)) != ESP_OK ||
        (strcmp(quality, "fast") != 0 && strcmp(quality, "standard") != 0 && strcmp(quality, "best") != 0) ||
        !hex_header(req, "X-Eink-Checksum", &rx.expected)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "frame contract mismatch; reload page");
        return ESP_FAIL;
    }
    if (req->content_len != SCREEN_FRAME_BYTES) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected 172800 bytes");
        return ESP_FAIL;
    }
    if (httpd_req_get_hdr_value_len(req, "X-Eink-Base-Generation") &&
        (!decimal_header(req, "X-Eink-Base-Generation", &rx.base_generation) ||
         !rx.base_generation || rx.base_generation != screen_driver_generation())) {
        httpd_resp_set_status(req, "409 Conflict");
        send_json(req, "{\"ok\":false,\"error\":\"region baseline changed; send a full image\"}");
        return ESP_FAIL;
    }
    if (!decimal_header(req, "X-Eink-Rotation-Epoch", &rx.rotation_epoch) ||
        !screen_rotation_matches(rx.rotation_epoch) || !begin_job(JOB_FRAME)) {
        httpd_resp_set_status(req, "409 Conflict");
        send_json(req, "{\"ok\":false,\"error\":\"screen busy\"}");
        return ESP_FAIL;
    }
    esp_err_t err = httpd_req_async_handler_begin(req, &rx.req);
    if (err != ESP_OK) {
        const screen_upload_report_t empty = {0};
        finish_job(err, &empty);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    rx.id = s_job_id;
    s_pending = rx;
    ESP_LOGI(TAG, "frame id=%u format=%s quality=%s bytes=%u",
             (unsigned)rx.id, format, quality, (unsigned)req->content_len);
    log_heap_stats("frame-start", rx.id);
    /* The worker owns first byte through POF and response; HTTP stays available. */
    xTaskNotifyGive(s_job_task);
    return ESP_OK;
}

static esp_err_t panel_get_handler(httpd_req_t *req)
{
    return send_json(req, "{\"ok\":true,\"controller\":\"EPD037A02\","
        "\"image_formats\":[\"otp6-v1\"],\"colors\":6,\"full_refresh\":true}");
}

static esp_err_t clear_post_handler(httpd_req_t *req)
{
    if (req->content_len != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected empty body");
        return ESP_FAIL;
    }
    if (!begin_job(JOB_CLEAR_SCREEN)) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, "{\"ok\":false,\"error\":\"screen busy\"}");
    }
    portENTER_CRITICAL(&s_job_lock);
    s_job_phase = JOB_REFRESH;
    s_job_step = "clear";
    portEXIT_CRITICAL(&s_job_lock);
    xTaskNotifyGive(s_job_task);
    httpd_resp_set_status(req, "202 Accepted");
    return send_job_json(req);
}

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 12;
    config.stack_size = 12288;
    config.max_open_sockets = 7;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 8;
    config.lru_purge_enable = true;

    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);

    if (s_job_task == NULL &&
        xTaskCreate(frame_job_task, "eink_job", 4096, NULL, 5, &s_job_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "httpd_start");

    const httpd_uri_t handlers[] = {
        { .uri = "/", .method = HTTP_GET, .handler = root_get_handler },
        { .uri = "/vue.global.prod.js", .method = HTTP_GET, .handler = vue_get_handler },
        { .uri = "/eink_pipeline.js", .method = HTTP_GET, .handler = pipeline_get_handler },
        { .uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler },
        { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_handler },
        { .uri = "/api/wifi/connect", .method = HTTP_POST, .handler = wifi_connect_handler },
        { .uri = "/api/charge", .method = HTTP_GET, .handler = charge_get_handler },
        { .uri = "/api/charge", .method = HTTP_POST, .handler = charge_post_handler },
        { .uri = "/api/frame", .method = HTTP_POST, .handler = frame_post_handler },
        { .uri = "/api/job", .method = HTTP_GET, .handler = job_get_handler },
        { .uri = "/api/panel", .method = HTTP_GET, .handler = panel_get_handler },
        { .uri = "/api/clear", .method = HTTP_POST, .handler = clear_post_handler },
    };
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &handlers[i]), TAG, "register URI");
    }

    ESP_LOGI(TAG, "HTTP ready on :80");
    return ESP_OK;
}
