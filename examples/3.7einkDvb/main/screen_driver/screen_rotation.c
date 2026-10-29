#include "screen_rotation.h"
#include "pin_map.h"
#include "screen_driver.h"
#include "screen_ui.h"

#include "web_server.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "rotation";
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_worker;
static unsigned s_rotation = EINK_ROTATION;
static uint32_t s_epoch = 1, s_pending;
static bool s_working, s_ui_current;
static const esp_partition_t *s_partition;

#define CACHE_SLOT_BYTES (512U * 1024U)
#define CACHE_SECTOR_BYTES 4096U
#define CACHE_PATCH_LIMIT 16U
typedef struct {
    uint16_t source, gate, sources, gates;
    size_t offset, bytes;
} cache_patch_t;
typedef enum { CAP_NONE, CAP_FULL, CAP_PATCH, CAP_RMW } capture_mode_t;
typedef struct {
    screen_cache_kind_t kind;
    unsigned rotation, planes;
    uint8_t blank[3];
    size_t base, bytes, tail;
    uint32_t generation, source_generation;
    bool valid;

    unsigned patch_count;
    capture_mode_t capturing;
    size_t written, capture_bytes;
    cache_patch_t pending;
    uint8_t *scratch;
    size_t scratch_sector;
    bool scratch_dirty;
} cache_t;
static cache_t s_cache;
static bool s_blank_current;
static bool s_hidden;
static uint32_t s_hidden_generation;
static uint32_t s_source_generation;

static size_t sector_size(size_t n) { return (n + CACHE_SECTOR_BYTES - 1U) & ~(CACHE_SECTOR_BYTES - 1U); }

unsigned screen_rotation_get(void) { return __atomic_load_n(&s_rotation, __ATOMIC_ACQUIRE); }
uint32_t screen_rotation_epoch(void) { return __atomic_load_n(&s_epoch, __ATOMIC_ACQUIRE); }
bool screen_rotation_busy(void)
{
    return __atomic_load_n(&s_working, __ATOMIC_ACQUIRE) ||
           __atomic_load_n(&s_pending, __ATOMIC_ACQUIRE) != 0;
}
bool screen_rotation_matches(uint32_t epoch) { return epoch == screen_rotation_epoch(); }
void screen_rotation_lock(void) { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
void screen_rotation_unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }
void screen_rotation_size(unsigned r, int *w, int *h)
{
    *w = r == 90 || r == 270 ? SCREEN_HEIGHT : SCREEN_WIDTH;
    *h = r == 90 || r == 270 ? SCREEN_WIDTH : SCREEN_HEIGHT;
}
void screen_rotation_source_xy(unsigned r, int px, int py, int *x, int *y)
{
    switch (r) {
    case 90: *x = py; *y = SCREEN_WIDTH - 1 - px; break;
    case 180: *x = SCREEN_WIDTH - 1 - px; *y = SCREEN_HEIGHT - 1 - py; break;
    case 270: *x = SCREEN_HEIGHT - 1 - py; *y = px; break;
    default: *x = px; *y = py; break;
    }
}
void screen_rotation_panel_xy(unsigned r, int x, int y, int *px, int *py)
{
    switch (r) {
    case 90: *px = SCREEN_WIDTH - 1 - y; *py = x; break;
    case 180: *px = SCREEN_WIDTH - 1 - x; *py = SCREEN_HEIGHT - 1 - y; break;
    case 270: *px = y; *py = SCREEN_HEIGHT - 1 - x; break;
    default: *px = x; *py = y; break;
    }
}
uint32_t screen_rotation_source_generation(void)
{
    return __atomic_load_n(&s_source_generation, __ATOMIC_ACQUIRE);
}
void screen_rotation_invalidate(void)
{
    s_hidden = false;
    s_cache.valid = false; s_cache.capturing = CAP_NONE;
    __atomic_store_n(&s_source_generation, 0, __ATOMIC_RELEASE);
    free(s_cache.scratch); s_cache.scratch = NULL; s_cache.scratch_dirty = false;
}
void screen_rotation_note_external(void)
{
    screen_rotation_invalidate(); s_ui_current = false; s_blank_current = false;
}
void screen_rotation_note_ui(void)
{
    screen_rotation_invalidate(); s_ui_current = true; s_blank_current = false;
}
void screen_rotation_note_blank(void)
{
    screen_rotation_invalidate(); s_ui_current = false; s_blank_current = true;
}

