#ifndef SCREEN_ROTATION_H
#define SCREEN_ROTATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    SCREEN_CACHE_OTP6, SCREEN_CACHE_CQ64, SCREEN_CACHE_CQ256,
    SCREEN_CACHE_CQ256_V2, SCREEN_CACHE_OTP384, SCREEN_CACHE_FAST64,
    SCREEN_CACHE_512_QUAL, SCREEN_CACHE_512_FAST,
} screen_cache_kind_t;

/* Lock order: rotation -> display -> board SPI. Never take rotation from a
 * display read/progress callback. HTTP and local UI own this lock per job. */
esp_err_t screen_rotation_init(void);
void screen_rotation_lock(void);
void screen_rotation_unlock(void);
unsigned screen_rotation_get(void);
uint32_t screen_rotation_epoch(void);
bool screen_rotation_busy(void);
bool screen_rotation_matches(uint32_t epoch);
void screen_rotation_size(unsigned rotation, int *width, int *height);
void screen_rotation_source_xy(unsigned rotation, int px, int py, int *x, int *y);
void screen_rotation_panel_xy(unsigned rotation, int x, int y, int *px, int *py);

/* Call with rotation lock held. The cache uses two disposable Flash slots,
 * valid only after a verified complete display job in this boot. */
esp_err_t screen_rotation_capture_begin(screen_cache_kind_t kind, const uint8_t blank[3]);
uint32_t screen_rotation_source_generation(void);
esp_err_t screen_rotation_window_begin(uint16_t x, uint16_t y, uint16_t width,
    uint16_t height, uint32_t display_generation, const uint8_t blank[3]);
esp_err_t screen_rotation_capture_write(size_t offset, const uint8_t *data, size_t length);
void screen_rotation_capture_end(bool success, uint32_t display_generation);
void screen_rotation_invalidate(void);
void screen_rotation_note_ui(void);
void screen_rotation_note_external(void);
void screen_rotation_note_blank(void);
bool screen_rotation_hidden(void);
esp_err_t screen_rotation_toggle_blank(void);

#endif
