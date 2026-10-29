#include "axp2101.h"

#include "board.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "axp2101";

#define AXP_ADDR                 I2C_ADDR_AXP2101
#define AXP_CHIP_ID              0x4A
#define AXP_REG_STATUS1          0x00
#define AXP_REG_STATUS2          0x01
#define AXP_REG_IC_TYPE          0x03
#define AXP_REG_INPUT_CUR_LIMIT  0x16  /* XPOWERS_AXP2101_INPUT_CUR_LIMIT_CTRL */
#define AXP_REG_CHG_GAUGE_WDT    0x18  /* XPOWERS_AXP2101_CHARGE_GAUGE_WDT_CTRL */
#define AXP_REG_ADC_CHANNEL      0x30  /* XPOWERS_AXP2101_ADC_CHANNEL_CTRL */
#define AXP_REG_ADC_VBAT_H       0x34  /* XPOWERS_AXP2101_ADC_DATA_RELUST0 */
#define AXP_REG_ADC_VBAT_L       0x35  /* XPOWERS_AXP2101_ADC_DATA_RELUST1 */
#define AXP_REG_INTSTS1          0x48
#define AXP_REG_INTEN2           0x41
#define AXP_REG_INTSTS2          0x49
#define AXP_REG_TS_PIN_CTRL      0x50
#define AXP_REG_ICC_CHG          0x62  /* XPOWERS_AXP2101_ICC_CHG_SET */
#define AXP_REG_BAT_DET          0x68  /* XPOWERS_AXP2101_BAT_DET_CTRL */
#define AXP_REG_LDO_ONOFF0       0x90
#define AXP_REG_ALDO3_VOL        0x94  /* XPOWERS_AXP2101_LDO_VOL2_CTRL */
#define AXP_REG_DLDO1_VOL        0x99  /* XPOWERS_AXP2101_LDO_VOL7_CTRL */
#define AXP_REG_FUEL_GAUGE       0xA2  /* XPOWERS_AXP2101_FUEL_GAUGE_CTRL */
#define AXP_REG_BAT_PERCENT      0xA4  /* XPOWERS_AXP2101_BAT_PERCENT_DATA */

#define AXP_ALDO3_EN_BIT         2
#define AXP_DLDO1_EN_BIT         7
#define AXP_GAUGE_EN_BIT         3  /* enableGauge */
#define AXP_CHG_EN_BIT           1
#define AXP_PWRON_SHORT_BIT      3  /* XPOWERS_AXP2101_PKEY_SHORT_IRQ >> 8 */
#define AXP_ADC_BAT_BIT          0  /* enableBattVoltageMeasure */
#define AXP_ADC_TS_BIT           1
#define AXP_BAT_DET_BIT          0  /* enableBattDetection */
#define AXP_BROM_WRITE_BIT       0  /* parameter write access, NOT gauge enable */
#define AXP_TS_FUNC_EXT_BIT      4  /* 1=外部输入不参与充电，0=NTC 温度并停充 */
#define AXP_TS_CUR_SRC_SHIFT     2
#define AXP_TS_CUR_SRC_ADC       0x01  /* 01: TS ADC 开时灌电流 */
#define AXP_TS_CUR_50UA          0x02  /* 10: 50uA，10k NTC@25℃ */

#define AXP_ICC_200MA            0x08  /* XPOWERS_AXP2101_CHG_CUR_200MA */
#define AXP_ICC_500MA            0x0B  /* XPOWERS_AXP2101_CHG_CUR_500MA */
#define AXP_ICC_1000MA           0x10  /* XPOWERS_AXP2101_CHG_CUR_1000MA */
#define AXP_VBUS_LIM_500MA       0x01  /* XPOWERS_AXP2101_VBUS_CUR_LIM_500MA */
#define AXP_VBUS_LIM_1000MA      0x03  /* XPOWERS_AXP2101_VBUS_CUR_LIM_1000MA */
#define AXP_VBUS_LIM_1500MA      0x04  /* XPOWERS_AXP2101_VBUS_CUR_LIM_1500MA */

#define AXP_NVS_NS               "axp"
#define AXP_NVS_KEY_PWR          "chg_pwr"

#define AXP_LDO_VOL_MIN_MV       500
#define AXP_LDO_VOL_STEP_MV      100
#define AXP_RAIL_MV              3300

#define AXP_I2C_HZ               100000
#define AXP_I2C_TIMEOUT_MS       200
#define AXP_SOC_LOG_MS           10000
#define AXP_ID_TRIES             8

