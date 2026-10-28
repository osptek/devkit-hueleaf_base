#ifndef WIFI_AP_H
#define WIFI_AP_H

#include "esp_err.h"

#define WIFI_AP_SSID    "Eink E6 3.7"
#define WIFI_AP_CHANNEL 36

esp_err_t wifi_ap_start(void);

#endif /* WIFI_AP_H */
