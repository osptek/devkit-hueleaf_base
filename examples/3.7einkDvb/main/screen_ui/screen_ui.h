#ifndef SCREEN_UI_H
#define SCREEN_UI_H

#include "esp_err.h"

esp_err_t screen_ui_show_link_qr(const char *title, const char *url);

#endif /* SCREEN_UI_H */