esp_err_t screen_rotation_capture_begin(screen_cache_kind_t kind, const uint8_t blank[3])
{
    screen_rotation_note_external();
    if (!s_partition) return ESP_ERR_NOT_FOUND;
    if (!blank) return ESP_ERR_INVALID_ARG;
    unsigned planes = kind == SCREEN_CACHE_OTP6 ? 1 :
        kind == SCREEN_CACHE_CQ64 || kind == SCREEN_CACHE_FAST64 ? 2 : 3;
    const size_t bytes = planes * SCREEN_FRAME_BYTES;
    const size_t erase = sector_size(bytes);
    if (erase > CACHE_SLOT_BYTES) return ESP_ERR_INVALID_SIZE;
    const size_t base = s_cache.base == 0 ? CACHE_SLOT_BYTES : 0;
    esp_err_t err = esp_partition_erase_range(s_partition, base, erase);
    if (err != ESP_OK) return err;
    s_cache = (cache_t){.kind = kind, .rotation = screen_rotation_get(),
        .planes = planes, .base = base, .bytes = bytes, .tail = erase,
        .capturing = CAP_FULL, .capture_bytes = bytes};
    memcpy(s_cache.blank, blank, sizeof(s_cache.blank));
    return ESP_OK;
}

esp_err_t screen_rotation_capture_write(size_t offset, const uint8_t *data, size_t length)
{
    if (s_cache.capturing == CAP_NONE) return ESP_ERR_INVALID_STATE;
    if (offset >= s_cache.capture_bytes) return ESP_OK;
    if (offset != s_cache.written || !data) { screen_rotation_invalidate(); return ESP_ERR_INVALID_SIZE; }
    if (length > s_cache.capture_bytes - offset) length = s_cache.capture_bytes - offset;
    esp_err_t err;
    {
        const size_t start = s_cache.capturing == CAP_PATCH ? s_cache.pending.offset : 0;
        err = esp_partition_write(s_partition, s_cache.base + start + offset, data, length);
    }
    if (err != ESP_OK) {
        screen_rotation_invalidate();
        ESP_LOGW(TAG, "image cache write disabled: %s", esp_err_to_name(err));
        return err;
    }
    s_cache.written += length;
    return ESP_OK;
}
void screen_rotation_capture_end(bool success, uint32_t generation)
{
    bool valid = success && s_cache.capturing != CAP_NONE && s_cache.written == s_cache.capture_bytes;

    free(s_cache.scratch); s_cache.scratch = NULL; s_cache.scratch_dirty = false;
    s_cache.valid = valid; s_cache.capturing = CAP_NONE;
    s_cache.generation = generation; s_cache.source_generation = valid ? generation : 0;
    __atomic_store_n(&s_source_generation, valid ? generation : 0, __ATOMIC_RELEASE);
    if (valid) ESP_LOGI(TAG, "image cache ready bytes=%u patches=%u source_rotation=%u",
        (unsigned)s_cache.bytes, s_cache.patch_count, s_cache.rotation);
}

