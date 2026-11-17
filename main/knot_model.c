#include "knot_model.h"
#include <stddef.h>

uint32_t knot_crc(const void *data, unsigned length) {
    const unsigned char *p = data;
    uint32_t crc = 0xffffffffU;
    for (unsigned i = 0; i < length; ++i) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}
void knot_seal(knot_event_t *event) {
    event->crc = knot_crc(event, offsetof(knot_event_t, crc));
}
bool knot_valid(const knot_event_t *event) {
    return event->version == 1 && event->seq != 0 && event->boot != 0 &&
        event->mono_us <= INT64_MAX && event->utc_ms >= 0 &&
        event->crc == knot_crc(event, offsetof(knot_event_t, crc));
}
bool knot_resolve(knot_event_t *event, const knot_anchor_t *anchor) {
    if (event->utc_ms != 0) return true;
    if (event->boot != anchor->boot || anchor->utc_ms < 1704067200000LL ||
        anchor->utc_ms > 4102444800000LL || event->mono_us > INT64_MAX ||
        anchor->mono_us > INT64_MAX) return false;
    int64_t delta = ((int64_t)event->mono_us - (int64_t)anchor->mono_us) / 1000;
    /* Bounds also avoid overflow and reject implausible stale anchors. */
    if (delta < -31536000000LL || delta > 31536000000LL) return false;
    event->utc_ms = anchor->utc_ms + delta;
    if (event->utc_ms < 1704067200000LL || event->utc_ms > 4102444800000LL) {
        event->utc_ms = 0;
        return false;
    }
    knot_seal(event);
    return true;
}
