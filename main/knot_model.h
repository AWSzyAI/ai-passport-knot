#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { KNOT_KIND_TIME = 0, KNOT_KIND_START = 1, KNOT_KIND_END = 2, KNOT_KIND_VOICE = 3 };
#define KNOT_FLAG_AUDIO_DONE 1U   /* recording finished (audio_bytes may be 0 = interrupted) */
#define KNOT_NO_SLOT 0xffU

/* Version 2 event. Version 1 blobs (no kind/audio) are upgraded on read. */
typedef struct {
    uint32_t version, seq, boot;
    uint8_t kind, audio_slot, flags, reserved;
    uint64_t mono_us;
    int64_t utc_ms; /* zero means unknown, never substitute upload time */
    uint32_t audio_bytes, audio_crc;
    uint32_t crc;
} knot_event_t;

/* On-flash layout of version 1, kept only for upgrading stored records. */
typedef struct {
    uint32_t version, seq, boot;
    uint64_t mono_us;
    int64_t utc_ms;
    uint32_t crc;
} knot_event_v1_t;

typedef struct {
    uint32_t boot;
    uint64_t mono_us;
    int64_t utc_ms;
} knot_anchor_t;

uint32_t knot_crc(const void *data, unsigned length);
uint32_t knot_crc_update(uint32_t state, const void *data, unsigned length); /* start 0xffffffff, finish with ~ */
void knot_seal(knot_event_t *event);
bool knot_valid(const knot_event_t *event);
bool knot_upgrade_v1(const knot_event_v1_t *old, knot_event_t *event);
bool knot_resolve(knot_event_t *event, const knot_anchor_t *anchor);
