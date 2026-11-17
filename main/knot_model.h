#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t version, seq, boot;
    uint64_t mono_us;
    int64_t utc_ms; /* zero means unknown, never substitute upload time */
    uint32_t crc;
} knot_event_t;

typedef struct {
    uint32_t boot;
    uint64_t mono_us;
    int64_t utc_ms;
} knot_anchor_t;

uint32_t knot_crc(const void *data, unsigned length);
void knot_seal(knot_event_t *event);
bool knot_valid(const knot_event_t *event);
bool knot_resolve(knot_event_t *event, const knot_anchor_t *anchor);
