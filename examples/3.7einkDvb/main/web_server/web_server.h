#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"
#include <stdbool.h>
bool web_server_screen_busy(void);

esp_err_t web_server_start(void);

#endif /* WEB_SERVER_H */
