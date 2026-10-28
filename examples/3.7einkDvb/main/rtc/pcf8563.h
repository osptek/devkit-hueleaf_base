#ifndef PCF8563_H
#define PCF8563_H

#include "esp_err.h"
#include <stdint.h>

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t weekday;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} pcf8563_time_t;

esp_err_t pcf8563_init(void);
esp_err_t pcf8563_set_time(const pcf8563_time_t *time);
esp_err_t pcf8563_get_time(pcf8563_time_t *time);

#endif /* PCF8563_H */
