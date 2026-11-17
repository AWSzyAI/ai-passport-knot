#include "knot_adpcm.h"

static const int16_t steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
    449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int8_t index_adjust[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

void knot_adpcm_init(knot_adpcm_t *state) { state->predictor = 0; state->index = 0; }

static void apply(knot_adpcm_t *s, uint8_t code) {
    int step = steps[s->index];
    int diff = step >> 3;
    if (code & 4) diff += step;
    if (code & 2) diff += step >> 1;
    if (code & 1) diff += step >> 2;
    s->predictor += (code & 8) ? -diff : diff;
    if (s->predictor > 32767) s->predictor = 32767;
    if (s->predictor < -32768) s->predictor = -32768;
    s->index += index_adjust[code & 7];
    if (s->index < 0) s->index = 0;
    if (s->index > 88) s->index = 88;
}
static uint8_t encode_one(knot_adpcm_t *s, int16_t sample) {
    int step = steps[s->index];
    int diff = sample - s->predictor;
    uint8_t code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    if (diff >= step) { code |= 4; diff -= step; }
    step >>= 1;
    if (diff >= step) { code |= 2; diff -= step; }
    step >>= 1;
    if (diff >= step) code |= 1;
    apply(s, code);
    return code;
}
void knot_adpcm_encode(knot_adpcm_t *state, const int16_t *pcm, size_t samples, uint8_t *out) {
    for (size_t i = 0; i + 1 < samples; i += 2)
        out[i / 2] = (uint8_t)(encode_one(state, pcm[i]) | (encode_one(state, pcm[i + 1]) << 4));
}
void knot_adpcm_decode(knot_adpcm_t *state, const uint8_t *in, size_t bytes, int16_t *pcm) {
    for (size_t i = 0; i < bytes; ++i) {
        apply(state, in[i] & 0x0f); pcm[2 * i] = (int16_t)state->predictor;
        apply(state, in[i] >> 4); pcm[2 * i + 1] = (int16_t)state->predictor;
    }
}
