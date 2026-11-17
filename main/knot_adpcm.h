#pragma once
#include <stddef.h>
#include <stdint.h>
/* IMA ADPCM, one continuous mono stream, 2 samples per byte (low nibble first).
 * Encoder state starts at predictor 0 / index 0; the receiver decodes with the same rule. */
typedef struct { int32_t predictor; int index; } knot_adpcm_t;
void knot_adpcm_init(knot_adpcm_t *state);
/* samples must be even; writes samples/2 bytes. */
void knot_adpcm_encode(knot_adpcm_t *state, const int16_t *pcm, size_t samples, uint8_t *out);
void knot_adpcm_decode(knot_adpcm_t *state, const uint8_t *in, size_t bytes, int16_t *pcm);
