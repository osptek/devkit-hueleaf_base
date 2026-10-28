#ifndef TF_CARD_H
#define TF_CARD_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#define TF_CARD_MOUNT_POINT "/sdcard"
#define TF_CARD_NFC_DIR     TF_CARD_MOUNT_POINT "/nfc"
#define TF_CARD_NFC_LOG     TF_CARD_NFC_DIR "/nfc_log.txt"
#define TF_CARD_NFC_LAST    TF_CARD_NFC_DIR "/nfc_last.txt"
#define TF_CARD_NFC_DUMP    TF_CARD_NFC_DIR "/nfc_dump.txt"
#define TF_CARD_NFC_BIN     TF_CARD_NFC_DIR "/nfc_dump.bin"

esp_err_t tf_card_mount(void);
bool tf_card_is_mounted(void);
esp_err_t tf_card_write_text(const char *path, const char *text, bool append);
esp_err_t tf_card_begin_write(const char *path, bool append);
esp_err_t tf_card_write_raw(const void *data, size_t len);
esp_err_t tf_card_print(const char *text);
esp_err_t tf_card_end_write(void);

#endif /* TF_CARD_H */
