#include "knot_audio.h"
#include "knot_model.h"
#include "esp_partition.h"
#include <string.h>

#define FREE_MAGIC 0x45455246U  /* "FREE" */
#define HEAD_MAGIC 0x31414e4bU  /* "KNA1" */
#define HEAD_OFFSET 64U
enum { SLOT_UNKNOWN, SLOT_READY, SLOT_USED, SLOT_DIRTY };
typedef struct { uint32_t magic, seq, bytes, crc, check; } header_t;

static const esp_partition_t *part;
static uint8_t state[KNOT_AUDIO_SLOTS_MAX];
static unsigned slots;
static uint32_t erased[KNOT_AUDIO_SLOTS_MAX]; /* bytes erased so far in a DIRTY slot */
#define ERASE_STEP 0x10000U

esp_err_t knot_audio_init(void) {
    part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "audio");
    if (!part) return ESP_ERR_NOT_FOUND;
    slots = part->size / KNOT_AUDIO_SLOT_SIZE;
    if (slots > KNOT_AUDIO_SLOTS_MAX) slots = KNOT_AUDIO_SLOTS_MAX;
    memset(state, SLOT_UNKNOWN, sizeof(state));
    return slots ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
unsigned knot_audio_slot_count(void) { return slots; }
void knot_audio_mark_used(unsigned slot) { if (slot < slots) state[slot] = SLOT_USED; }
static uint32_t base(unsigned slot) { return slot * KNOT_AUDIO_SLOT_SIZE; }
static bool is_ready(unsigned slot) {
    uint32_t marker; uint8_t head[sizeof(header_t)];
    if (esp_partition_read(part, base(slot), &marker, sizeof(marker)) != ESP_OK || marker != FREE_MAGIC) return false;
    if (esp_partition_read(part, base(slot) + HEAD_OFFSET, head, sizeof(head)) != ESP_OK) return false;
    for (unsigned i = 0; i < sizeof(head); ++i) if (head[i] != 0xff) return false;
    /* First data sector must be blank too, or an interrupted recording left bytes behind. */
    uint8_t probe[64];
    if (esp_partition_read(part, base(slot) + KNOT_AUDIO_DATA_OFFSET, probe, sizeof(probe)) != ESP_OK) return false;
    for (unsigned i = 0; i < sizeof(probe); ++i) if (probe[i] != 0xff) return false;
    return true;
}
void knot_audio_finish_init(void) {
    for (unsigned i = 0; i < slots; ++i)
        if (state[i] == SLOT_UNKNOWN) { state[i] = is_ready(i) ? SLOT_READY : SLOT_DIRTY; erased[i] = 0; }
}
int knot_audio_claim(void) {
    for (unsigned i = 0; i < slots; ++i) if (state[i] == SLOT_READY) { state[i] = SLOT_USED; return (int)i; }
    return -1;
}
void knot_audio_release(unsigned slot) { if (slot < slots && state[slot] == SLOT_USED) { state[slot] = SLOT_DIRTY; erased[slot] = 0; } }
unsigned knot_audio_ready_count(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < slots; ++i) n += state[i] == SLOT_READY;
    return n;
}
bool knot_audio_maintain(void) {
    /* One 64 KiB block per call so the main loop never stalls for a whole slot erase. */
    for (unsigned i = 0; i < slots; ++i) {
        if (state[i] != SLOT_DIRTY) continue;
        if (erased[i] < KNOT_AUDIO_SLOT_SIZE) {
            if (esp_partition_erase_range(part, base(i) + erased[i], ERASE_STEP) == ESP_OK) erased[i] += ERASE_STEP;
            return true;
        }
        uint32_t marker = FREE_MAGIC;
        if (esp_partition_write(part, base(i), &marker, sizeof(marker)) == ESP_OK) state[i] = SLOT_READY;
        else erased[i] = 0;
        return true;
    }
    return false;
}
esp_err_t knot_audio_write(unsigned slot, uint32_t offset, const void *data, size_t bytes) {
    if (slot >= slots || offset + bytes > KNOT_AUDIO_SLOT_SIZE - KNOT_AUDIO_DATA_OFFSET) return ESP_ERR_INVALID_SIZE;
    return esp_partition_write(part, base(slot) + KNOT_AUDIO_DATA_OFFSET + offset, data, bytes);
}
esp_err_t knot_audio_read(unsigned slot, uint32_t offset, void *data, size_t bytes) {
    if (slot >= slots || offset + bytes > KNOT_AUDIO_SLOT_SIZE - KNOT_AUDIO_DATA_OFFSET) return ESP_ERR_INVALID_SIZE;
    return esp_partition_read(part, base(slot) + KNOT_AUDIO_DATA_OFFSET + offset, data, bytes);
}
esp_err_t knot_audio_seal(unsigned slot, uint32_t seq, uint32_t bytes, uint32_t crc) {
    if (slot >= slots) return ESP_ERR_INVALID_SIZE;
    header_t h = { HEAD_MAGIC, seq, bytes, crc, 0 };
    h.check = knot_crc(&h, offsetof(header_t, check));
    return esp_partition_write(part, base(slot) + HEAD_OFFSET, &h, sizeof(h));
}
esp_err_t knot_audio_header(unsigned slot, uint32_t seq, uint32_t *bytes, uint32_t *crc) {
    if (slot >= slots) return ESP_ERR_INVALID_SIZE;
    header_t h;
    esp_err_t err = esp_partition_read(part, base(slot) + HEAD_OFFSET, &h, sizeof(h));
    if (err != ESP_OK) return err;
    if (h.magic != HEAD_MAGIC || h.seq != seq || h.bytes > KNOT_AUDIO_MAX_BYTES ||
        h.check != knot_crc(&h, offsetof(header_t, check))) return ESP_ERR_NOT_FOUND;
    *bytes = h.bytes; *crc = h.crc;
    return ESP_OK;
}
