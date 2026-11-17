#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Audio partition "audio" (data, subtype 0x40) split into fixed slots.
 * Slot layout: sector 0 = [FREE marker @0][recording header @64]; data from 4096.
 * READY slot: fully erased, then FREE marker written. A recording writes data, then the header.
 * After the owning event is acknowledged the slot becomes DIRTY and is erased later. */
#define KNOT_AUDIO_RATE 16000U
#define KNOT_AUDIO_MAX_MS 60000U
#define KNOT_AUDIO_MAX_BYTES (KNOT_AUDIO_RATE / 2U * (KNOT_AUDIO_MAX_MS / 1000U)) /* 4-bit ADPCM */
#define KNOT_AUDIO_SLOT_SIZE 0x80000U
#define KNOT_AUDIO_DATA_OFFSET 4096U
#define KNOT_AUDIO_SLOTS_MAX 16U

esp_err_t knot_audio_init(void);          /* finds partition; all slots start unknown */
unsigned knot_audio_slot_count(void);
void knot_audio_mark_used(unsigned slot); /* call for every stored voice event before finish_init */
void knot_audio_finish_init(void);        /* classify the remaining slots as READY or DIRTY */
int knot_audio_claim(void);               /* READY slot -> USED, or -1 */
void knot_audio_release(unsigned slot);   /* USED -> DIRTY */
unsigned knot_audio_ready_count(void);
bool knot_audio_maintain(void);           /* erase one DIRTY slot; returns true if it did work */
esp_err_t knot_audio_write(unsigned slot, uint32_t offset, const void *data, size_t bytes);
esp_err_t knot_audio_seal(unsigned slot, uint32_t seq, uint32_t bytes, uint32_t crc);
/* Valid header for this seq -> ESP_OK with length and CRC. */
esp_err_t knot_audio_header(unsigned slot, uint32_t seq, uint32_t *bytes, uint32_t *crc);
esp_err_t knot_audio_read(unsigned slot, uint32_t offset, void *data, size_t bytes);
