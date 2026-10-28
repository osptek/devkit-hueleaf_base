#include "board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nfc_driver.h"
#include "pcf8563.h"
#include "msa321.h"
#include "screen_driver.h"
#include "screen_ui.h"
#include "tf_card.h"
#include "web_server.h"
#include "wifi_mgr.h"

static const char *TAG = "main";
static TaskHandle_t s_qr_task;

static void show_current_qr(void)
{
    char url[48];
    const char *title = wifi_mgr_is_sta_up() ? "Scan to upload" : "Scan to setup";
    wifi_mgr_service_url(url, sizeof(url));
    ESP_LOGI(TAG, "screen QR %s  %s", title, url);
    (void)screen_ui_show_link_qr(title, url);
}

static void qr_task(void *arg)
{
    (void)arg;
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        show_current_qr();
    }
}

static void on_lan_ready(void)
{
    if (s_qr_task) {
        xTaskNotifyGive(s_qr_task);
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

    ESP_ERROR_CHECK(wifi_mgr_start());
    ESP_ERROR_CHECK(web_server_start());
    ESP_ERROR_CHECK(screen_driver_init());
    if (xTaskCreate(qr_task, "qr_ui", 4096, NULL, 3, &s_qr_task) != pdPASS) {
        ESP_LOGE(TAG, "qr task");
        show_current_qr();
    } else {
        wifi_mgr_set_ready_cb(on_lan_ready);
        xTaskNotifyGive(s_qr_task);
    }

    char url[48];
    wifi_mgr_service_url(url, sizeof(url));
    ESP_LOGI(TAG, "HTTP %s", url);
}
