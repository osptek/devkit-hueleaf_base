#ifndef PIN_MAP_H
#define PIN_MAP_H

#include "driver/gpio.h"

#ifndef EINK_ROTATION
#define EINK_ROTATION 0 /* 0 or 180 degrees 更改此处设置默认屏幕方向 */
#endif
#define EINK_ROTATE_BUTTON GPIO_NUM_28
#define EINK_ROTATE_BUTTON_ACTIVE_LEVEL 0
#if EINK_ROTATION != 0 && EINK_ROTATION != 90 && EINK_ROTATION != 180 && EINK_ROTATION != 270
#error "EINK_ROTATION must be 0, 90, 180 or 270"
#endif
#define EINK_LOGICAL_X(x) (EINK_ROTATION == 180 ? 479 - (x) : (x))
#define EINK_LOGICAL_Y(y) (EINK_ROTATION == 180 ? 719 - (y) : (y))

/* 3.7" E6 480x720 JD7601 */
#define eink_sda    GPIO_NUM_0
#define eink_clk    GPIO_NUM_1
#define eink_cs     GPIO_NUM_5
#define eink_dc     GPIO_NUM_6
#define eink_rst    GPIO_NUM_7
#define eink_busy   GPIO_NUM_8

#define i2c0_sda    GPIO_NUM_2
#define i2c0_scl    GPIO_NUM_3

#define imu_irq     GPIO_NUM_4
#define pmic_irq    GPIO_NUM_25 /* AXP2101 nIRQ，低有效，下降沿通知任务读取中断状态 */
#define nfc_irq     GPIO_NUM_24
#define rtc_irq     GPIO_NUM_23

#define nfc_en      GPIO_NUM_26

/* 复用同一条 SPI2 */
#define spi2_sdi    eink_sda
#define spi2_sck    eink_clk
#define spi2_sdo    GPIO_NUM_9
#define spi2_tf_cs  GPIO_NUM_10

#endif /* PIN_MAP_H */
