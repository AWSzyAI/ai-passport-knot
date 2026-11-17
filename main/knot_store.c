#include "knot_store.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>

#define MAX_EVENTS 512U
static nvs_handle_t db;
static uint32_t boot_id, next_seq;
static unsigned count;
static knot_anchor_t anchor;
static knot_history_t history[KNOT_HISTORY];
static void key(char *buffer, char prefix, uint32_t value) {
    snprintf(buffer, 16, "%c%08lx", prefix, (unsigned long)value);
}
/* History slot = seq % KNOT_HISTORY; failures here never affect the event queue. */
static void put_history(const knot_history_t *entry) {
    char name[16]; key(name, 'h', entry->seq % KNOT_HISTORY);
    history[entry->seq % KNOT_HISTORY] = *entry;
    if (nvs_set_blob(db, name, entry, sizeof(*entry)) == ESP_OK) (void)nvs_commit(db);
}
static void touch_history(uint32_t seq, int synced, int64_t utc_ms) {
    knot_history_t entry = history[seq % KNOT_HISTORY];
    if (entry.seq != seq) return;
    if (synced >= 0) entry.synced = (uint32_t)synced;
    if (utc_ms) entry.utc_ms = utc_ms;
    put_history(&entry);
}
/* One worker owns all calls. Errors are never repaired by erasing user data. */
static esp_err_t read_event(const char *name, knot_event_t *event) {
    size_t size = sizeof(*event);
    esp_err_t err = nvs_get_blob(db, name, event, &size);
    if (err == ESP_OK && (size != sizeof(*event) || !knot_valid(event)))
        return ESP_ERR_INVALID_CRC;
    return err;
}
static esp_err_t put_event(knot_event_t *event) {
    char name[16]; key(name, 'e', event->seq); knot_seal(event);
    esp_err_t err = nvs_set_blob(db, name, event, sizeof(*event));
    return err == ESP_OK ? nvs_commit(db) : err;
}
esp_err_t knot_store_init(uint32_t *boot) {
    esp_err_t err = nvs_flash_init_partition("knots");
    if (err != ESP_OK) return err;
    err = nvs_open_from_partition("knots", "events", NVS_READWRITE, &db);
    if (err != ESP_OK) return err;
    uint32_t previous = 0;
    err = nvs_get_u32(db, "boot", &previous);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    if (previous == UINT32_MAX) return ESP_ERR_INVALID_STATE;
    boot_id = previous + 1;
    next_seq = 1;
    err = nvs_get_u32(db, "next", &next_seq);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    if (!next_seq) return ESP_ERR_INVALID_STATE;
    count = 0;
    nvs_iterator_t it = NULL;
    err = nvs_entry_find("knots", "events", NVS_TYPE_BLOB, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info; nvs_entry_info(it, &info);
        if (info.key[0] == 'e') {
            knot_event_t event;
            esp_err_t read = read_event(info.key, &event);
            if (read != ESP_OK) { nvs_release_iterator(it); return read; }
            if (event.seq >= next_seq || event.boot > previous) {
                nvs_release_iterator(it); return ESP_ERR_INVALID_STATE;
            }
            ++count;
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    if (err != ESP_ERR_NVS_NOT_FOUND) return err;
    memset(history, 0, sizeof(history));
    for (unsigned slot = 0; slot < KNOT_HISTORY; ++slot) {
        char name[16]; key(name, 'h', slot);
        knot_history_t entry; size_t size = sizeof(entry);
        if (nvs_get_blob(db, name, &entry, &size) != ESP_OK || size != sizeof(entry) ||
            !entry.seq || entry.seq % KNOT_HISTORY != slot || entry.seq >= next_seq) continue;
        history[slot] = entry;
        if (!entry.synced) {
            /* ACK committed but history update lost: the event is gone, so it was synced. */
            char event_name[16]; key(event_name, 'e', entry.seq);
            knot_event_t event;
            if (read_event(event_name, &event) == ESP_ERR_NVS_NOT_FOUND) touch_history(entry.seq, 1, 0);
        }
    }
    err = nvs_set_u32(db, "boot", boot_id);
    if (err == ESP_OK) err = nvs_commit(db);
    if (err == ESP_OK) *boot = boot_id;
    return err;
}
esp_err_t knot_store_add(uint64_t mono_us, knot_event_t *event) {
    if (count >= MAX_EVENTS || next_seq == UINT32_MAX) return ESP_ERR_NO_MEM;
    uint32_t seq = next_seq;
    /* Reserve first: a crash can leave a gap but can never reuse an ID. */
    esp_err_t err = nvs_set_u32(db, "next", seq + 1);
    if (err == ESP_OK) err = nvs_commit(db);
    if (err != ESP_OK) return err;
    next_seq = seq + 1;
    memset(event, 0, sizeof(*event));
    event->version = 1; event->seq = seq; event->boot = boot_id;
    event->mono_us = mono_us;
    (void)knot_resolve(event, &anchor);
    err = put_event(event);
    if (err == ESP_OK) {
        ++count;
        knot_history_t entry = { .seq = seq, .synced = 0, .utc_ms = event->utc_ms };
        put_history(&entry);
    }
    return err;
}
esp_err_t knot_store_first(knot_event_t *event) {
    uint32_t minimum = UINT32_MAX;
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find("knots", "events", NVS_TYPE_BLOB, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info; nvs_entry_info(it, &info);
        if (info.key[0] == 'e') {
            knot_event_t candidate;
            esp_err_t read = read_event(info.key, &candidate);
            if (read != ESP_OK) { nvs_release_iterator(it); return read; }
            if (candidate.seq < minimum) { *event = candidate; minimum = candidate.seq; }
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    if (err != ESP_ERR_NVS_NOT_FOUND) return err;
    if (minimum == UINT32_MAX) return ESP_ERR_NOT_FOUND;
    if (!event->utc_ms) {
        /* Frozen time is persisted before first transmission, including after reboot. */
        char name[16]; key(name, 't', event->boot);
        knot_anchor_t saved; size_t size = sizeof(saved);
        err = nvs_get_blob(db, name, &saved, &size);
        if (err == ESP_OK) {
            if (size != sizeof(saved)) return ESP_ERR_INVALID_SIZE;
            if (knot_resolve(event, &saved)) {
                err = put_event(event);
                if (err == ESP_OK) touch_history(event->seq, -1, event->utc_ms);
                return err;
            }
        } else if (err != ESP_ERR_NVS_NOT_FOUND) return err;
    }
    return ESP_OK;
}
esp_err_t knot_store_sync(uint64_t mono_us, int64_t utc_ms) {
    knot_anchor_t next = { .boot = boot_id, .mono_us = mono_us, .utc_ms = utc_ms };
    char name[16]; key(name, 't', boot_id);
    esp_err_t err = nvs_set_blob(db, name, &next, sizeof(next));
    if (err == ESP_OK) err = nvs_commit(db);
    if (err == ESP_OK) anchor = next;
    return err;
}
esp_err_t knot_store_ack(uint32_t seq) {
    char name[16]; key(name, 'e', seq);
    knot_event_t event;
    esp_err_t err = read_event(name, &event);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(db, name);
    if (err == ESP_OK) err = nvs_commit(db);
    if (err == ESP_OK && count) --count;
    if (err == ESP_OK) touch_history(seq, 1, event.utc_ms);
    return err;
}
unsigned knot_store_count(void) { return count; }
unsigned knot_store_history(knot_history_t *out, unsigned max) {
    unsigned n = 0;
    for (uint32_t seq = next_seq; seq-- > 1 && n < max && next_seq - seq <= KNOT_HISTORY;) {
        const knot_history_t *entry = &history[seq % KNOT_HISTORY];
        if (entry->seq == seq) out[n++] = *entry;
    }
    return n;
}
