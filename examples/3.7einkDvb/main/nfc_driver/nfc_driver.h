#ifndef NFC_DRIVER_H
#define NFC_DRIVER_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#define NFC_DUMP_MAX  1024
#define NFC_NDEF_MAX  256

typedef enum {
    NFC_TECH_NONE = 0,
    NFC_TECH_A,
    NFC_TECH_B,
    NFC_TECH_F,
    NFC_TECH_V,
} nfc_tech_t;

typedef enum {
    NFC_PROFILE_UNKNOWN = 0,
    NFC_PROFILE_T2T,
    NFC_PROFILE_T4T,
    NFC_PROFILE_MFC,
    NFC_PROFILE_NFCV,
    NFC_PROFILE_TYPEB,
    NFC_PROFILE_TYPEF,
} nfc_profile_t;

typedef struct {
    nfc_tech_t tech;
    uint8_t uid[10];
    uint8_t uid_len;
    uint8_t sak;
    uint8_t atqa[2];
} nfc_tag_t;

typedef struct {
    nfc_tech_t tech;
    nfc_profile_t profile;
    uint8_t uid[10];
    uint8_t uid_len;
    uint8_t sak;
    uint8_t atqa[2];
    uint8_t version[8];
    uint8_t version_len;
    uint8_t block_size;
    uint16_t block_count;
    uint16_t mem_len;
    uint8_t mem[NFC_DUMP_MAX];
    uint16_t ndef_len;
    uint8_t ndef[NFC_NDEF_MAX];
    char note[64];
} nfc_dump_t;

esp_err_t nfc_driver_init(void);
esp_err_t nfc_driver_poll(nfc_tag_t *tag);
esp_err_t nfc_driver_capture(nfc_dump_t *dump);
void nfc_driver_rf_selftest(void);

#endif /* NFC_DRIVER_H */