typedef struct {
    const uint8_t *data;
    unsigned target, plane;
    size_t sent;
    int sw, sh, dw, dh, width, height, left, top;
} replay_t;
static uint8_t replay_pixel(const replay_t *r, int px, int py)
{
    int x, y;
    screen_rotation_source_xy(r->target, px, py, &x, &y);
    if (x < r->left || y < r->top || x >= r->left + r->width || y >= r->top + r->height)
        return s_cache.blank[r->plane];
    const int sx = (x - r->left) * r->sw / r->width;
    const int sy = (y - r->top) * r->sh / r->height;
    screen_rotation_panel_xy(s_cache.rotation, sx, sy, &px, &py);
    const unsigned gate = SCREEN_WIDTH - 1U - px;
    const size_t index = r->plane * SCREEN_FRAME_BYTES +
        gate * (SCREEN_HEIGHT / 2) + py / 2;
    const uint8_t pair = r->data[index];
    return py & 1 ? pair & 15 : pair >> 4;
}
static esp_err_t replay_read(void *context, uint8_t *buffer, size_t capacity, size_t *received)
{
    replay_t *r = context;
    size_t n = SCREEN_FRAME_BYTES - r->sent;
    if (n > capacity) n = capacity;
    for (size_t i = 0; i < n; ++i) {
        const size_t pos = r->sent + i;
        const int px = SCREEN_WIDTH - 1 - (int)(pos / (SCREEN_HEIGHT / 2));
        const int py = (int)(pos % (SCREEN_HEIGHT / 2)) * 2;
        buffer[i] = (uint8_t)((replay_pixel(r, px, py) << 4) | replay_pixel(r, px, py + 1));
    }
    r->sent += n;
    *received = n;
    return ESP_OK;
}
static void replay_setup(replay_t *r, const uint8_t *mapped, unsigned target)
{
    *r = (replay_t){.data = mapped, .target = target};
    screen_rotation_size(s_cache.rotation, &r->sw, &r->sh);
    screen_rotation_size(target, &r->dw, &r->dh);
    if (r->dw * r->sh <= r->dh * r->sw) { r->width = r->dw; r->height = r->sh * r->dw / r->sw; }
    else { r->height = r->dh; r->width = r->sw * r->dh / r->sh; }
    r->left = (r->dw - r->width) / 2; r->top = (r->dh - r->height) / 2;
}
static esp_err_t replay_progress(void *context, screen_upload_phase_t phase)
{
    (void)context;
    (void)phase;
    return ESP_OK;
}

static esp_err_t replay_image(unsigned target)
{
    const void *mapped = NULL;
    esp_partition_mmap_handle_t handle;
    esp_err_t err = esp_partition_mmap(s_partition, s_cache.base, s_cache.tail,
        ESP_PARTITION_MMAP_DATA, &mapped, &handle);
    if (err != ESP_OK) return err;
    replay_t r;
    replay_setup(&r, mapped, target);
    uint8_t buffer[1024];
    screen_upload_report_t report = {0};
    err = screen_driver_upload(0, buffer, sizeof(buffer), replay_read, &r, replay_progress, &report);
    esp_partition_munmap(handle);
    if (err == ESP_OK) s_cache.generation = report.display_generation;
    else screen_rotation_invalidate();
    return err;
}
bool screen_rotation_hidden(void)
{
    return s_hidden && s_hidden_generation == screen_driver_generation();
}

esp_err_t screen_rotation_toggle_blank(void)
{
    if (screen_rotation_hidden()) {
        esp_err_t err = s_ui_current ? screen_ui_redraw_rotation(screen_rotation_get()) :
            s_cache.valid ? replay_image(screen_rotation_get()) : ESP_ERR_INVALID_STATE;
        if (err == ESP_OK) s_hidden = false;
        return err;
    }
    s_hidden = false;
    if (!s_ui_current && (!s_cache.valid || s_cache.generation != screen_driver_generation()))
        return ESP_ERR_INVALID_STATE;
    screen_upload_report_t report = {0};
    esp_err_t err = screen_driver_clear(&report);
    if (err == ESP_OK) {
        s_hidden = true;
        s_hidden_generation = screen_driver_generation();
        if (s_cache.valid) s_cache.generation = s_hidden_generation;
    }
    return err;
}

