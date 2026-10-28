#ifndef PANEL_LUT_H
#define PANEL_LUT_H

#include <stdint.h>

/* Select the panel's built-in six-ink OTP waveform; no RAM LUT is supplied. */
static const uint8_t k_lut_otp_setting[] = { 0x0F, 0x29 };

#endif /* PANEL_LUT_H */
