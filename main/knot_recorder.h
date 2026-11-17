#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
/* Microphone -> IMA ADPCM -> audio slot, on its own task (bsp_audio_read blocks). */
esp_err_t knot_recorder_init(void);
esp_err_t knot_recorder_start(unsigned slot, uint32_t seq);
void knot_recorder_stop(void);              /* asynchronous; poll for completion */
bool knot_recorder_active(void);
uint32_t knot_recorder_elapsed_ms(void);
/* True once after a recording ended and its header was sealed (bytes 0 = nothing usable). */
bool knot_recorder_poll(uint32_t *seq, uint32_t *bytes, uint32_t *crc);