static void rotation_worker(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (__atomic_load_n(&s_pending, __ATOMIC_ACQUIRE)) {
            /* Leave an accepted upload its entire session, including gaps
             * between HTTP requests, before applying queued button presses. */
            if (web_server_screen_busy()) { vTaskDelay(pdMS_TO_TICKS(25)); continue; }
            __atomic_store_n(&s_working, true, __ATOMIC_RELEASE);
            screen_rotation_lock();
            if (web_server_screen_busy()) {
                screen_rotation_unlock();
                __atomic_store_n(&s_working, false, __ATOMIC_RELEASE);
                vTaskDelay(pdMS_TO_TICKS(25)); continue;
            }
            const unsigned steps = __atomic_exchange_n(&s_pending, 0, __ATOMIC_ACQ_REL) & 3U;
            const unsigned target = (screen_rotation_get() + steps * 90U) % 360U;
            ESP_LOGI(TAG, "GPIO28 redraw steps=%u target=%u", steps, target);
            esp_err_t err = ESP_OK;
            if (steps) {
                if (screen_rotation_hidden()) err = ESP_OK;
                else if (s_cache.valid && s_cache.generation == screen_driver_generation()) err = replay_image(target);
                
                else if (s_ui_current) err = screen_ui_redraw_rotation(target);
                else if (s_blank_current) err = ESP_OK;
                else { err = ESP_ERR_INVALID_STATE; ESP_LOGE(TAG, "no current image cache; flash partition table and upload again"); }
                if (err == ESP_OK) {
                    __atomic_store_n(&s_rotation, target, __ATOMIC_RELEASE);
                    if (__atomic_add_fetch(&s_epoch, 1, __ATOMIC_ACQ_REL) == 0)
                        (void)__atomic_add_fetch(&s_epoch, 1, __ATOMIC_ACQ_REL);
                }
                ESP_LOGI(TAG, "button rotate target=%u applied=%u epoch=%u err=%s", target,
                    screen_rotation_get(), (unsigned)screen_rotation_epoch(), esp_err_to_name(err));
            }
            screen_rotation_unlock();
            __atomic_store_n(&s_working, false, __ATOMIC_RELEASE);
        }
    }
}
static void rotation_button(void *arg)
{
    (void)arg;
    int raw = gpio_get_level(EINK_ROTATE_BUTTON), stable = raw;
    TickType_t changed = xTaskGetTickCount();
    bool armed = false; /* Ignore a BOOT button still held during power-on. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10));
        const int value = gpio_get_level(EINK_ROTATE_BUTTON);
        const TickType_t now = xTaskGetTickCount();
        if (value != raw) { raw = value; changed = now; }
        if ((TickType_t)(now - changed) < pdMS_TO_TICKS(30)) continue;
        if (raw != EINK_ROTATE_BUTTON_ACTIVE_LEVEL) armed = true;
        if (raw == stable) continue;
        stable = raw;
        if (stable == EINK_ROTATE_BUTTON_ACTIVE_LEVEL && armed) {
            armed = false; /* Keep libc/logging off the small GPIO polling stack. */
            (void)__atomic_add_fetch(&s_pending, 1, __ATOMIC_ACQ_REL);
            xTaskNotifyGive(s_worker);
        }
    }
}
esp_err_t screen_rotation_init(void)
{
    if (s_lock) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    s_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "image_cache");
    if (!s_partition || s_partition->size < 2U * CACHE_SLOT_BYTES) {
        s_partition = NULL;
        ESP_LOGW(TAG, "image_cache partition unavailable; flash the updated partition table");
    }
    gpio_config_t cfg = {.pin_bit_mask = 1ULL << EINK_ROTATE_BUTTON, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE};
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) { vSemaphoreDelete(s_lock); s_lock = NULL; return err; }
    if (xTaskCreate(rotation_worker, "rotate_ui", 4096, NULL, 3, &s_worker) != pdPASS) {
        vSemaphoreDelete(s_lock); s_lock = NULL; return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(rotation_button, "rotate_key", 2048, NULL, 2, NULL) != pdPASS) {
        vTaskDelete(s_worker); s_worker = NULL; vSemaphoreDelete(s_lock); s_lock = NULL; return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "GPIO%d rotate button ready, default=%u cache=%s",
        (int)EINK_ROTATE_BUTTON, screen_rotation_get(), s_partition ? "flash" : "unavailable");
    return ESP_OK;
}
