#ifndef AXP2101_H
#define AXP2101_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    AXP2101_CHG_PWR_1W = 0,   /* 涓流0.2C；旧枚举名仅为兼容 */
    AXP2101_CHG_PWR_2W5 = 1,  /* 正常0.5C */
    AXP2101_CHG_PWR_5W = 2,   /* 快速1.0C */
} axp2101_chg_pwr_t;

esp_err_t axp2101_init(void);
/* Arm PWRON short-press IRQ, discarding events latched before startup. */
esp_err_t axp2101_pwron_init(void);
/* Task context: read/log/acknowledge the three IRQ status bytes (W1C), so nIRQ
 * releases. Only INTSTS2[3] reports a short press. A true result remains valid
 * even if acknowledging another IRQ source subsequently returns an error. */
esp_err_t axp2101_take_pwron_short_press(bool *pressed);
esp_err_t axp2101_enable_peripheral_rails(void);
esp_err_t axp2101_configure_charge(void);
esp_err_t axp2101_set_charge_power(axp2101_chg_pwr_t pwr);
axp2101_chg_pwr_t axp2101_get_charge_power(void);
const char *axp2101_charge_power_id(axp2101_chg_pwr_t pwr);
const char *axp2101_charge_power_name(axp2101_chg_pwr_t pwr);
uint16_t axp2101_charge_icc_ma(axp2101_chg_pwr_t pwr);
uint16_t axp2101_charge_vbus_lim_ma(axp2101_chg_pwr_t pwr);

/* Nominal label capacity, 500..10000mAh; ICC rounded down, max1000mA. */
esp_err_t axp2101_set_charge_config(axp2101_chg_pwr_t pwr, uint16_t capacity);
uint16_t axp2101_capacity_mah(void);
uint16_t axp2101_charge_target_ma(axp2101_chg_pwr_t pwr);
esp_err_t axp2101_gauge_status(bool *enabled, bool *present, int *soc);
#endif /* AXP2101_H */