static const uint8_t s_rate_tenths[] = { 2, 5, 10 };
static const uint8_t s_vbus_code[] = { AXP_VBUS_LIM_500MA, AXP_VBUS_LIM_1000MA, AXP_VBUS_LIM_1500MA };
static const uint16_t s_vbus_ma[] = { 500, 1000, 1500 };
static const char *s_pwr_id[] = { "slow", "balanced", "fast" };
static const char *s_pwr_name[] = { "涓流充电", "正常充电", "快速充电" };

static i2c_master_dev_handle_t s_dev;
static bool s_inited;
static uint16_t s_capacity_mah = 500;
static axp2101_chg_pwr_t s_pwr = AXP2101_CHG_PWR_2W5;

static esp_err_t axp_read(uint8_t reg, uint8_t *val)
{
    board_i2c_lock();
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, AXP_I2C_TIMEOUT_MS);
    board_i2c_unlock();
    return err;
}

static esp_err_t axp_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    board_i2c_lock();
    esp_err_t err = i2c_master_transmit(s_dev, buf, sizeof(buf), AXP_I2C_TIMEOUT_MS);
    board_i2c_unlock();
    return err;
}

esp_err_t axp2101_pwron_init(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    uint8_t enabled;
    ESP_RETURN_ON_ERROR(axp_read(AXP_REG_INTEN2, &enabled), TAG, "PWRON IRQ enable");
    /* Discard startup IRQs before accepting user presses, including the power-on key. */
    bool ignored;
    ESP_RETURN_ON_ERROR(axp2101_take_pwron_short_press(&ignored), TAG, "startup IRQ clear");
    return axp_write(AXP_REG_INTEN2, (uint8_t)(enabled | (1u << AXP_PWRON_SHORT_BIT)));
}

esp_err_t axp2101_take_pwron_short_press(bool *pressed)
{
    if (!pressed) return ESP_ERR_INVALID_ARG;
    *pressed = false;
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    uint8_t status[3];
    for (unsigned i = 0; i < sizeof(status); ++i) {
        ESP_RETURN_ON_ERROR(axp_read(AXP_REG_INTSTS1 + i, &status[i]), TAG, "IRQ status");
    }
    if (!(status[0] | status[1] | status[2])) return ESP_OK;
    ESP_LOGI(TAG, "IRQ INTSTS=0x%02X/0x%02X/0x%02X", status[0], status[1], status[2]);
    /* W1C only the observed snapshot. Consume all sources so nIRQ can deassert
     * and the next GPIO falling edge can be detected. INTEN and power timings stay intact. */
    if (status[1]) {
        ESP_RETURN_ON_ERROR(axp_write(AXP_REG_INTSTS2, status[1]), TAG, "PWRON IRQ acknowledge");
        *pressed = (status[1] & (1u << AXP_PWRON_SHORT_BIT)) != 0;
    }
    /* Preserve an acknowledged key result even if another source fails to clear. */
    for (unsigned i = 0; i < sizeof(status); i += 2) {
        if (status[i]) {
            ESP_RETURN_ON_ERROR(axp_write(AXP_REG_INTSTS1 + i, status[i]), TAG, "IRQ acknowledge");
        }
    }
    return ESP_OK;
}

static uint8_t volt_to_code(uint16_t mv)
{
    return (uint8_t)((mv - AXP_LDO_VOL_MIN_MV) / AXP_LDO_VOL_STEP_MV);
}

static uint16_t code_to_volt(uint8_t code)
{
    return (uint16_t)((code & 0x1F) * AXP_LDO_VOL_STEP_MV + AXP_LDO_VOL_MIN_MV);
}

static esp_err_t set_ldo_voltage(uint8_t reg, uint16_t mv)
{
    uint8_t val = 0;
    ESP_RETURN_ON_ERROR(axp_read(reg, &val), TAG, "read vol 0x%02X", reg);
    val = (uint8_t)((val & 0xE0) | (volt_to_code(mv) & 0x1F));
    return axp_write(reg, val);
}

static esp_err_t enable_ldo_bits(uint8_t bits)
{
    uint8_t val = 0;
    ESP_RETURN_ON_ERROR(axp_read(AXP_REG_LDO_ONOFF0, &val), TAG, "read onoff");
    val = (uint8_t)(val | bits);
    return axp_write(AXP_REG_LDO_ONOFF0, val);
}

