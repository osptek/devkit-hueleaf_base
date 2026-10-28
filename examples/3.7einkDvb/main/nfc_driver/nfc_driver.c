/*
 * ST25R3916 测试流程：对齐 demoCode
 *   616 Type A UID + T2T dump / T4T NDEF
 *   601 Type V UID + 602 Read Single Block dump
 *   613 Type B PUPI
 *   614 Type F IDm
 * 可读用户区写入 TF：/sdcard/nfc/nfc_dump.txt + nfc_dump.bin
 */

#include "nfc_driver.h"

#include <stdio.h>
#include <string.h>
#include "board.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pin_map.h"
#include "tf_card.h"

static const char *TAG = "nfc";

#define ST25R_I2C_ADDR          I2C_ADDR_ST25R3916
#define ST25R_I2C_HZ            100000
#define ST25R_I2C_TIMEOUT_MS    50

#define ST25R_READ              0x40
#define ST25R_FIFO_LOAD         0x80
#define ST25R_FIFO_READ         0x9F
#define ST25R_CMD_SET_DEFAULT   0xC1
#define ST25R_CMD_STOP          0xC2
#define ST25R_CMD_TX_CRC        0xC4
#define ST25R_CMD_TX_NOCRC      0xC5
#define ST25R_CMD_TX_REQA       0xC6
#define ST25R_CMD_UNMASK_RX     0xD1
#define ST25R_CMD_ADJUST_REG    0xD5
#define ST25R_CMD_CALIBRATE_ANT 0xD8
#define ST25R_CMD_MEASURE_AMP   0xD9
#define ST25R_CMD_MEASURE_PHASE 0xDC
#define ST25R_CMD_MEASURE_CAP   0xDE
#define ST25R_CMD_MEASURE_VDD   0xDF
#define ST25R_CMD_CLEAR_FIFO    0xDB

#define REG_IO_CONF1            0x00
#define REG_IO_CONF2            0x01
#define REG_OP_CONTROL          0x02
#define REG_MODE                0x03
#define REG_BIT_RATE            0x04
#define REG_ISO14443A           0x05
#define REG_RX_CONF1            0x0B
#define REG_RX_CONF2            0x0C
#define REG_MASK_RX_TIMER       0x0F
#define REG_NRT1                0x10
#define REG_NRT2                0x11
#define REG_IRQ_MASK_MAIN       0x16
#define REG_IRQ_MASK_TIMER      0x17
#define REG_IRQ_MASK_ERROR      0x18
#define REG_IRQ_MASK_TARGET     0x19
#define REG_IRQ_MAIN            0x1A
#define REG_IRQ_TIMER           0x1B
#define REG_FIFO_STATUS1        0x1E
#define REG_NUM_TX_BYTES1       0x22
#define REG_NUM_TX_BYTES2       0x23
#define REG_TX_DRIVER           0x29
#define REG_REGULATOR_CONTROL   0x2A
#define REG_REGULATOR_RESULT    0x2B
#define REG_RSSI_RESULT         0x2C
#define REG_CAP_SENSOR_RESULT   0x2F
#define REG_AUX_DISPLAY         0x30
#define REG_AD_RESULT           0x3A
#define REG_IC_IDENTITY         0x3F

#define OP_EN                   (1u << 7)
#define OP_RX_EN                (1u << 6)
#define OP_TX_EN                (1u << 3)
#define MODE_OM_NFCA            0x08
#define MODE_OM_NFCB            0x04
#define MODE_OM_NFCF            0x0C
#define MODE_OM_ISO15693        0x0D
#define IRQ_OSC                 (1u << 7)
#define IRQ_RXE                 (1u << 4)
#define IRQ_COL                 (1u << 2)
#define IRQ_DCT                 (1u << 7) /* IRQ_TIMER bit7：I_dct 直接命令完成 */
#define AUX_OSC_OK              (1u << 4)
#define AUX_TX_ON               (1u << 5)
#define IO_CONF2_I2C_3V3        0x98 /* sup3V | miso_pd2 | bit4 */
#define REG_MPSV_MASK           0x07

#define IC_TYPE_MASK            0xF8
#define IC_TYPE_3916            0x28
#define IC_TYPE_3916B           0x30

#define SAK_CASCADE             0x04
#define SAK_ISODEP              0x20
#define SAK_MIFARE              0x08

static i2c_master_dev_handle_t s_dev;
static bool s_inited;
static bool s_osc_irq_ok;
static nfc_dump_t s_dump;

static void delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1));
}

static const char *tech_name(nfc_tech_t tech)
{
    switch (tech) {
    case NFC_TECH_A: return "TypeA";
    case NFC_TECH_B: return "TypeB";
    case NFC_TECH_F: return "TypeF";
    case NFC_TECH_V: return "TypeV";
    default: return "None";
    }
}

static const char *profile_name(nfc_profile_t p)
{
    switch (p) {
    case NFC_PROFILE_T2T: return "T2T";
    case NFC_PROFILE_T4T: return "T4T";
    case NFC_PROFILE_MFC: return "MIFARE_Classic";
    case NFC_PROFILE_NFCV: return "NFCV";
    case NFC_PROFILE_TYPEB: return "TypeB";
    case NFC_PROFILE_TYPEF: return "TypeF";
    default: return "Unknown";
    }
}

static void format_uid(const uint8_t *uid, uint8_t uid_len, char *out, size_t out_sz)
{
    size_t off = 0;
    out[0] = '\0';
    for (uint8_t i = 0; i < uid_len && off + 3 < out_sz; i++) {
        off += (size_t)snprintf(out + off, out_sz - off, "%s%02X",
                                i ? ":" : "", uid[i]);
    }
}

