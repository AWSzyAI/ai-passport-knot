#pragma once
#include "knot_model.h"
#include "esp_err.h"
esp_err_t knot_store_init(uint32_t *boot);
esp_err_t knot_store_add(uint64_t mono_us, knot_event_t *event);
esp_err_t knot_store_first(knot_event_t *event);
esp_err_t knot_store_ack(uint32_t seq);
esp_err_t knot_store_sync(uint64_t mono_us, int64_t utc_ms);
unsigned knot_store_count(void);

/* Display-only history of recent presses. The event queue stays the source of truth. */
#define KNOT_HISTORY 32U
typedef struct { uint32_t seq, synced; int64_t utc_ms; } knot_history_t;
/* Newest first; returns the number written. */
unsigned knot_store_history(knot_history_t *out, unsigned max);
