#include "board.h"
#include "axp2101.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nfc_driver.h"
#include "pcf8563.h"
#include "msa321.h"
#include "pin_map.h"
#include "screen_driver.h"
#include "screen_ui.h"
#include "screen_rotation.h"
#include "tf_card.h"
#include "web_server.h"
#include "wifi_mgr.h"

static const char *TAG = "main";
static TaskHandle_t s_screen_task;

#define SCREEN_EVENT_NETWORK  (1u << 0)
#define SCREEN_EVENT_PMIC_IRQ (1u << 1)

static void IRAM_ATTR pmic_irq_handler(void *arg)
{
    BaseType_t woken = pdFALSE;
    xTaskNotifyFromISR((TaskHandle_t)arg, SCREEN_EVENT_PMIC_IRQ, eSetBits, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

static esp_err_t setup_pmic_irq(void)
{
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = gpio_set_intr_type(pmic_irq, GPIO_INTR_NEGEDGE);
    if (err != ESP_OK) return err;
    err = gpio_isr_handler_add(pmic_irq, pmic_irq_handler, xTaskGetCurrentTaskHandle());
    if (err != ESP_OK) return err;
    err = axp2101_pwron_init();
    if (err != ESP_OK) {
        (void)gpio_isr_handler_remove(pmic_irq);
        return err;
    }
    /* Service a latched event even if IRQ was already low before registration. */
    if (gpio_get_level(pmic_irq) == 0) {
        xTaskNotify(xTaskGetCurrentTaskHandle(), SCREEN_EVENT_PMIC_IRQ, eSetBits);
    }
    ESP_LOGI(TAG, "AXP2101 IRQ on GPIO%d, falling edge", (int)pmic_irq);
    return ESP_OK;
}

static void show_network_page(void)
{
    char url[48];
    wifi_mgr_service_url(url, sizeof(url));
    screen_rotation_lock();
    if (screen_rotation_hidden()) { screen_rotation_unlock(); return; }
    const bool connected = wifi_mgr_is_sta_up();
    ESP_LOGI(TAG, "screen %s  %s", connected ? "LAN QR" : "setup text", url);
    esp_err_t err = connected ? screen_ui_show_link_qr("Scan to upload", url)
                              : screen_ui_show_setup(url);
    screen_rotation_unlock();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "network page failed: %s", esp_err_to_name(err));
    }
}

static void screen_task(void *arg)
{
    (void)arg;

    const esp_err_t pwron_err = setup_pmic_irq();
    if (pwron_err != ESP_OK) {
        ESP_LOGE(TAG, "PWRON init failed: %s", esp_err_to_name(pwron_err));
    }
    while (1) {
        uint32_t events = 0;
        xTaskNotifyWait(0, UINT32_MAX, &events, portMAX_DELAY);
        bool pressed = false;
        /* GPIO ISR only notifies; I2C and display work run here, never in the ISR. */
        if (pwron_err == ESP_OK && (events & SCREEN_EVENT_PMIC_IRQ)) {
            esp_err_t err = axp2101_take_pwron_short_press(&pressed);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "PWRON read failed: %s", esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(1000));
                xTaskNotify(xTaskGetCurrentTaskHandle(), SCREEN_EVENT_PMIC_IRQ, eSetBits);
            } else if (gpio_get_level(pmic_irq) == 0) {
                /* A new source may have latched while the previous snapshot was acknowledged. */
                vTaskDelay(pdMS_TO_TICKS(10));
                xTaskNotify(xTaskGetCurrentTaskHandle(), SCREEN_EVENT_PMIC_IRQ, eSetBits);
            }
        }
        if (pressed) {
            /* Preserve the source for the next short press. */
            for (;;) {
                while (web_server_screen_busy()) vTaskDelay(pdMS_TO_TICKS(25));
                screen_rotation_lock();
                if (!web_server_screen_busy()) break;
                screen_rotation_unlock();
            }
            esp_err_t err = screen_rotation_toggle_blank();
            bool hidden = screen_rotation_hidden();
            screen_rotation_unlock();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "PWRON screen %s", hidden ? "white (source retained)" : "image restored");
            } else {
                ESP_LOGE(TAG, "PWRON toggle failed: %s", esp_err_to_name(err));
            }
        } else if (events & SCREEN_EVENT_NETWORK) {
            show_network_page();
        }
    }
}

static void on_lan_ready(void)
{
    if (s_screen_task) {
        xTaskNotify(s_screen_task, SCREEN_EVENT_NETWORK, eSetBits);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Eink E6 3.7  ESP32-C5");
    esp_err_t board_err = board_init();

    if (board_err != ESP_OK) {
        ESP_LOGE(TAG, "board_init: %s, retry", esp_err_to_name(board_err));
        vTaskDelay(pdMS_TO_TICKS(200));
        board_err = board_init();
    }

    if (board_err != ESP_OK) {
        ESP_LOGE(TAG, "board_init failed: %s", esp_err_to_name(board_err));
        return;
    }
    /* Optional I2C peripherals share the bus and lock created by board_init. */
    esp_err_t rtc_err = pcf8563_init();
    if (rtc_err != ESP_OK) {
        ESP_LOGW(TAG, "RTC init failed: %s", esp_err_to_name(rtc_err));
    }

    esp_err_t imu_err = msa321_init();
    if (imu_err != ESP_OK) {
        ESP_LOGW(TAG, "IMU init failed: %s", esp_err_to_name(imu_err));
    } else {
        msa321_sample_t sample;
        imu_err = msa321_read(&sample);
        if (imu_err == ESP_OK) {
            ESP_LOGI(TAG, "IMU acceleration x=%.3f y=%.3f z=%.3f g",
                     (double)sample.x_g, (double)sample.y_g, (double)sample.z_g);
        } else {
            ESP_LOGW(TAG, "IMU read failed: %s", esp_err_to_name(imu_err));
        }
    }

    esp_err_t tf_err = tf_card_mount();

    if (tf_err != ESP_OK) {
        ESP_LOGW(TAG, "TF mount failed: %s  NFC will log only", esp_err_to_name(tf_err));
    }

    esp_err_t nfc_err = nfc_driver_init();

    if (nfc_err != ESP_OK) {
        ESP_LOGW(TAG, "NFC init failed: %s", esp_err_to_name(nfc_err));
    }

    ESP_ERROR_CHECK(screen_driver_init());
    ESP_ERROR_CHECK(screen_rotation_init());
    ESP_ERROR_CHECK(wifi_mgr_start());
    ESP_ERROR_CHECK(web_server_start());
    if (xTaskCreate(screen_task, "screen_ui", 4096, NULL, 3, &s_screen_task) != pdPASS) {
        ESP_LOGE(TAG, "screen task; PWRON clear unavailable");
        show_network_page();
    } else {
        wifi_mgr_set_ready_cb(on_lan_ready);
        xTaskNotify(s_screen_task, SCREEN_EVENT_NETWORK, eSetBits);
    }

    char url[48];
    wifi_mgr_service_url(url, sizeof(url));
    ESP_LOGI(TAG, "HTTP %s", url);
}