static void set_note(nfc_dump_t *d, const char *note)
{
    snprintf(d->note, sizeof(d->note), "%s", note ? note : "");
}

static esp_err_t raw_tx(const uint8_t *data, size_t len)
{
    board_i2c_lock();
    esp_err_t err = i2c_master_transmit(s_dev, data, len, ST25R_I2C_TIMEOUT_MS);
    board_i2c_unlock();
    return err;
}

static esp_err_t raw_txrx(const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len)
{
    board_i2c_lock();
    esp_err_t err = i2c_master_transmit_receive(s_dev, tx, tx_len, rx, rx_len,
                                                ST25R_I2C_TIMEOUT_MS);
    board_i2c_unlock();
    return err;
}

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { (uint8_t)(reg & 0x3F), val };
    return raw_tx(buf, sizeof(buf));
}

static esp_err_t read_reg(uint8_t reg, uint8_t *val)
{
    uint8_t opcode = (uint8_t)(ST25R_READ | (reg & 0x3F));
    return raw_txrx(&opcode, 1, val, 1);
}

static esp_err_t send_cmd(uint8_t cmd)
{
    return raw_tx(&cmd, 1);
}

static esp_err_t fifo_write(const uint8_t *data, size_t len)
{
    uint8_t buf[65];
    if (len == 0 || len > 64) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = ST25R_FIFO_LOAD;
    memcpy(&buf[1], data, len);
    return raw_tx(buf, len + 1);
}

static esp_err_t fifo_read(uint8_t *data, size_t len)
{
    uint8_t opcode = ST25R_FIFO_READ;
    return raw_txrx(&opcode, 1, data, len);
}

