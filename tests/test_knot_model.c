#include "knot_model.h"
#include <assert.h>
#include <stdio.h>
#include <stddef.h>

int main(void) {
    knot_event_t e = { .version=2, .audio_slot=KNOT_NO_SLOT, .seq=1, .boot=4, .mono_us=1000000 };
    knot_seal(&e); assert(knot_valid(&e));
    knot_anchor_t a = { .boot=4, .mono_us=11000000, .utc_ms=1794844800000LL };
    assert(knot_resolve(&e, &a)); assert(e.utc_ms == a.utc_ms-10000); assert(knot_valid(&e));
    int64_t frozen = e.utc_ms; a.utc_ms += 100000;
    assert(knot_resolve(&e, &a)); assert(e.utc_ms == frozen);
    e.seq++; assert(!knot_valid(&e)); knot_seal(&e);
    e.utc_ms=0; e.boot=3; assert(!knot_resolve(&e, &a)); assert(!e.utc_ms);
    e.boot=4; e.mono_us=12000000; assert(knot_resolve(&e,&a)); assert(e.utc_ms==a.utc_ms+1000);
    e.utc_ms=0; a.utc_ms=0; assert(!knot_resolve(&e,&a));
    e.version=9; knot_seal(&e); assert(!knot_valid(&e));
    knot_event_v1_t old = { .version=1, .seq=7, .boot=3, .mono_us=5, .utc_ms=1794844800000LL };
    old.crc = knot_crc(&old, offsetof(knot_event_v1_t, crc));
    knot_event_t up; assert(knot_upgrade_v1(&old,&up) && up.seq==7 && up.kind==KNOT_KIND_TIME && up.utc_ms==old.utc_ms);
    old.seq=8; assert(!knot_upgrade_v1(&old,&up));
    e.version=2; e.kind=9; knot_seal(&e); assert(!knot_valid(&e));
    assert(knot_crc("123456789",9)==0xcbf43926U); /* standard CRC-32, same as Python zlib.crc32 */
    puts("knot_model: PASS (v1 upgrade, same-boot reconstruction, cross-boot refusal, immutable timestamp, CRC)");
}
