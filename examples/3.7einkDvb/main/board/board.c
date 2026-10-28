#include "board.h"

#include <stdbool.h>
#include "axp2101.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pin_map.h"

static const char *TAG = "board";

static bool s_inited;
static bool s_spi_ready;
static bool s_i2c_ready;
static i2c_master_bus_handle_t s_i2c;
static SemaphoreHandle_t s_i2c_lock;
static SemaphoreHandle_t s_spi_lock;

static void hold_idle_outputs(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << spi2_tf_cs) | (1ULL << nfc_en),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out));

    gpio_set_level(spi2_tf_cs, 1);
    gpio_set_level(nfc_en, 0);
}

static void config_irq_inputs(void)
{
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << imu_irq) | (1ULL << pmic_irq) |
                        (1ULL << nfc_irq) | (1ULL << rtc_irq),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in));
}

static esp_err_t init_spi_bus(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = spi2_sdi,
        .miso_io_num = spi2_sdo,
        .sclk_io_num = spi2_sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    return spi_bus_initialize(BOARD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
}

static bool i2c_probe_addr(uint8_t addr)
{
    board_i2c_lock();
    esp_err_t err = i2c_master_probe(s_i2c, addr, 20);
    board_i2c_unlock();
    return err == ESP_OK;
}

static bool i2c_axp_alive(void)
{
    return i2c_probe_addr(I2C_ADDR_AXP2101);
}

int board_i2c_scan(const char *why)
{
    int found = 0;

    ESP_LOGI(TAG, "I2C scan [%s] EN=%d", why ? why : "-", gpio_get_level(nfc_en));
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        board_i2c_lock();
        esp_err_t err = i2c_master_probe(s_i2c, addr, 20);
        board_i2c_unlock();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "  0x%02X", addr);
            found++;
        } else if (err == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "  bus timeout at 0x%02X, abort scan", addr);
            break;
        }
    }
    ESP_LOGI(TAG, "I2C scan [%s] %d device(s)", why ? why : "-", found);
    return found;
}

static esp_err_t init_i2c_bus(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = i2c0_sda,
        .scl_io_num = i2c0_scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_i2c), TAG, "i2c bus");
    return ESP_OK;
}

esp_err_t board_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    if (s_i2c_lock == NULL) {
        s_i2c_lock = xSemaphoreCreateMutex();
        s_spi_lock = xSemaphoreCreateMutex();
        if (s_i2c_lock == NULL || s_spi_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
        hold_idle_outputs();
        config_irq_inputs();
    }

    if (!s_spi_ready) {
        ESP_RETURN_ON_ERROR(init_spi_bus(), TAG, "spi2");
        s_spi_ready = true;
    }
    if (!s_i2c_ready) {
        ESP_RETURN_ON_ERROR(init_i2c_bus(), TAG, "i2c0");
        s_i2c_ready = true;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(axp2101_init(), TAG, "axp2101");
    vTaskDelay(pdMS_TO_TICKS(10));
    board_i2c_scan("after-rails EN=0");

    if (i2c_probe_addr(I2C_ADDR_ST25R3916)) {
        ESP_LOGI(TAG, "0x50 already ACK with EN=0, keep NFC_EN low");
    } else {
        gpio_set_level(nfc_en, 1);
        vTaskDelay(pdMS_TO_TICKS(20));
        if (i2c_axp_alive()) {
            board_i2c_scan("after-rails EN=1");
        } else {
            ESP_LOGW(TAG, "EN=1 hung I2C, switch to EN=0");
            gpio_set_level(nfc_en, 0);
            vTaskDelay(pdMS_TO_TICKS(30));
            (void)i2c_master_bus_reset(s_i2c);
            board_i2c_scan("after-rails EN=0 retry");
        }
    }

    s_inited = true;
    ESP_LOGI(TAG, "ESP32-C5 buses ready  SPI2 MOSI=%d MISO=%d CLK=%d CS_EPD=%d CS_TF=%d  I2C SDA=%d SCL=%d  NFC_EN pin=%d level=%d",
             (int)spi2_sdi, (int)spi2_sdo, (int)spi2_sck,
             (int)eink_cs, (int)spi2_tf_cs,
             (int)i2c0_sda, (int)i2c0_scl, (int)nfc_en, gpio_get_level(nfc_en));
    return ESP_OK;
}

spi_host_device_t board_spi_host(void)
{
    return BOARD_SPI_HOST;
}

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return s_i2c;
}

void board_i2c_lock(void)
{
    if (s_i2c_lock) {
        xSemaphoreTake(s_i2c_lock, portMAX_DELAY);
    }
}

void board_i2c_unlock(void)
{
    if (s_i2c_lock) {
        xSemaphoreGive(s_i2c_lock);
    }
}

void board_spi_lock(void)
{
    if (s_spi_lock) {
        xSemaphoreTake(s_spi_lock, portMAX_DELAY);
    }
}

void board_spi_unlock(void)
{
    if (s_spi_lock) {
        xSemaphoreGive(s_spi_lock);
    }
}

void board_tf_cs_idle(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << spi2_tf_cs),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&out);
    gpio_set_level(spi2_tf_cs, 1);
}

void board_nfc_set_enable(bool enable)
{
    gpio_set_level(nfc_en, enable ? 1 : 0);
}

int board_nfc_en_pin_level(void)
{
    return gpio_get_level(nfc_en);
}