static esp_err_t wait_irq_main(uint8_t mask, uint32_t timeout_ms, uint8_t *status)
{
    TickType_t wait_ticks = pdMS_TO_TICKS(timeout_ms);
    const TickType_t deadline = xTaskGetTickCount() + (wait_ticks ? wait_ticks : 1);
    uint8_t irq = 0;

    for (;;) {
        ESP_RETURN_ON_ERROR(read_reg(REG_IRQ_MAIN, &irq), TAG, "irq");
        if (irq & mask) {
            if (status) {
                *status = irq;
            }
            return ESP_OK;
        }
        if (xTaskGetTickCount() >= deadline) {
            break;
        }
        vTaskDelay(1);
    }

    /* 节拍粒度下，超时后再读一次，避免 ATQA 落在 delay 窗口里被漏掉 */
    ESP_RETURN_ON_ERROR(read_reg(REG_IRQ_MAIN, &irq), TAG, "irq");
    if (status) {
        *status = irq;
    }
    if (irq & mask) {
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t clear_irqs(void)
{
    uint8_t dummy;
    (void)read_reg(REG_IRQ_MAIN, &dummy);
    (void)read_reg(REG_IRQ_TIMER, &dummy);
    (void)read_reg(0x1C, &dummy);
    (void)read_reg(0x1D, &dummy);
    return ESP_OK;
}

static void field_idle(void)
{
    (void)send_cmd(ST25R_CMD_STOP);
    (void)write_reg(REG_OP_CONTROL, OP_EN);
}

static esp_err_t wait_irq_timer(uint8_t mask, uint32_t timeout_ms)
{
    TickType_t wait_ticks = pdMS_TO_TICKS(timeout_ms);
    const TickType_t deadline = xTaskGetTickCount() + (wait_ticks ? wait_ticks : 1);
    uint8_t irq = 0;

    for (;;) {
        if (read_reg(REG_IRQ_TIMER, &irq) != ESP_OK) {
            return ESP_FAIL;
        }
        if (irq & mask) {
            return ESP_OK;
        }
        if (xTaskGetTickCount() >= deadline) {
            break;
        }
        vTaskDelay(1);
    }
    return (read_reg(REG_IRQ_TIMER, &irq) == ESP_OK && (irq & mask)) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static bool measure_cmd(uint8_t cmd, uint8_t *adc, uint8_t *irq_out)
{
    uint8_t irq = 0;
    bool dct = false;

    (void)clear_irqs();
    if (send_cmd(cmd) != ESP_OK) {
        return false;
    }
    dct = (wait_irq_timer(IRQ_DCT, 20) == ESP_OK);
    if (!dct) {
        delay_ms(2);
        (void)read_reg(REG_IRQ_TIMER, &irq);
    }
    if (irq_out) {
        *irq_out = irq;
    }
    return read_reg(REG_AD_RESULT, adc) == ESP_OK;
}

static void dump_regs(void)
{
    uint8_t page[16];
    char line[64];
    for (uint8_t base = 0; base < 0x40; base = (uint8_t)(base + 16)) {
        int off = 0;
        for (uint8_t i = 0; i < 16; i++) {
            page[i] = 0;
            (void)read_reg((uint8_t)(base + i), &page[i]);
            off += snprintf(line + off, sizeof(line) - (size_t)off, "%02X ", page[i]);
        }
        ESP_LOGI(TAG, "dump %02X: %s", base, line);
    }
}

static esp_err_t field_on_nfca(void);

static const char *band_of(unsigned score)
{
    if (score >= 80) {
        return "GOOD";
    }
    if (score >= 50) {
        return "FAIR";
    }
    return "POOR";
}

void nfc_driver_rf_selftest(void)
{
    uint8_t id = 0;
    uint8_t op = 0;
    uint8_t io2 = 0;
    uint8_t txdrv = 0;
    uint8_t regu = 0;
    uint8_t irq_t = 0;
    uint8_t aux30 = 0;
    uint8_t aux31 = 0;
    uint8_t amp = 0;
    uint8_t phase = 0;
    uint8_t cap = 0;
    uint8_t vdd_adc = 0;
    unsigned score = 0;
    bool osc_ok = false;
    bool analog_ok = false;
    bool field_ok = false;

    if (!s_inited) {
        ESP_LOGW(TAG, "RF selftest skipped: NFC not ready");
        return;
    }

    ESP_LOGI(TAG, "======== NFC RF selftest ========");

    if (read_reg(REG_IC_IDENTITY, &id) == ESP_OK &&
        ((id & IC_TYPE_MASK) == IC_TYPE_3916 || (id & IC_TYPE_MASK) == IC_TYPE_3916B)) {
        score += 25;
        ESP_LOGI(TAG, "chip     IC=0x%02X type=0x%02X  OK", id, id & IC_TYPE_MASK);
    } else {
        ESP_LOGW(TAG, "chip     IC=0x%02X  unexpected", id);
    }

    (void)read_reg(REG_IO_CONF2, &io2);
    (void)read_reg(REG_OP_CONTROL, &op);
    osc_ok = s_osc_irq_ok || ((op & OP_EN) != 0);
    if (osc_ok) {
        score += 20;
        ESP_LOGI(TAG, "osc      %s  op=0x%02X io2=0x%02X",
                 s_osc_irq_ok ? "IRQ latched" : "EN set", op, io2);
    } else {
        ESP_LOGW(TAG, "osc      no IRQ and EN off  op=0x%02X io2=0x%02X", op, io2);
    }

    (void)read_reg(REG_TX_DRIVER, &txdrv);
    (void)read_reg(REG_REGULATOR_RESULT, &regu);
    analog_ok = (regu != 0);
    if (analog_ok) {
        score += 15;
    }
    ESP_LOGI(TAG, "tx/reg   driver=0x%02X  regulator=0x%02X  %s",
             txdrv, regu, analog_ok ? "OK" : "unset");

    if (field_on_nfca() == ESP_OK &&
        read_reg(REG_OP_CONTROL, &op) == ESP_OK &&
        (op & (OP_EN | OP_TX_EN)) == (OP_EN | OP_TX_EN)) {
        field_ok = true;
        score += 25;
        ESP_LOGI(TAG, "field    ON   op=0x%02X  same path as poll / phone popup", op);
    } else {
        ESP_LOGW(TAG, "field    not confirmed  op=0x%02X", op);
    }

    (void)read_reg(REG_AUX_DISPLAY, &aux30);
    (void)read_reg(0x31, &aux31);
    (void)measure_cmd(ST25R_CMD_MEASURE_VDD, &vdd_adc, &irq_t);
    (void)measure_cmd(ST25R_CMD_MEASURE_AMP, &amp, &irq_t);
    (void)measure_cmd(ST25R_CMD_MEASURE_PHASE, &phase, NULL);
    (void)measure_cmd(ST25R_CMD_MEASURE_CAP, &cap, NULL);
    ESP_LOGI(TAG, "telem    aux30=0x%02X aux31=0x%02X vdd=0x%02X amp=0x%02X phase=0x%02X cap=0x%02X irqT=0x%02X",
             aux30, aux31, vdd_adc, amp, phase, cap, irq_t);
    if (aux30 == 0 && vdd_adc == 0 && amp == 0) {
        ESP_LOGI(TAG, "telem    AD/AUX stay 0 on I2C — not used for antenna judge");
    }

    dump_regs();
    field_idle();

    ESP_LOGI(TAG, "score    %u/100  %s", score, band_of(score));
    if (field_ok && osc_ok) {
        ESP_LOGI(TAG, "verdict  RF path OK (chip + osc + TX). Phone tap popup is the live check.");
    } else if (score >= 50) {
        ESP_LOGW(TAG, "verdict  digital OK, field not confirmed in this pass");
    } else {
        ESP_LOGW(TAG, "verdict  chip/I2C problem");
    }
    ESP_LOGI(TAG, "================================");
}

static esp_err_t transceive_to(const uint8_t *tx, size_t tx_len, bool crc,
                               uint8_t *rx, size_t rx_max, size_t *rx_len,
                               uint32_t timeout_ms)
{
    uint8_t irq = 0;
    uint8_t fifo_bytes = 0;
    const uint16_t bits = (uint16_t)(tx_len * 8);

    ESP_RETURN_ON_ERROR(send_cmd(ST25R_CMD_CLEAR_FIFO), TAG, "clr fifo");
    ESP_RETURN_ON_ERROR(clear_irqs(), TAG, "clr irq");
    if (tx_len > 0) {
        ESP_RETURN_ON_ERROR(fifo_write(tx, tx_len), TAG, "fifo wr");
        ESP_RETURN_ON_ERROR(write_reg(REG_NUM_TX_BYTES2, (uint8_t)(bits & 0x07)), TAG, "ntx2");
        ESP_RETURN_ON_ERROR(write_reg(REG_NUM_TX_BYTES1, (uint8_t)(bits >> 3)), TAG, "ntx1");
        ESP_RETURN_ON_ERROR(send_cmd(crc ? ST25R_CMD_TX_CRC : ST25R_CMD_TX_NOCRC), TAG, "tx");
    }

    if (wait_irq_main(IRQ_RXE | IRQ_COL, timeout_ms, &irq) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    if (irq & IRQ_COL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_RETURN_ON_ERROR(read_reg(REG_FIFO_STATUS1, &fifo_bytes), TAG, "fifo st");
    fifo_bytes &= 0x7F;
    if (fifo_bytes == 0 || fifo_bytes > rx_max) {
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_RETURN_ON_ERROR(fifo_read(rx, fifo_bytes), TAG, "fifo rd");
    *rx_len = fifo_bytes;
    return ESP_OK;
}

static esp_err_t transceive(const uint8_t *tx, size_t tx_len, bool crc,
                            uint8_t *rx, size_t rx_max, size_t *rx_len)
{
    return transceive_to(tx, tx_len, crc, rx, rx_max, rx_len, 20);
}

static esp_err_t field_on_nfca(void)
{
    ESP_RETURN_ON_ERROR(send_cmd(ST25R_CMD_STOP), TAG, "stop");
    ESP_RETURN_ON_ERROR(send_cmd(ST25R_CMD_CLEAR_FIFO), TAG, "fifo");
    ESP_RETURN_ON_ERROR(write_reg(REG_MODE, MODE_OM_NFCA), TAG, "mode A");
    ESP_RETURN_ON_ERROR(write_reg(REG_BIT_RATE, 0x00), TAG, "106k");
    ESP_RETURN_ON_ERROR(write_reg(REG_ISO14443A, 0x80), TAG, "antcl");
    ESP_RETURN_ON_ERROR(write_reg(REG_MASK_RX_TIMER, 0x0E), TAG, "mrt");
    ESP_RETURN_ON_ERROR(write_reg(REG_NRT1, 0x20), TAG, "nrt");
    ESP_RETURN_ON_ERROR(write_reg(REG_NRT2, 0x00), TAG, "nrt");
    ESP_RETURN_ON_ERROR(write_reg(REG_RX_CONF1, 0x08), TAG, "rx1");
    ESP_RETURN_ON_ERROR(write_reg(REG_RX_CONF2, 0x2D), TAG, "rx2");
    ESP_RETURN_ON_ERROR(write_reg(REG_OP_CONTROL, OP_EN | OP_RX_EN | OP_TX_EN), TAG, "op");
    delay_ms(5);
    return clear_irqs();
}

static esp_err_t select_cascade(uint8_t cl, uint8_t *uid4, uint8_t *sak)
{
    uint8_t cl_frame[5] = {0};
    uint8_t sel[7];
    uint8_t sak_buf[3] = {0};
    size_t n = 0;
    const uint8_t anticol[2] = { cl, 0x20 };

    if (transceive(anticol, sizeof(anticol), false, cl_frame, sizeof(cl_frame), &n) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    if (n < 5) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(uid4, cl_frame, 4);

    sel[0] = cl;
    sel[1] = 0x70;
    memcpy(&sel[2], cl_frame, 5);
    if (transceive(sel, 7, true, sak_buf, sizeof(sak_buf), &n) != ESP_OK || n == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *sak = sak_buf[0];
    return ESP_OK;
}

static esp_err_t activate_type_a(nfc_dump_t *d)
{
    uint8_t irq = 0;
    uint8_t fifo_bytes = 0;
    uint8_t atqa[2] = {0};
    uint8_t uid4[4] = {0};
    uint8_t sak = 0;

    ESP_RETURN_ON_ERROR(field_on_nfca(), TAG, "field A");
    ESP_RETURN_ON_ERROR(send_cmd(ST25R_CMD_CLEAR_FIFO), TAG, "fifo");
    ESP_RETURN_ON_ERROR(clear_irqs(), TAG, "irq");
    ESP_RETURN_ON_ERROR(send_cmd(ST25R_CMD_UNMASK_RX), TAG, "unmask");
    ESP_RETURN_ON_ERROR(send_cmd(ST25R_CMD_TX_REQA), TAG, "REQA");

    if (wait_irq_main(IRQ_RXE, 20, &irq) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(read_reg(REG_FIFO_STATUS1, &fifo_bytes), TAG, "atqa len");
    fifo_bytes &= 0x7F;
    if (fifo_bytes < 2) {
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_RETURN_ON_ERROR(fifo_read(atqa, 2), TAG, "atqa");

    d->tech = NFC_TECH_A;
    d->atqa[0] = atqa[0];
    d->atqa[1] = atqa[1];

    if (select_cascade(0x93, uid4, &sak) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    if (uid4[0] == 0x88) {
        memcpy(d->uid, &uid4[1], 3);
        d->uid_len = 3;
        if (sak & SAK_CASCADE) {
            ESP_RETURN_ON_ERROR(select_cascade(0x95, uid4, &sak), TAG, "CL2");
            memcpy(&d->uid[3], uid4, 4);
            d->uid_len = 7;
            if (sak & SAK_CASCADE) {
                ESP_RETURN_ON_ERROR(select_cascade(0x97, uid4, &sak), TAG, "CL3");
                memcpy(&d->uid[7], uid4, 3);
                d->uid_len = 10;
            }
        }
    } else {
        memcpy(d->uid, uid4, 4);
        d->uid_len = 4;
    }
    d->sak = sak;
    (void)write_reg(REG_ISO14443A, 0x00);

    if (sak & SAK_ISODEP) {
        d->profile = NFC_PROFILE_T4T;
    } else if (sak & SAK_MIFARE) {
        d->profile = NFC_PROFILE_MFC;
        set_note(d, "Classic encrypted, UID only");
    } else {
        d->profile = NFC_PROFILE_T2T;
    }
    return ESP_OK;
}

static uint16_t t2t_pages_from_version(const uint8_t *ver)
{
    switch (ver[6]) {
    case 0x0B: return 20;
    case 0x0E: return 41;
    case 0x0F: return 45;
    case 0x11: return 135;
    case 0x13: return 231;
    default: return 64;
    }
}

static void dump_t2t(nfc_dump_t *d)
{
    uint8_t rx[20];
    size_t n = 0;
    const uint8_t get_ver[] = { 0x60 };
    uint16_t pages = 64;

    if (transceive_to(get_ver, sizeof(get_ver), true, rx, sizeof(rx), &n, 30) == ESP_OK &&
        n >= 8) {
        memcpy(d->version, rx, 8);
        d->version_len = 8;
        pages = t2t_pages_from_version(rx);
    }

    d->block_size = 4;
    for (uint16_t page = 0; page < pages && d->mem_len + 16 <= NFC_DUMP_MAX; page += 4) {
        uint8_t cmd[2] = { 0x30, (uint8_t)page };
        if (transceive_to(cmd, sizeof(cmd), true, rx, sizeof(rx), &n, 20) != ESP_OK || n < 16) {
            break;
        }
        memcpy(&d->mem[d->mem_len], rx, 16);
        d->mem_len += 16;
        d->block_count = (uint16_t)(d->mem_len / 4);
    }

    if (d->mem_len == 0) {
        set_note(d, "T2T READ failed");
    } else {
        set_note(d, "T2T user memory dumped");
    }
}

static bool apdu_ok(const uint8_t *rx, size_t n)
{
    return n >= 3 && rx[n - 2] == 0x90 && rx[n - 1] == 0x00;
}

static void dump_t4t_ndef(nfc_dump_t *d)
{
    uint8_t rx[64];
    size_t n = 0;
    const uint8_t rats[] = { 0xE0, 0x80 };
    const uint8_t sel_app[] = {
        0x02, 0x00, 0xA4, 0x04, 0x00, 0x07,
        0xD2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x01, 0x00
    };
    const uint8_t sel_cc[] = { 0x03, 0x00, 0xA4, 0x00, 0x0C, 0x02, 0xE1, 0x03 };
    const uint8_t rd_cc[] = { 0x02, 0x00, 0xB0, 0x00, 0x00, 0x0F };

    if (transceive_to(rats, sizeof(rats), true, rx, sizeof(rx), &n, 40) != ESP_OK) {
        set_note(d, "T4T RATS failed");
        return;
    }

    if (transceive_to(sel_app, sizeof(sel_app), true, rx, sizeof(rx), &n, 40) != ESP_OK ||
        !apdu_ok(rx, n)) {
        set_note(d, "T4T NDEF select failed");
        return;
    }
    if (transceive_to(sel_cc, sizeof(sel_cc), true, rx, sizeof(rx), &n, 40) != ESP_OK ||
        !apdu_ok(rx, n)) {
        set_note(d, "T4T CC select failed");
        return;
    }
    if (transceive_to(rd_cc, sizeof(rd_cc), true, rx, sizeof(rx), &n, 40) != ESP_OK ||
        n < 17 || !apdu_ok(rx, n)) {
        set_note(d, "T4T CC read failed");
        return;
    }

    const uint8_t *cc = &rx[1];
    uint8_t file_id[2] = { 0xE1, 0x04 };
    uint16_t ndef_max = 128;
    if (n >= 17) {
        file_id[0] = cc[9];
        file_id[1] = cc[10];
        ndef_max = (uint16_t)((cc[11] << 8) | cc[12]);
        if (ndef_max == 0 || ndef_max > NFC_NDEF_MAX) {
            ndef_max = NFC_NDEF_MAX;
        }
    }

    uint8_t sel_ndef[] = { 0x03, 0x00, 0xA4, 0x00, 0x0C, 0x02, file_id[0], file_id[1] };
    if (transceive_to(sel_ndef, sizeof(sel_ndef), true, rx, sizeof(rx), &n, 40) != ESP_OK ||
        !apdu_ok(rx, n)) {
        set_note(d, "T4T NDEF file select failed");
        return;
    }

    uint8_t rd_len[] = { 0x02, 0x00, 0xB0, 0x00, 0x00, 0x02 };
    if (transceive_to(rd_len, sizeof(rd_len), true, rx, sizeof(rx), &n, 40) != ESP_OK ||
        n < 5 || !apdu_ok(rx, n)) {
        set_note(d, "T4T NLEN failed");
        return;
    }
    uint16_t ndef_len = (uint16_t)((rx[1] << 8) | rx[2]);
    if (ndef_len == 0) {
        set_note(d, "T4T empty NDEF");
        return;
    }
    if (ndef_len > ndef_max) {
        ndef_len = ndef_max;
    }

    uint8_t rd_ndef[] = { 0x03, 0x00, 0xB0, 0x00, 0x00, (uint8_t)(ndef_len + 2) };
    if (transceive_to(rd_ndef, sizeof(rd_ndef), true, rx, sizeof(rx), &n, 50) != ESP_OK ||
        n < (size_t)(ndef_len + 3) || !apdu_ok(rx, n)) {
        set_note(d, "T4T NDEF read failed");
        return;
    }

    size_t copy = n - 3;
    if (copy > NFC_NDEF_MAX) {
        copy = NFC_NDEF_MAX;
    }
    memcpy(d->ndef, &rx[1], copy);
    d->ndef_len = (uint16_t)copy;
    if (copy > 2 && copy - 2 <= NFC_DUMP_MAX) {
        memcpy(d->mem, &rx[3], copy - 2);
        d->mem_len = (uint16_t)(copy - 2);
    }
    set_note(d, "T4T NDEF dumped");
}

static esp_err_t capture_type_a(nfc_dump_t *d)
{
    esp_err_t err = activate_type_a(d);
    if (err != ESP_OK) {
        return err;
    }
    if (d->profile == NFC_PROFILE_T2T) {
        dump_t2t(d);
    } else if (d->profile == NFC_PROFILE_T4T) {
        dump_t4t_ndef(d);
    }
    return ESP_OK;
}

static esp_err_t field_on_nfcv(void)
{
    ESP_RETURN_ON_ERROR(send_cmd(ST25R_CMD_STOP), TAG, "stop");
    ESP_RETURN_ON_ERROR(write_reg(REG_MODE, MODE_OM_ISO15693), TAG, "mode V");
    ESP_RETURN_ON_ERROR(write_reg(REG_BIT_RATE, 0x00), TAG, "br");
    ESP_RETURN_ON_ERROR(write_reg(REG_OP_CONTROL, OP_EN | OP_RX_EN | OP_TX_EN), TAG, "op");
    delay_ms(5);
    return clear_irqs();
}

static bool nfcv_read_block(const uint8_t *uid, uint8_t flags, uint8_t block,
                            uint8_t *out, uint8_t *out_len)
{
    uint8_t tx[12];
    uint8_t rx[16];
    size_t n = 0;
    size_t tlen = 0;

    tx[tlen++] = flags;
    tx[tlen++] = 0x20;
    if (flags & 0x20) {
        memcpy(&tx[tlen], uid, 8);
        tlen += 8;
    }
    tx[tlen++] = block;

    if (transceive_to(tx, tlen, true, rx, sizeof(rx), &n, 30) != ESP_OK || n < 5) {
        return false;
    }
    if (rx[0] & 0x01) {
        return false;
    }
    uint8_t data_off = 1;
    uint8_t dlen = (uint8_t)(n - 1);
    if (dlen > 8) {
        dlen = 8;
    }
    memcpy(out, &rx[data_off], dlen);
    *out_len = dlen;
    return true;
}

static void dump_nfcv(nfc_dump_t *d)
{
    uint8_t rx[24];
    size_t n = 0;
    uint8_t flags = 0x22;
    uint8_t sys[11] = { 0x22, 0x2B };
    memcpy(&sys[2], d->uid, 8);

    d->profile = NFC_PROFILE_NFCV;
    d->block_size = 4;
    d->block_count = 32;

    if (transceive_to(sys, 10, true, rx, sizeof(rx), &n, 30) == ESP_OK && n >= 10 &&
        (rx[0] & 0x01) == 0) {
        size_t i = 10;
        uint8_t info = rx[1];
        if (info & 0x01) {
            i += 1;
        }
        if (info & 0x02) {
            i += 1;
        }
        if ((info & 0x04) && i + 1 < n) {
            d->block_count = (uint16_t)(rx[i] + 1);
            d->block_size = (uint8_t)((rx[i + 1] & 0x1F) + 1);
        }
    } else {
        flags = 0x02;
        sys[0] = 0x02;
        if (transceive_to(sys, 2, true, rx, sizeof(rx), &n, 30) == ESP_OK && n >= 2) {
            flags = 0x02;
        } else {
            flags = 0x22;
        }
    }

    if (d->block_count == 0) {
        d->block_count = 32;
    }
    if ((uint32_t)d->block_count * d->block_size > NFC_DUMP_MAX) {
        d->block_count = (uint16_t)(NFC_DUMP_MAX / (d->block_size ? d->block_size : 4));
    }

    for (uint16_t b = 0; b < d->block_count; b++) {
        uint8_t blk[8];
        uint8_t blen = 0;
        if (!nfcv_read_block(d->uid, flags, (uint8_t)b, blk, &blen)) {
            break;
        }
        if (d->mem_len + blen > NFC_DUMP_MAX) {
            break;
        }
        memcpy(&d->mem[d->mem_len], blk, blen);
        d->mem_len = (uint16_t)(d->mem_len + blen);
        d->block_size = blen;
    }
    d->block_count = d->block_size ? (uint16_t)(d->mem_len / d->block_size) : 0;
    set_note(d, d->mem_len ? "NFCV blocks dumped (602)" : "NFCV UID only (601)");
}

static esp_err_t capture_type_v(nfc_dump_t *d)
{
    uint8_t rx[16];
    size_t n = 0;
    const uint8_t inventory[] = { 0x26, 0x01, 0x00 };

    ESP_RETURN_ON_ERROR(field_on_nfcv(), TAG, "field V");
    if (transceive_to(inventory, sizeof(inventory), true, rx, sizeof(rx), &n, 25) != ESP_OK ||
        n < 9) {
        return ESP_ERR_NOT_FOUND;
    }

    d->tech = NFC_TECH_V;
    d->uid_len = 8;
    if (n >= 10) {
        memcpy(d->uid, &rx[2], 8);
    } else {
        memcpy(d->uid, &rx[1], 8);
    }
    dump_nfcv(d);
    return ESP_OK;
}

static esp_err_t capture_type_b(nfc_dump_t *d)
{
    uint8_t rx[16];
    size_t n = 0;
    const uint8_t reqb[] = { 0x05, 0x00, 0x00 };

    ESP_RETURN_ON_ERROR(send_cmd(ST25R_CMD_STOP), TAG, "stop");
    ESP_RETURN_ON_ERROR(write_reg(REG_MODE, MODE_OM_NFCB), TAG, "mode B");
    ESP_RETURN_ON_ERROR(write_reg(REG_BIT_RATE, 0x00), TAG, "106k");
    ESP_RETURN_ON_ERROR(write_reg(REG_OP_CONTROL, OP_EN | OP_RX_EN | OP_TX_EN), TAG, "op");
    delay_ms(5);
    (void)clear_irqs();

    if (transceive_to(reqb, sizeof(reqb), true, rx, sizeof(rx), &n, 20) != ESP_OK || n < 5) {
        return ESP_ERR_NOT_FOUND;
    }
    if (rx[0] != 0x50) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    d->tech = NFC_TECH_B;
    d->profile = NFC_PROFILE_TYPEB;
    d->uid_len = 4;
    memcpy(d->uid, &rx[1], 4);
    set_note(d, "Type B PUPI (613)");
    return ESP_OK;
}

static esp_err_t capture_type_f(nfc_dump_t *d)
{
    uint8_t rx[24];
    size_t n = 0;
    const uint8_t sensf[] = { 0x06, 0x00, 0xFF, 0xFF, 0x00, 0x00 };

    ESP_RETURN_ON_ERROR(send_cmd(ST25R_CMD_STOP), TAG, "stop");
    ESP_RETURN_ON_ERROR(write_reg(REG_MODE, MODE_OM_NFCF), TAG, "mode F");
    ESP_RETURN_ON_ERROR(write_reg(REG_BIT_RATE, 0x11), TAG, "212k");
    ESP_RETURN_ON_ERROR(write_reg(REG_OP_CONTROL, OP_EN | OP_RX_EN | OP_TX_EN), TAG, "op");
    delay_ms(5);
    (void)clear_irqs();

    if (transceive_to(sensf, sizeof(sensf), false, rx, sizeof(rx), &n, 20) != ESP_OK || n < 10) {
        return ESP_ERR_NOT_FOUND;
    }

    d->tech = NFC_TECH_F;
    d->profile = NFC_PROFILE_TYPEF;
    d->uid_len = 8;
    memcpy(d->uid, &rx[2], 8);
    set_note(d, "Type F IDm (614)");
    return ESP_OK;
}

static bool same_dump(const nfc_dump_t *a, const nfc_dump_t *b)
{
    return a->tech == b->tech &&
           a->uid_len == b->uid_len &&
           memcmp(a->uid, b->uid, a->uid_len) == 0;
}

static void write_hex_lines(const uint8_t *data, size_t len, uint8_t width)
{
    char line[80];
    if (width == 0) {
        width = 16;
    }
    for (size_t i = 0; i < len; i += width) {
        size_t chunk = (len - i) > width ? width : (len - i);
        int off = snprintf(line, sizeof(line), "%04X:", (unsigned)i);
        for (size_t b = 0; b < chunk && off > 0 && (size_t)off < sizeof(line) - 4; b++) {
            off += snprintf(line + off, sizeof(line) - (size_t)off, " %02X", data[i + b]);
        }
        if (off > 0 && (size_t)off < sizeof(line) - 2) {
            line[off++] = '\n';
            line[off] = '\0';
        }
        (void)tf_card_print(line);
    }
}

static void save_dump(const nfc_dump_t *d)
{
    char uid[40];
    char hdr[320];
    format_uid(d->uid, d->uid_len, uid, sizeof(uid));

    if (!tf_card_is_mounted()) {
        ESP_LOGW(TAG, "TF not mounted, skip dump UID %s", uid);
        return;
    }

    int n = snprintf(hdr, sizeof(hdr),
                     "# NFC copy dump\n"
                     "uptime_ms=%llu\n"
                     "tech=%s\n"
                     "profile=%s\n"
                     "uid=%s\n"
                     "sak=0x%02X\n"
                     "atqa=0x%02X%02X\n"
                     "block_size=%u\n"
                     "block_count=%u\n"
                     "mem_len=%u\n"
                     "ndef_len=%u\n"
                     "note=%s\n"
                     "\n",
                     (unsigned long long)(esp_timer_get_time() / 1000ULL),
                     tech_name(d->tech),
                     profile_name(d->profile),
                     uid,
                     d->sak,
                     d->atqa[1], d->atqa[0],
                     d->block_size,
                     d->block_count,
                     d->mem_len,
                     d->ndef_len,
                     d->note);
    if (n <= 0) {
        return;
    }

    if (tf_card_begin_write(TF_CARD_NFC_DUMP, false) == ESP_OK) {
        (void)tf_card_print(hdr);
        if (d->version_len) {
            (void)tf_card_print("[version]\n");
            write_hex_lines(d->version, d->version_len, 8);
            (void)tf_card_print("\n");
        }
        if (d->mem_len) {
            (void)tf_card_print("[memory]\n");
            write_hex_lines(d->mem, d->mem_len, d->block_size ? d->block_size : 16);
        }
        if (d->ndef_len) {
            (void)tf_card_print("\n[ndef]\n");
            write_hex_lines(d->ndef, d->ndef_len, 16);
        }
        (void)tf_card_end_write();
    }

    if (d->mem_len && tf_card_begin_write(TF_CARD_NFC_BIN, false) == ESP_OK) {
        (void)tf_card_write_raw(d->mem, d->mem_len);
        (void)tf_card_end_write();
    } else if (d->ndef_len && tf_card_begin_write(TF_CARD_NFC_BIN, false) == ESP_OK) {
        (void)tf_card_write_raw(d->ndef, d->ndef_len);
        (void)tf_card_end_write();
    }

    snprintf(hdr, sizeof(hdr),
             "---- uptime %llums ----\n"
             "tech=%s profile=%s uid=%s mem=%u ndef=%u note=%s\n\n",
             (unsigned long long)(esp_timer_get_time() / 1000ULL),
             tech_name(d->tech), profile_name(d->profile), uid,
             d->mem_len, d->ndef_len, d->note);
    (void)tf_card_write_text(TF_CARD_NFC_LOG, hdr, true);
    (void)tf_card_write_text(TF_CARD_NFC_LAST, hdr, false);
    ESP_LOGI(TAG, "copied %s %s UID %s  mem=%u ndef=%u -> TF /nfc",
             tech_name(d->tech), profile_name(d->profile), uid,
             d->mem_len, d->ndef_len);
}

esp_err_t nfc_driver_capture(nfc_dump_t *dump)
{
    if (!s_inited || dump == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(dump, 0, sizeof(*dump));

    if (capture_type_a(dump) == ESP_OK) {
        field_idle();
        return ESP_OK;
    }
    if (capture_type_v(dump) == ESP_OK) {
        field_idle();
        return ESP_OK;
    }
    if (capture_type_b(dump) == ESP_OK) {
        field_idle();
        return ESP_OK;
    }
    if (capture_type_f(dump) == ESP_OK) {
        field_idle();
        return ESP_OK;
    }
    field_idle();
    return ESP_ERR_NOT_FOUND;
}

static void nfc_poll_task(void *arg)
{
    (void)arg;
    nfc_dump_t last = {0};
    bool have_last = false;

    ESP_LOGI(TAG, "test loop: A(616/T2T/T4T) V(601/602) B(613) F(614)");
    while (1) {
        memset(&s_dump, 0, sizeof(s_dump));
        if (nfc_driver_capture(&s_dump) == ESP_OK) {
            char uid[40];
            format_uid(s_dump.uid, s_dump.uid_len, uid, sizeof(uid));
            ESP_LOGI(TAG, "%s/%s UID %s mem=%u %s",
                     tech_name(s_dump.tech), profile_name(s_dump.profile),
                     uid, s_dump.mem_len, s_dump.note);
            if (!have_last || !same_dump(&s_dump, &last)) {
                save_dump(&s_dump);
                last = s_dump;
                have_last = true;
            }
        } else {
            have_last = false;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

esp_err_t nfc_driver_init(void)
{
    uint8_t id = 0;

    if (s_inited) {
        return ESP_OK;
    }
    if (board_i2c_bus() == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "init with NFC_EN=%d, scan leftover addresses", board_nfc_en_pin_level());
    board_i2c_scan("nfc-init");

    static const uint8_t candidates[] = { ST25R_I2C_ADDR, 0x5A, 0x2A, 0x28 };
    uint8_t addr = 0;
    for (size_t i = 0; i < sizeof(candidates); i++) {
        if (candidates[i] == I2C_ADDR_AXP2101 ||
            candidates[i] == I2C_ADDR_MSA321 ||
            candidates[i] == I2C_ADDR_PCF8563) {
            continue;
        }
        board_i2c_lock();
        esp_err_t probe = i2c_master_probe(board_i2c_bus(), candidates[i], 20);
        board_i2c_unlock();
        if (probe == ESP_OK) {
            addr = candidates[i];
            break;
        }
        if (probe == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "I2C hung while probing 0x%02X", candidates[i]);
            return probe;
        }
    }
    if (addr == 0) {
        ESP_LOGE(TAG, "ST25R3916 not found, EN=%d", board_nfc_en_pin_level());
        return ESP_ERR_NOT_FOUND;
    }

    i2c_device_config_t devcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = ST25R_I2C_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(board_i2c_bus(), &devcfg, &s_dev),
                        TAG, "add i2c");
    ESP_LOGI(TAG, "use I2C 0x%02X", addr);

    ESP_RETURN_ON_ERROR(send_cmd(ST25R_CMD_SET_DEFAULT), TAG, "default");
    delay_ms(2);
    ESP_RETURN_ON_ERROR(write_reg(REG_IO_CONF2, IO_CONF2_I2C_3V3), TAG, "io2");
    ESP_RETURN_ON_ERROR(write_reg(REG_OP_CONTROL, OP_EN), TAG, "osc");
    s_osc_irq_ok = (wait_irq_main(IRQ_OSC, 20, NULL) == ESP_OK);
    if (!s_osc_irq_ok) {
        ESP_LOGW(TAG, "OSC ready timeout, continue");
    }

    ESP_RETURN_ON_ERROR(read_reg(REG_IC_IDENTITY, &id), TAG, "id");
    ESP_LOGI(TAG, "IC_IDENTITY=0x%02X type=0x%02X", id, id & IC_TYPE_MASK);
    if ((id & IC_TYPE_MASK) != IC_TYPE_3916 && (id & IC_TYPE_MASK) != IC_TYPE_3916B) {
        ESP_LOGW(TAG, "unexpected IC type, still try polling");
    }

    (void)send_cmd(ST25R_CMD_ADJUST_REG);
    delay_ms(5);
    ESP_RETURN_ON_ERROR(write_reg(REG_TX_DRIVER, 0x80), TAG, "tx drv");
    ESP_RETURN_ON_ERROR(write_reg(REG_IRQ_MASK_MAIN, 0x00), TAG, "imain");
    ESP_RETURN_ON_ERROR(write_reg(REG_IRQ_MASK_TIMER, 0x00), TAG, "itimer");
    ESP_RETURN_ON_ERROR(write_reg(REG_IRQ_MASK_ERROR, 0x00), TAG, "ierr");
    ESP_RETURN_ON_ERROR(write_reg(REG_IRQ_MASK_TARGET, 0xFF), TAG, "itgt");
    (void)clear_irqs();

    s_inited = true;
    nfc_driver_rf_selftest();
    if (xTaskCreate(nfc_poll_task, "nfc_poll", 10240, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ST25R3916 ready  EN=%d IRQ=%d I2C=0x%02X",
             board_nfc_en_pin_level(), (int)nfc_irq, addr);
    return ESP_OK;
}

esp_err_t nfc_driver_poll(nfc_tag_t *tag)
{
    if (!s_inited || tag == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(tag, 0, sizeof(*tag));
    if (nfc_driver_capture(&s_dump) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    tag->tech = s_dump.tech;
    tag->uid_len = s_dump.uid_len;
    tag->sak = s_dump.sak;
    memcpy(tag->uid, s_dump.uid, s_dump.uid_len);
    memcpy(tag->atqa, s_dump.atqa, 2);
    return ESP_OK;
}
