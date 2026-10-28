#include "wifi_ap.h"

#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_ap";

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "station " MACSTR " joined, AID=%d", MAC2STR(e->mac), e->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "station " MACSTR " left, AID=%d", MAC2STR(e->mac), e->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "AP_START — beaconing");
    }
}

esp_err_t wifi_ap_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));

    /* SoftAP 必须关省电，否则 beacon 不稳定，手机经常搜不到 */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.ap.ssid, WIFI_AP_SSID, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = strlen(WIFI_AP_SSID);
    wifi_config.ap.channel = WIFI_AP_CHANNEL;
    wifi_config.ap.password[0] = '\0';
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    wifi_config.ap.ssid_hidden = 0;
    wifi_config.ap.beacon_interval = 100;
    wifi_config.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

#if CONFIG_IDF_TARGET_ESP32C5
    /* C5 必须先 start 再设频段；失败只打日志，不把整机拉死 */
    err = esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_band_mode 5G: %s", esp_err_to_name(err));
    }
    /* 切到 5G 后再写一次信道，避免仍停在 2.4G ch6 */
    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_config 5G ch%d: %s", WIFI_AP_CHANNEL, esp_err_to_name(err));
    }
#endif

    err = esp_wifi_set_country_code("CN", true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_country_code: %s", esp_err_to_name(err));
    }
#if CONFIG_IDF_TARGET_ESP32C5
    wifi_protocols_t ap_proto = {
        .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX,
        .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX,
    };
    err = esp_wifi_set_protocols(WIFI_IF_AP, &ap_proto);
#else
    err = esp_wifi_set_protocol(WIFI_IF_AP,
                               WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N |
                               WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX);
#endif
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_protocol 5G: %s", esp_err_to_name(err));
    }
    err = esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_bandwidth: %s", esp_err_to_name(err));
    }
    err = esp_wifi_set_max_tx_power(80);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_max_tx_power: %s", esp_err_to_name(err));
    }
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    uint8_t mac[6] = {0};
    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    wifi_band_t band = WIFI_BAND_2G;
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    (void)esp_wifi_get_channel(&primary, &second);
    (void)esp_wifi_get_band(&band);
    ESP_LOGI(TAG, "SoftAP \"%s\" 5G ch=%u band=%s open  MAC=" MACSTR,
             WIFI_AP_SSID, primary, band == WIFI_BAND_5G ? "5G" : "2.4G", MAC2STR(mac));
    ESP_LOGI(TAG, "connect then open http://192.168.4.1");
    return ESP_OK;
}