esp_err_t axp2101_enable_peripheral_rails(void)
{
    uint8_t onoff = 0;
    uint8_t aldo3 = 0;
    uint8_t dldo1 = 0;

    ESP_RETURN_ON_ERROR(set_ldo_voltage(AXP_REG_ALDO3_VOL, AXP_RAIL_MV), TAG, "ALDO3 volt");
    ESP_RETURN_ON_ERROR(set_ldo_voltage(AXP_REG_DLDO1_VOL, AXP_RAIL_MV), TAG, "DLDO1 volt");
    ESP_RETURN_ON_ERROR(enable_ldo_bits((1u << AXP_ALDO3_EN_BIT) | (1u << AXP_DLDO1_EN_BIT)),
                        TAG, "LDO on");

    ESP_RETURN_ON_ERROR(axp_read(AXP_REG_LDO_ONOFF0, &onoff), TAG, "onoff");
    ESP_RETURN_ON_ERROR(axp_read(AXP_REG_ALDO3_VOL, &aldo3), TAG, "aldo3");
    ESP_RETURN_ON_ERROR(axp_read(AXP_REG_DLDO1_VOL, &dldo1), TAG, "dldo1");

    ESP_LOGI(TAG, "ALDO3 %s %umV  DLDO1 %s %umV  onoff=0x%02X",
             (onoff & (1u << AXP_ALDO3_EN_BIT)) ? "ON" : "OFF",
             code_to_volt(aldo3),
             (onoff & (1u << AXP_DLDO1_EN_BIT)) ? "ON" : "OFF",
             code_to_volt(dldo1),
             onoff);
    return ESP_OK;
}

static uint16_t icc_to_ma(uint8_t code)
{
    code &= 0x1F;
    if (code <= 8) {
        return (uint16_t)(code * 25u);
    }
    return (uint16_t)(200u + (uint16_t)(code - 8u) * 100u);
}

static bool pwr_valid(axp2101_chg_pwr_t pwr)
{
    return (unsigned)pwr <= AXP2101_CHG_PWR_5W;
}

/* Use battery label capacity; never infer charge limits from SOC. */
static uint8_t charge_code(axp2101_chg_pwr_t pwr, uint16_t capacity)
{
    uint32_t target = (uint32_t)capacity * s_rate_tenths[pwr] / 10u;
    if (target >= 1000) return 16;
    if (target >= 200) return (uint8_t)(8 + (target - 200) / 100);
    return (uint8_t)(target / 25); /* capacity >=500 => target >=100 */
}

uint16_t axp2101_capacity_mah(void) { return s_capacity_mah; }
uint16_t axp2101_charge_target_ma(axp2101_chg_pwr_t pwr)
{
    if (!pwr_valid(pwr)) pwr = AXP2101_CHG_PWR_2W5;
    return (uint16_t)((uint32_t)s_capacity_mah * s_rate_tenths[pwr] / 10u);
}

static void load_pwr_nvs(void)
{
    nvs_handle_t nvs;
    if (nvs_flash_init() != ESP_OK || nvs_open(AXP_NVS_NS, NVS_READONLY, &nvs) != ESP_OK) return;
    uint32_t config = 0;
    uint8_t legacy = 1;
    if (nvs_get_u32(nvs, "charge_cfg", &config) == ESP_OK) {
        unsigned capacity = config >> 8;
        if (capacity >= 500 && capacity <= 10000 && pwr_valid((axp2101_chg_pwr_t)(config & 255))) {
            s_capacity_mah = (uint16_t)capacity;
            s_pwr = (axp2101_chg_pwr_t)(config & 255);
        }
    } else if (nvs_get_u8(nvs, AXP_NVS_KEY_PWR, &legacy) == ESP_OK && pwr_valid((axp2101_chg_pwr_t)legacy)) {
        s_pwr = (axp2101_chg_pwr_t)legacy;
    }
    nvs_close(nvs);
}

