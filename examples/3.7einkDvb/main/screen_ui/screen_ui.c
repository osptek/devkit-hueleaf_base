#include "screen_ui.h"
#include "pin_map.h"
#include "screen_rotation.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "qrcode.h"
#include "screen_driver.h"

static const char *TAG = "screen_ui";

#define FONT_W  5
#define FONT_H  7
/* 5x7 列优先，bit0=顶行。覆盖 URL 常用字符。 */
static const uint8_t k_font[96][5] = {
    {0,0,0,0,0}, {0,0,0x5F,0,0}, {0,7,0,7,0}, {0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62}, {0x36,0x49,0x55,0x22,0x50},
    {0,5,3,0,0}, {0,0x1C,0x22,0x41,0}, {0,0x41,0x22,0x1C,0}, {0x14,8,0x3E,8,0x14},
    {8,8,0x3E,8,8}, {0,0x50,0x30,0,0}, {8,8,8,8,8}, {0,0x60,0x60,0,0},
    {0x20,0x10,8,4,2}, {0x3E,0x51,0x49,0x45,0x3E}, {0,0x42,0x7F,0x40,0},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31}, {0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {1,0x71,9,5,3},
    {0x36,0x49,0x49,0x49,0x36}, {6,0x49,0x49,0x29,0x1E}, {0,0x36,0x36,0,0},
    {0,0x56,0x36,0,0}, {8,0x14,0x22,0x41,0}, {0x14,0x14,0x14,0x14,0x14},
    {0,0x41,0x22,0x14,8}, {2,1,0x51,9,6}, {0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,9,9,9,1},
    {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,8,8,8,0x7F}, {0,0x41,0x7F,0x41,0},
    {0x20,0x40,0x41,0x3F,1}, {0x7F,8,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,2,0x0C,2,0x7F}, {0x7F,4,8,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,9,9,9,6}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,9,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {1,1,0x7F,1,1}, {0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,8,0x14,0x63},
    {7,8,0x70,8,7}, {0x61,0x51,0x49,0x45,0x43}, {0,0x7F,0x41,0x41,0},
    {2,4,8,0x10,0x20}, {0,0x41,0x41,0x7F,0}, {4,2,1,2,4}, {0x40,0x40,0x40,0x40,0x40},
    {0,1,2,4,0}, {0x20,0x54,0x54,0x54,0x78}, {0x7F,0x48,0x44,0x44,0x38},
    {0x38,0x44,0x44,0x44,0x20}, {0x38,0x44,0x44,0x48,0x7F}, {0x38,0x54,0x54,0x54,0x18},
    {8,0x7E,9,1,2}, {0x0C,0x52,0x52,0x52,0x3E}, {0x7F,8,4,4,0x78},
    {0,0x44,0x7D,0x40,0}, {0x20,0x40,0x44,0x3D,0}, {0x7F,0x10,0x28,0x44,0},
    {0,0x41,0x7F,0x40,0}, {0x7C,4,0x18,4,0x78}, {0x7C,8,4,4,0x78},
    {0x38,0x44,0x44,0x44,0x38}, {0x7C,0x14,0x14,0x14,8}, {8,0x14,0x14,0x18,0x7C},
    {0x7C,8,4,4,8}, {0x48,0x54,0x54,0x54,0x20}, {4,0x3F,0x44,0x40,0x20},
    {0x3C,0x40,0x40,0x20,0x7C}, {0x1C,0x20,0x40,0x20,0x1C}, {0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C}, {0x44,0x64,0x54,0x4C,0x44},
    {0,8,0x36,0x41,0}, {0,0,0x7F,0,0}, {0,0x41,0x36,8,0}, {8,4,8,0x10,8}, {0,0,0,0,0},
};

typedef struct {
    uint8_t modules[QR_MAX_SIZE * QR_MAX_SIZE];
    int qr_size;
    int scale;
    int origin_x;
    int origin_y;
    const char *title;
    const char *url;
    size_t sent;
    int last_x;
    uint8_t col[SCREEN_HEIGHT];
} qr_stream_t;

/* Setup text and LAN QR share the same column buffer. Only the UI task uses it. */
static qr_stream_t s_stream;
static int ui_width=SCREEN_WIDTH, ui_height=SCREEN_HEIGHT;
static unsigned ui_rotation;
static char saved_title[48], saved_url[96];

static int font_index(char c)
{
    if (c < 32 || c > 126) {
        return 0;
    }
    return c - 32;
}

static uint8_t glyph_pixel(char c, int gx, int gy)
{
    if (gx < 0 || gx >= FONT_W || gy < 0 || gy >= FONT_H) {
        return 0;
    }
    return (k_font[font_index(c)][gx] >> gy) & 1;
}

static void draw_text_row(uint8_t *col, int x, int y, const char *text, int scale)
{
    int len = (int)strlen(text);
    int tw = len * (FONT_W + 1) * scale - scale;
    int start_x = (ui_width - tw) / 2;
    if (x < start_x || x >= start_x + tw) {
        return;
    }
    int local = x - start_x;
    int cell = FONT_W + 1;
    int ci = local / (cell * scale);
    int ox = (local / scale) % cell;
    if (ci < 0 || ci >= len || ox >= FONT_W) {
        return;
    }
    for (int gy = 0; gy < FONT_H; gy++) {
        if (!glyph_pixel(text[ci], ox, gy)) {
            continue;
        }
        for (int s = 0; s < scale; s++) {
            int yy = y + gy * scale + s;
            if (yy >= 0 && yy < ui_height) {
                col[yy] = SCREEN_COLOR_BLACK;
            }
        }
    }
}

