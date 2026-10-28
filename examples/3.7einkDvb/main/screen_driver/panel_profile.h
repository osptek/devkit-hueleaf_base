#ifndef EINK_PANEL_PROFILE_H
#define EINK_PANEL_PROFILE_H

#include <stdint.h>

/* Verified EPD037A02 480x720 settings, historically named JD7601.
 * IST7601CA0/IST7513 references share base opcodes but have different init
 * sequences. Do not substitute their panel parameters or infer LUT fields.
 * A future panel needs its own verified profile and physical dimensions. */
#define EPD037_PROFILE_NAME "epd037a02-480x720-v1"
#define EPD037_SPI_HZ (2 * 1000 * 1000)
#define EPD037_BUSY_TIMEOUT_MS 120000
#define EPD037_DRF_ASSERT_MS 150

typedef struct {
    uint8_t command;
    uint8_t value;
} panel_init_command_t;

/* Preserve the existing six-ink panel initialization. It uses a fixed
 * temperature input rather than live panel sensing; refresh uses OTP. */
static const panel_init_command_t k_epd037_init[] = {
    {0xE9, 0x01}, {0x30, 0x04}, {0x41, 0x00}, {0xE0, 0x02}, {0xE6, 0x19},
};

#endif