static esp_err_t save_config(axp2101_chg_pwr_t pwr, uint16_t capacity)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(AXP_NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_u32(nvs, "charge_cfg", ((uint32_t)capacity << 8) | (uint8_t)pwr);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static esp_err_t apply_charge_config(axp2101_chg_pwr_t pwr, uint16_t capacity)
{
    uint8_t icc, vbus, check;
    uint8_t code = charge_code(pwr, capacity);
    esp_err_t err;

    board_i2c_lock();
    uint8_t reg = AXP_REG_ICC_CHG;
    err = i2c_master_transmit_receive(s_dev, &reg, 1, &icc, 1, AXP_I2C_TIMEOUT_MS);
    if (err != ESP_OK) goto done;
    reg = AXP_REG_INPUT_CUR_LIMIT;
    err = i2c_master_transmit_receive(s_dev, &reg, 1, &vbus, 1, AXP_I2C_TIMEOUT_MS);
    if (err != ESP_OK) goto done;
    if ((icc & 31) != code) {
        uint8_t data[] = { AXP_REG_ICC_CHG, (uint8_t)((icc & 0xE0) | code) };
        err = i2c_master_transmit(s_dev, data, sizeof(data), AXP_I2C_TIMEOUT_MS);
        if (err != ESP_OK) goto rollback;
    }
    if ((vbus & 7) != s_vbus_code[pwr]) {
        uint8_t data[] = { AXP_REG_INPUT_CUR_LIMIT, (uint8_t)((vbus & 0xF8) | s_vbus_code[pwr]) };
        err = i2c_master_transmit(s_dev, data, sizeof(data), AXP_I2C_TIMEOUT_MS);
        if (err != ESP_OK) goto rollback;
    }
    reg = AXP_REG_ICC_CHG;
    err = i2c_master_transmit_receive(s_dev, &reg, 1, &check, 1, AXP_I2C_TIMEOUT_MS);
    if (err != ESP_OK || (check & 31) != code) { if (err == ESP_OK) err = ESP_FAIL; goto rollback; }
    reg = AXP_REG_INPUT_CUR_LIMIT;
    err = i2c_master_transmit_receive(s_dev, &reg, 1, &check, 1, AXP_I2C_TIMEOUT_MS);
    if (err == ESP_OK && (check & 7) == s_vbus_code[pwr]) goto done;
    if (err == ESP_OK) err = ESP_FAIL;
rollback: {
        uint8_t data_icc[] = { AXP_REG_ICC_CHG, icc };
        uint8_t data_vbus[] = { AXP_REG_INPUT_CUR_LIMIT, vbus };
        esp_err_t r1 = i2c_master_transmit(s_dev, data_icc, sizeof(data_icc), AXP_I2C_TIMEOUT_MS);
        esp_err_t r2 = i2c_master_transmit(s_dev, data_vbus, sizeof(data_vbus), AXP_I2C_TIMEOUT_MS);
        if (r1 != ESP_OK || r2 != ESP_OK) ESP_LOGE(TAG, "charge rollback failed; inspect hardware");
    }
done:
    board_i2c_unlock();
    return err;
}

static const char *chg_state_name(uint8_t status2)
{
    switch (status2 & 0x07) {
    case 0: return "tri";
    case 1: return "pre";
    case 2: return "CC";
    case 3: return "CV";
    case 4: return "done";
    case 5: return "stop";
    default: return "rsvd";
    }
}

esp_err_t axp2101_configure_charge(void)
{
    uint8_t icc = 0;
    uint8_t chg = 0;
    uint8_t adc = 0;
    uint8_t ts = 0;
    uint8_t st1 = 0;
    uint8_t st2 = 0;
    /* 与 XPowers setChargerConstantCurr / setVbusCurrentLimit / enableCellbatteryCharge 一致 */
    ESP_RETURN_ON_ERROR(apply_charge_config(s_pwr, s_capacity_mah), TAG, "pwr");

    ESP_RETURN_ON_ERROR(axp_read(AXP_REG_CHG_GAUGE_WDT, &chg), TAG, "chg en");
    ESP_RETURN_ON_ERROR(axp_write(AXP_REG_CHG_GAUGE_WDT,
                                  (uint8_t)(chg | (1u << AXP_CHG_EN_BIT) | (1u << AXP_GAUGE_EN_BIT))),
                        TAG, "chg+gauge on");

    /* 板载 NTC + 电池电压：打开 TS/BAT ADC，50uA 电流源，REG50[4]=0 让温度参与停充 */
    ESP_RETURN_ON_ERROR(axp_read(AXP_REG_ADC_CHANNEL, &adc), TAG, "adc");
    ESP_RETURN_ON_ERROR(axp_write(AXP_REG_ADC_CHANNEL,
                                  (uint8_t)(adc | (1u << AXP_ADC_TS_BIT) | (1u << AXP_ADC_BAT_BIT))),
                        TAG, "ts/bat adc on");
    ESP_RETURN_ON_ERROR(axp_read(AXP_REG_TS_PIN_CTRL, &ts), TAG, "ts pin");
    ts = (uint8_t)(ts & (uint8_t)~((1u << AXP_TS_FUNC_EXT_BIT) | 0x0Fu));
    ts = (uint8_t)(ts | (AXP_TS_CUR_SRC_ADC << AXP_TS_CUR_SRC_SHIFT) | AXP_TS_CUR_50UA);
    ESP_RETURN_ON_ERROR(axp_write(AXP_REG_TS_PIN_CTRL, ts), TAG, "ts ntc");

    ESP_RETURN_ON_ERROR(axp_read(AXP_REG_BAT_DET, &chg), TAG, "bat det");
    ESP_RETURN_ON_ERROR(axp_write(AXP_REG_BAT_DET, (uint8_t)(chg | (1u << AXP_BAT_DET_BIT))),
                        TAG, "bat det on");
    ESP_RETURN_ON_ERROR(axp_read(AXP_REG_FUEL_GAUGE, &chg), TAG, "fuel");
    if (chg & (1u << AXP_BROM_WRITE_BIT)) {
        ESP_RETURN_ON_ERROR(axp_write(AXP_REG_FUEL_GAUGE, (uint8_t)(chg & ~(1u << AXP_BROM_WRITE_BIT))),
                            TAG, "close battery model write access");
    }

    ESP_RETURN_ON_ERROR(axp_read(AXP_REG_ICC_CHG, &icc), TAG, "icc rd");
    ESP_RETURN_ON_ERROR(axp_read(AXP_REG_STATUS1, &st1), TAG, "st1");
    ESP_RETURN_ON_ERROR(axp_read(AXP_REG_STATUS2, &st2), TAG, "st2");

    ESP_LOGI(TAG, "charge  %s  ICC=%umA  VBUS_LIM=%umA  NTC on  bat=%s vbus=%s %s  st=0x%02X/0x%02X ts=0x%02X",
             s_pwr_name[s_pwr],
             icc_to_ma(icc),
             s_vbus_ma[s_pwr],
             (st1 & (1u << 3)) ? "in" : "out",
             (st1 & (1u << 5)) ? "good" : "no",
             chg_state_name(st2),
             st1, st2, ts);
    return ESP_OK;
}

esp_err_t axp2101_set_charge_config(axp2101_chg_pwr_t pwr, uint16_t capacity)
{
    if (!pwr_valid(pwr) || capacity < 500 || capacity > 10000) return ESP_ERR_INVALID_ARG;
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (pwr == s_pwr && capacity == s_capacity_mah) return ESP_OK;
    ESP_RETURN_ON_ERROR(apply_charge_config(pwr, capacity), TAG, "apply charge");
    esp_err_t err = save_config(pwr, capacity);
    if (err != ESP_OK) {
        if (apply_charge_config(s_pwr, s_capacity_mah) != ESP_OK) ESP_LOGE(TAG, "NVS rollback failed");
        return err;
    }
    s_pwr = pwr;
    s_capacity_mah = capacity;
    ESP_LOGI(TAG, "charge %s capacity=%umAh target=%umA set=%umA", s_pwr_name[pwr],
             (unsigned)capacity, (unsigned)axp2101_charge_target_ma(pwr), (unsigned)icc_to_ma(charge_code(pwr, capacity)));
    return ESP_OK;
}

esp_err_t axp2101_set_charge_power(axp2101_chg_pwr_t pwr)
{
    return axp2101_set_charge_config(pwr, s_capacity_mah);
}

esp_err_t axp2101_gauge_status(bool *enabled, bool *present, int *soc)
{
    if (!enabled || !present || !soc) return ESP_ERR_INVALID_ARG;
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    uint8_t ctrl, st, pct;
    ESP_RETURN_ON_ERROR(axp_read(AXP_REG_CHG_GAUGE_WDT, &ctrl), TAG, "gauge");
    ESP_RETURN_ON_ERROR(axp_read(AXP_REG_STATUS1, &st), TAG, "battery");
    ESP_RETURN_ON_ERROR(axp_read(AXP_REG_BAT_PERCENT, &pct), TAG, "soc");
    *enabled = (ctrl & (1u << AXP_GAUGE_EN_BIT)) != 0;
    *present = (st & 8) != 0;
    *soc = (*enabled && *present && pct <= 100) ? pct : -1;
    return ESP_OK;
}

axp2101_chg_pwr_t axp2101_get_charge_power(void)
{
    return s_pwr;
}

const char *axp2101_charge_power_id(axp2101_chg_pwr_t pwr)
{
    return pwr_valid(pwr) ? s_pwr_id[pwr] : s_pwr_id[AXP2101_CHG_PWR_2W5];
}

const char *axp2101_charge_power_name(axp2101_chg_pwr_t pwr)
{
    return pwr_valid(pwr) ? s_pwr_name[pwr] : s_pwr_name[AXP2101_CHG_PWR_2W5];
}

uint16_t axp2101_charge_icc_ma(axp2101_chg_pwr_t pwr)
{
    return pwr_valid(pwr) ? icc_to_ma(charge_code(pwr, s_capacity_mah)) : icc_to_ma(charge_code(AXP2101_CHG_PWR_2W5, s_capacity_mah));
}

uint16_t axp2101_charge_vbus_lim_ma(axp2101_chg_pwr_t pwr)
{
    return pwr_valid(pwr) ? s_vbus_ma[pwr] : s_vbus_ma[AXP2101_CHG_PWR_2W5];
}

static void soc_log_task(void *arg)
{
    (void)arg;
    while (1) {
        uint8_t pct = 0;
        uint8_t st1 = 0;
        uint8_t st2 = 0;
        uint8_t vh = 0;
        uint8_t vl = 0;

        uint8_t reg = AXP_REG_STATUS1;
        uint8_t status[2], voltage[2];
        board_i2c_lock();
        esp_err_t sample_err = i2c_master_transmit_receive(s_dev, &reg, 1, status, sizeof(status), AXP_I2C_TIMEOUT_MS);
        reg = AXP_REG_ADC_VBAT_H;
        if (sample_err == ESP_OK) sample_err = i2c_master_transmit_receive(s_dev, &reg, 1, voltage, sizeof(voltage), AXP_I2C_TIMEOUT_MS);
        if (sample_err == ESP_OK) {
            reg = AXP_REG_BAT_PERCENT;
            sample_err = i2c_master_transmit_receive(s_dev, &reg, 1, &pct, 1, AXP_I2C_TIMEOUT_MS);
        }
        board_i2c_unlock();
        if (sample_err == ESP_OK) {
            st1 = status[0]; st2 = status[1]; vh = voltage[0]; vl = voltage[1];
            if ((st1 & (1u << 3)) && pct <= 100) {
                ESP_LOGI(TAG, "soc  %u%%  %umV  %s  %s",
                         (unsigned)pct,
                         (unsigned)(((vh & 0x1F) << 8) | vl),
                         chg_state_name(st2),
                         s_pwr_name[s_pwr]);
            } else {
                ESP_LOGI(TAG, "soc unavailable bat=%u raw=%u", (unsigned)((st1 >> 3) & 1), (unsigned)pct);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(AXP_SOC_LOG_MS));
    }
}

esp_err_t axp2101_init(void)
{
    uint8_t chip_id = 0;

    if (s_inited) {
        return ESP_OK;
    }
    if (board_i2c_bus() == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_dev == NULL) {
        i2c_device_config_t devcfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = AXP_ADDR,
            .scl_speed_hz = AXP_I2C_HZ,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(board_i2c_bus(), &devcfg, &s_dev),
                            TAG, "add i2c");
    }

    for (int i = 0; i < AXP_ID_TRIES; i++) {
        esp_err_t err = axp_read(AXP_REG_IC_TYPE, &chip_id);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "chip id try %d: %s", i + 1, esp_err_to_name(err));
        (void)i2c_master_bus_reset(board_i2c_bus());
        vTaskDelay(pdMS_TO_TICKS(30));
        if (i + 1 == AXP_ID_TRIES) {
            ESP_RETURN_ON_ERROR(err, TAG, "chip id");
        }
    }
    if (chip_id != AXP_CHIP_ID) {
        ESP_LOGW(TAG, "IC_TYPE=0x%02X, expected 0x%02X", chip_id, AXP_CHIP_ID);
    } else {
        ESP_LOGI(TAG, "AXP2101 chip id 0x%02X", chip_id);
    }

    load_pwr_nvs();
    ESP_RETURN_ON_ERROR(axp2101_enable_peripheral_rails(), TAG, "rails");
    ESP_RETURN_ON_ERROR(axp2101_configure_charge(), TAG, "charge");
    if (xTaskCreate(soc_log_task, "axp_soc", 2048, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "soc log task");
    }
    s_inited = true;
    return ESP_OK;
}
