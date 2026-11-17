#include "knot_recorder.h"
#include "knot_adpcm.h"
#include "knot_audio.h"
#include "knot_model.h"
#include "bsp_audio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define KNOT_MIC_GAIN 4

static TaskHandle_t task;
static volatile bool active, stop_requested, done;
static unsigned rec_slot;
static uint32_t rec_seq, rec_bytes, rec_crc;
static int64_t started_us;

static void recorder_task(void *arg) {
    (void)arg;
    static int16_t pcm[512];
    static uint8_t page[4096];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        knot_adpcm_t state; knot_adpcm_init(&state);
        uint32_t crc = 0xffffffffU, written = 0;
        size_t fill = 0;
        bool ok = true;
        while (!stop_requested && written + fill < KNOT_AUDIO_MAX_BYTES) {
            if (bsp_audio_read(pcm, sizeof(pcm)) != ESP_OK) { ok = false; break; }
            /* The codec's 30 dB PGA leaves speech near -50 dBFS; add 12 dB digitally, saturating. */
            for (unsigned i = 0; i < 512; ++i) {
                int32_t v = (int32_t)pcm[i] * KNOT_MIC_GAIN;
                pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
            knot_adpcm_encode(&state, pcm, 512, page + fill);
            fill += 256;
            if (fill == sizeof(page) || written + fill >= KNOT_AUDIO_MAX_BYTES) {
                if (knot_audio_write(rec_slot, written, page, fill) != ESP_OK) { ok = false; fill = 0; break; }
                crc = knot_crc_update(crc, page, (unsigned)fill);
                written += fill; fill = 0;
            }
        }
        if (ok && fill) {
            if (knot_audio_write(rec_slot, written, page, fill) == ESP_OK) {
                crc = knot_crc_update(crc, page, (unsigned)fill); written += fill;
            }
        }
        rec_bytes = written; rec_crc = ~crc;
        /* Whatever reached flash intact is kept, even after a read error. */
        if (rec_bytes && knot_audio_seal(rec_slot, rec_seq, rec_bytes, rec_crc) != ESP_OK) rec_bytes = 0;
        active = false; done = true;
    }
}
esp_err_t knot_recorder_init(void) {
    return xTaskCreate(recorder_task, "knot_rec", 4096, NULL, 5, &task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
esp_err_t knot_recorder_start(unsigned slot, uint32_t seq) {
    if (!task || active) return ESP_ERR_INVALID_STATE;
    rec_slot = slot; rec_seq = seq; stop_requested = false; done = false; active = true;
    started_us = esp_timer_get_time();
    xTaskNotifyGive(task);
    return ESP_OK;
}
void knot_recorder_stop(void) { stop_requested = true; }
bool knot_recorder_active(void) { return active; }
uint32_t knot_recorder_elapsed_ms(void) { return active ? (uint32_t)((esp_timer_get_time() - started_us) / 1000) : 0; }
bool knot_recorder_poll(uint32_t *seq, uint32_t *bytes, uint32_t *crc) {
    if (!done) return false;
    done = false; *seq = rec_seq; *bytes = rec_bytes; *crc = rec_crc;
    return true;
}
