#ifndef MSA321_H
#define MSA321_H

#include <stdint.h>
#include "esp_err.h"

typedef struct {
    int16_t raw_x;
    int16_t raw_y;
    int16_t raw_z;
    float x_g;
    float y_g;
    float z_g;
} msa321_sample_t;

esp_err_t msa321_init(void);
esp_err_t msa321_read(msa321_sample_t *sample);

#endif /* MSA321_H */
