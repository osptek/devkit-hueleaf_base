#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_err.h"

#define BOARD_SPI_HOST  SPI2_HOST
#define BOARD_I2C_PORT  I2C_NUM_0

#define I2C_ADDR_AXP2101    0x34
#define I2C_ADDR_MSA321     0x62
#define I2C_ADDR_PCF8563    0x51
#define I2C_ADDR_ST25R3916  0x50

esp_err_t board_init(void);
spi_host_device_t board_spi_host(void);
i2c_master_bus_handle_t board_i2c_bus(void);
void board_i2c_lock(void);
void board_i2c_unlock(void);
void board_spi_lock(void);
void board_spi_unlock(void);
void board_tf_cs_idle(void);
int board_i2c_scan(const char *why);
void board_nfc_set_enable(bool enable);
int board_nfc_en_pin_level(void);

#endif /* BOARD_H */
