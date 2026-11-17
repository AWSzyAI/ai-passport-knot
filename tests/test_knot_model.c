#include "knot_model.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    knot_event_t e = { .version=1, .seq=1, .boot=4, .mono_us=1000000 };
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
    puts("knot_model: PASS (same-boot reconstruction, cross-boot refusal, immutable timestamp, CRC)");
}
