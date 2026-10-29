#ifndef SCREEN_UI_H
#define SCREEN_UI_H

#include "esp_err.h"

esp_err_t screen_ui_show_link_qr(const char *title, const char *url);
/* Offline provisioning page: text and address, without QR generation. */
esp_err_t screen_ui_show_setup(const char *url);

esp_err_t screen_ui_redraw_rotation(unsigned rotation);
#endif /* SCREEN_UI_H */
