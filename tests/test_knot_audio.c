#include "knot_adpcm.h"
#include "knot_audio.h"
#include "knot_model.h"
#include "esp_partition.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIZE 0x480000U
static uint8_t flash[SIZE];
static esp_partition_t part = { 0x310000, SIZE };
static int erases;
const esp_partition_t *esp_partition_find_first(esp_partition_type_t t, esp_partition_subtype_t s, const char *l) {
    assert(t == ESP_PARTITION_TYPE_DATA && s == 0x40 && !strcmp(l, "audio")); return &part;
}
esp_err_t esp_partition_read(const esp_partition_t *p, size_t o, void *d, size_t n) {
    (void)p; assert(o + n <= SIZE); memcpy(d, flash + o, n); return ESP_OK;
}
esp_err_t esp_partition_write(const esp_partition_t *p, size_t o, const void *d, size_t n) {
    (void)p; assert(o + n <= SIZE);
    const uint8_t *b = d;
    for (size_t i = 0; i < n; ++i) flash[o + i] &= b[i];  /* NOR flash: only 1 -> 0 */
    return ESP_OK;
}
esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t o, size_t n) {
    (void)p; assert(o % 4096 == 0 && n % 4096 == 0 && o + n <= SIZE);
    memset(flash + o, 0xff, n); ++erases; return ESP_OK;
}

static void adpcm_quality(void) {
    static int16_t pcm[16000], out[16000]; static uint8_t enc[8000];
    for (int i = 0; i < 16000; ++i) pcm[i] = (int16_t)(8000 * sin(2 * M_PI * 440 * i / 16000.0) + 3000 * sin(2 * M_PI * 1700 * i / 16000.0));
    knot_adpcm_t s; knot_adpcm_init(&s); knot_adpcm_encode(&s, pcm, 16000, enc);
    knot_adpcm_init(&s); knot_adpcm_decode(&s, enc, 8000, out);
    double sig = 0, err = 0;
    for (int i = 200; i < 16000; ++i) { sig += (double)pcm[i] * pcm[i]; err += (double)(pcm[i] - out[i]) * (pcm[i] - out[i]); }
    double snr = 10 * log10(sig / err);
    assert(snr > 20);
    /* Golden prefix shared with the Python decoder test. */
    FILE *f = fopen(getenv("KNOT_ADPCM_VECTOR") ? getenv("KNOT_ADPCM_VECTOR") : "/dev/null", "wb");
    if (f) { fwrite(enc, 1, 8000, f); fclose(f); }
    printf("adpcm SNR %.1f dB\n", snr);
}

int main(void) {
    adpcm_quality();
    memset(flash, 0x5a, SIZE);  /* stale old-app bytes */
    assert(knot_audio_init() == ESP_OK && knot_audio_slot_count() == 9);
    knot_audio_mark_used(2);
    knot_audio_finish_init();
    assert(knot_audio_ready_count() == 0);                  /* garbage slots are DIRTY */
    while (knot_audio_maintain()) {}
    assert(knot_audio_ready_count() == 8 && erases == 8 * 8);   /* slot 2 still owned; 64 KiB steps */
    int slot = knot_audio_claim(); assert(slot == 0);
    uint8_t data[1000]; for (int i = 0; i < 1000; ++i) data[i] = (uint8_t)i;
    assert(knot_audio_write(slot, 0, data, 1000) == ESP_OK);
    uint32_t crc = knot_crc(data, 1000), bytes, got;
    assert(knot_audio_header(slot, 77, &bytes, &got) == ESP_ERR_NOT_FOUND);
    assert(knot_audio_seal(slot, 77, 1000, crc) == ESP_OK);
    assert(knot_audio_header(slot, 77, &bytes, &got) == ESP_OK && bytes == 1000 && got == crc);
    assert(knot_audio_header(slot, 78, &bytes, &got) == ESP_ERR_NOT_FOUND);
    uint8_t back[1000]; assert(knot_audio_read(slot, 0, back, 1000) == ESP_OK && !memcmp(back, data, 1000));
    assert(knot_audio_write(slot, KNOT_AUDIO_SLOT_SIZE, data, 1) != ESP_OK);
    /* Reboot: an unowned slot with data (interrupted recording) is not reused until erased. */
    assert(knot_audio_init() == ESP_OK); knot_audio_finish_init();
    assert(knot_audio_ready_count() == 7);
    knot_audio_mark_used(0); knot_audio_release(0);          /* acked -> dirty */
    while (knot_audio_maintain()) {}
    assert(knot_audio_init() == ESP_OK); knot_audio_mark_used(2); knot_audio_finish_init();
    assert(knot_audio_ready_count() == 8);
    for (int i = 0; i < 8; ++i) assert(knot_audio_claim() >= 0);
    assert(knot_audio_claim() == -1);
    assert(KNOT_AUDIO_MAX_BYTES + KNOT_AUDIO_DATA_OFFSET <= KNOT_AUDIO_SLOT_SIZE);
    puts("knot_audio: PASS (ADPCM quality, slot classify/erase/claim, header seal, reboot)");
}