static uint8_t pixel_at(const qr_stream_t *s, int x, int y)
{
    int qr_pix = s->qr_size * s->scale;
    if (x >= s->origin_x && x < s->origin_x + qr_pix &&
        y >= s->origin_y && y < s->origin_y + qr_pix) {
        int mx = (x - s->origin_x) / s->scale;
        int my = (y - s->origin_y) / s->scale;
        if (s->modules[my * s->qr_size + mx]) {
            return SCREEN_COLOR_BLACK;
        }
    }
    return SCREEN_COLOR_WHITE;
}

static void paint_column(qr_stream_t *s, int x)
{
    memset(s->col, SCREEN_COLOR_WHITE, SCREEN_HEIGHT);
    if (s->qr_size == 0) {
        draw_text_row(s->col, x, 240, "ready to setup", 4);
        draw_text_row(s->col, x, 340, "WiFi AP OPEN", 4);
        draw_text_row(s->col, x, 400, s->url, 3);
        return;
    }
    for (int y = 0; y < ui_height; y++) {
        s->col[y] = pixel_at(s, x, y);
    }
    if (s->title) {
        draw_text_row(s->col, x, 36, s->title, 3);
    }
    if (s->url) {
        draw_text_row(s->col, x, ui_height - 70, s->url, 2);
    }
}

static uint8_t qr_pair_byte(qr_stream_t *s, size_t pixel_index)
{
    int x = (int)(pixel_index / (SCREEN_HEIGHT / 2));
    int byte_in_col = (int)(pixel_index % (SCREEN_HEIGHT / 2));
    int y0 = byte_in_col * 2;
    /* 与网页 packIdsMirrored 一致：列优先写入时对 X 做镜像 */
    int xr, yr;
    screen_rotation_source_xy(ui_rotation, SCREEN_WIDTH-1-x, y0, &xr, &yr);
    if(xr!=s->last_x){paint_column(s,xr);s->last_x=xr;}
    uint8_t hi=s->col[yr];
    screen_rotation_source_xy(ui_rotation, SCREEN_WIDTH-1-x, y0+1, &xr, &yr);
    if(xr!=s->last_x){paint_column(s,xr);s->last_x=xr;}
    return (uint8_t)((hi<<4)|s->col[yr]);
}

static esp_err_t qr_read_cb(void *context, uint8_t *buffer, size_t capacity, size_t *received)
{
    qr_stream_t *s = context;
    size_t need = SCREEN_FRAME_BYTES - s->sent;
    size_t n = need > capacity ? capacity : need;
    for (size_t i = 0; i < n; ++i) {
        buffer[i] = qr_pair_byte(s, s->sent + i);
    }

    s->sent += n;
    *received = n;
    return ESP_OK;
}

esp_err_t screen_ui_show_link_qr(const char *title, const char *url)
{
    int qr_size = 0;

    if (url == NULL || url[0] == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ui_rotation = screen_rotation_get();
    screen_rotation_size(ui_rotation, &ui_width, &ui_height);
    memset(&s_stream, 0, sizeof(s_stream));
    if (!qrcode_encode(url, s_stream.modules, (int)sizeof(s_stream.modules), &qr_size)) {
        ESP_LOGE(TAG, "QR encode failed for %s", url);
        return ESP_FAIL;
    }

    int quiet = qr_size + 8;
    int scale = (ui_width < ui_height ? ui_width : ui_height) / quiet;
    if (scale < 2) {
        scale = 2;
    }
    if (scale > 12) {
        scale = 12;
    }

    s_stream.qr_size = qr_size;
    s_stream.scale = scale;
    s_stream.origin_x = (ui_width - qr_size * scale) / 2;
    s_stream.origin_y = (ui_height - qr_size * scale) / 2 - 10;
    snprintf(saved_title, sizeof(saved_title), "%s", title ? title : "");
    s_stream.title = saved_title;
    snprintf(saved_url, sizeof(saved_url), "%s", url);
    s_stream.url = saved_url;
    s_stream.sent = 0;
    s_stream.last_x = -1;

    ESP_LOGI(TAG, "QR %s  modules=%d scale=%d", url, qr_size, scale);
    screen_rotation_note_external();
    esp_err_t err = screen_driver_show_stream(qr_read_cb, &s_stream);

    if (err == ESP_OK) screen_rotation_note_ui();
    return err;
}

esp_err_t screen_ui_show_setup(const char *url)
{
    if (url == NULL || url[0] == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ui_rotation = screen_rotation_get();
    screen_rotation_size(ui_rotation, &ui_width, &ui_height);
    memset(&s_stream, 0, sizeof(s_stream));
    snprintf(saved_url, sizeof(saved_url), "%s", url);
    s_stream.url = saved_url;
    s_stream.last_x = -1;
    ESP_LOGI(TAG, "setup text %s", url);
    screen_rotation_note_external();
    esp_err_t err = screen_driver_show_stream(qr_read_cb, &s_stream);

    if (err == ESP_OK) screen_rotation_note_ui();
    return err;
}

esp_err_t screen_ui_redraw_rotation(unsigned rotation)
{
    ui_rotation = rotation;
    screen_rotation_size(rotation, &ui_width, &ui_height);
    s_stream.origin_x = (ui_width - s_stream.qr_size * s_stream.scale) / 2;
    s_stream.origin_y = (ui_height - s_stream.qr_size * s_stream.scale) / 2 - 10;
    s_stream.sent = 0;
    s_stream.last_x = -1;
    return screen_driver_show_stream(qr_read_cb, &s_stream);
}
