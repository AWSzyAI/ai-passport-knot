#pragma once
#include "knot_model.h"
#include "esp_err.h"
esp_err_t knot_store_init(uint32_t *boot);
esp_err_t knot_store_add(uint64_t mono_us, knot_event_t *event); /* time-only */
esp_err_t knot_store_add_kind(uint64_t mono_us, uint8_t kind, uint8_t audio_slot, knot_event_t *event);
/* Mark a voice event's recording finished; bytes 0 means interrupted (no audio). */
esp_err_t knot_store_finish_audio(uint32_t seq, uint32_t bytes, uint32_t crc);
/* Visit every stored event (used at boot to rebuild audio slot ownership). */
esp_err_t knot_store_scan(void (*visit)(const knot_event_t *event, void *context), void *context);
esp_err_t knot_store_first(knot_event_t *event);
esp_err_t knot_store_ack(uint32_t seq);
esp_err_t knot_store_sync(uint64_t mono_us, int64_t utc_ms);
unsigned knot_store_count(void);

/* Display-only history of recent presses. The event queue stays the source of truth. */
#define KNOT_HISTORY 32U
/* flags: bit0 synced, bits 8..15 kind. */
typedef struct { uint32_t seq, flags; int64_t utc_ms; } knot_history_t;
#define KNOT_HISTORY_SYNCED(h) (((h)->flags & 1U) != 0)
#define KNOT_HISTORY_KIND(h) ((uint8_t)(((h)->flags >> 8) & 0xffU))
/* Newest first; returns the number written. */
unsigned knot_store_history(knot_history_t *out, unsigned max);
