#pragma once
/* host preview stub: only the esp-dsp entry points the screens call */
#include "esp_err.h"
esp_err_t dsps_fft2r_init_fc32(float *fft_table_buff, int table_size);
esp_err_t dsps_fft2r_fc32(float *data, int N);
esp_err_t dsps_bit_rev_fc32(float *data, int N);
void dsps_wind_hann_f32(float *window, int len);
