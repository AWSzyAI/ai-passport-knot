#include "knot_store.h"
#include "knot_audio.h"
#include "knot_recorder.h"
#include "buddy_ble.h"
#include "bsp_button.h"
#include "bsp_pins.h"
#include "bsp_display.h"
#include "bsp_battery.h"
#include "bsp_audio.h"
#include "bsp_i2c.h"
#include "cJSON.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <math.h>

/* Keys (decided on release, timestamp taken at press-down):
 *   record mode: OK = time, hold OK = time + voice memo (OK again stops, 60 s cap),
 *                UP = time #START, DOWN = time #END, hold UP/DOWN = browse history.
 *   browse mode: UP/DOWN page, hold UP/DOWN = back to record mode; OK still records.
 *   30 s idle -> screen off, back to record mode. */
enum { MSG_KEY = 1, MSG_RX = 3, MSG_PIN = 4 };
typedef struct { int kind, button, event; uint64_t mono_us; uint32_t generation, passkey; char line[256]; } message_t;
#define HISTORY_ROWS 8
#define COLOR_SYNCED 0x4ade80
#define COLOR_WAITING 0xfacc15
#define COLOR_DATE 0x8fa3b8
#define VOICE_RESEND_US 180000000LL
#define EVENT_RESEND_US 2000000LL

static QueueHandle_t queue;
static volatile bool overflow;
static bool storage_ok, audio_ok, audio_store_ok, ui_ok, synced;
static uint32_t boot, session, pending;
static knot_event_t pending_event;
static char device[13];
static char notice[96] = "Starting...";
static lv_obj_t *status_label, *count_label, *battery_label, *history_label;
static lv_obj_t *row_a[HISTORY_ROWS], *row_b[HISTORY_ROWS];
static int64_t last_activity, last_send;
static bool screen_on = true, ui_dirty, browse;
static unsigned page;
static uint64_t press_mono[BSP_BTN_COUNT];
static bool long_fired[BSP_BTN_COUNT], consumed[BSP_BTN_COUNT];
static struct { bool active; uint32_t seq, off, end, chunk; } stream;

static void on_key(bsp_btn_t button, bsp_btn_ev_t event, void *context) {
    (void)context;
    if (event != BSP_BTN_PRESS && event != BSP_BTN_LONG && event != BSP_BTN_RELEASE) return;
    message_t msg = { .kind = MSG_KEY, .button = (int)button, .event = (int)event,
                      .mono_us = (uint64_t)esp_timer_get_time() };
    if (xQueueSend(queue, &msg, 0) != pdTRUE) overflow = true;
}
static void on_ble(const buddy_ble_event_t *event, void *context) {
    (void)context;
    message_t msg = {0};
    if (event->type == BUDDY_BLE_EVENT_RX_LINE) {
        if (event->data.rx_line.length >= sizeof(msg.line)) return;
        msg.kind = MSG_RX; msg.generation = event->data.rx_line.connection_generation;
        memcpy(msg.line, event->data.rx_line.data, event->data.rx_line.length);
    } else if (event->type == BUDDY_BLE_EVENT_PASSKEY) {
        msg.kind = MSG_PIN; msg.passkey = event->data.passkey.value;
    } else return;
    if (xQueueSend(queue, &msg, 0) != pdTRUE) overflow = true;
}
static esp_err_t send_line(const char *line) {
    return buddy_ble_send_for_generation(line, strlen(line), session);
}
static bool number(const cJSON *json, const char *key, int64_t min, int64_t max, int64_t *out) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(json, key);
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) ||
        value->valuedouble < (double)min || value->valuedouble > (double)max ||
        floor(value->valuedouble) != value->valuedouble) return false;
    *out = (int64_t)value->valuedouble;
    return true;
}
static void storage_failed(const char *text) {
    storage_ok = false; snprintf(notice, sizeof(notice), "%s", text); ui_dirty = true;
}
static void handle_line(const message_t *msg) {
    if (!buddy_ble_is_generation_secure(msg->generation)) return;
    cJSON *json = cJSON_Parse(msg->line);
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(json, "type");
    int64_t version;
    if (!json || !cJSON_IsString(type) || !number(json, "v", 1, 1, &version)) goto done;
    char reply[240];
    if (!strcmp(type->valuestring, "hello")) {
        session = msg->generation; synced = false; pending = 0; stream.active = false;
        snprintf(reply, sizeof(reply), "{\"v\":1,\"type\":\"hello\",\"device\":\"%s\",\"boot\":%" PRIu32 ",\"pending\":%u,\"audio\":1}\n",
                 device, boot, knot_store_count());
        send_line(reply);
    } else if (session == msg->generation && !strcmp(type->valuestring, "ping")) {
        int64_t token;
        if (!number(json, "token", 0, 1000000, &token)) goto done;
        snprintf(reply, sizeof(reply), "{\"v\":1,\"type\":\"pong\",\"token\":%" PRId64 ",\"mono_us\":%" PRId64 "}\n",
                 token, esp_timer_get_time());
        send_line(reply);
    } else if (session == msg->generation && !strcmp(type->valuestring, "sync")) {
        int64_t mono, utc;
        if (!storage_ok || !number(json, "mono_us", 0, esp_timer_get_time(), &mono) ||
            !number(json, "utc_ms", 1704067200000LL, 4102444800000LL, &utc)) goto done;
        if (esp_timer_get_time() - mono > 10000000) goto done;
        if (knot_store_sync((uint64_t)mono, utc) != ESP_OK) { storage_failed("Storage error. Restart required."); goto done; }
        synced = true; last_send = 0;
        send_line("{\"v\":1,\"type\":\"synced\"}\n");
        snprintf(notice, sizeof(notice), "Clock synced\nReady to record");
    } else if (storage_ok && session == msg->generation && synced && !strcmp(type->valuestring, "aget")) {
        int64_t seq, off, len, chunk;
        if (!number(json, "seq", 1, UINT32_MAX - 1, &seq) || seq != pending ||
            pending_event.kind != KNOT_KIND_VOICE || !number(json, "off", 0, KNOT_AUDIO_MAX_BYTES, &off) ||
            !number(json, "len", 1, KNOT_AUDIO_MAX_BYTES, &len) || off + len > pending_event.audio_bytes ||
            !number(json, "n", 48, 180, &chunk)) goto done;
        stream.active = true; stream.seq = (uint32_t)seq; stream.off = (uint32_t)off;
        stream.end = (uint32_t)(off + len); stream.chunk = (uint32_t)chunk;
        last_send = esp_timer_get_time();  /* receiver is busy with this event: hold the resend */
    } else if (storage_ok && session == msg->generation && synced && !strcmp(type->valuestring, "ack")) {
        int64_t seq;
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(json, "device");
        if (!cJSON_IsString(id) || strcmp(id->valuestring, device) ||
            !number(json, "seq", 1, UINT32_MAX - 1, &seq) || seq != pending) goto done;
        if (knot_store_ack(pending) != ESP_OK) { storage_failed("Storage error. Restart required."); goto done; }
        if (pending_event.kind == KNOT_KIND_VOICE && pending_event.audio_slot != KNOT_NO_SLOT)
            knot_audio_release(pending_event.audio_slot);
        pending = 0; last_send = 0; stream.active = false; ui_dirty = true;
    }
done:
    cJSON_Delete(json);
}
static const char *kind_name(uint8_t kind) {
    return kind == KNOT_KIND_START ? "start" : kind == KNOT_KIND_END ? "end" : kind == KNOT_KIND_VOICE ? "voice" : "time";
}
static void pump_audio(void) {
    char line[320];
    unsigned char raw[180], b64[248];
    for (int i = 0; i < 6 && stream.active; ++i) {
        if (stream.off >= stream.end) {
            snprintf(line, sizeof(line), "{\"v\":1,\"type\":\"aend\",\"seq\":%" PRIu32 ",\"off\":%" PRIu32 "}\n", stream.seq, stream.end);
            if (send_line(line) == ESP_OK) stream.active = false;
            return;
        }
        uint32_t n = stream.end - stream.off < stream.chunk ? stream.end - stream.off : stream.chunk;
        size_t written = 0;
        if (knot_audio_read(pending_event.audio_slot, stream.off, raw, n) != ESP_OK ||
            mbedtls_base64_encode(b64, sizeof(b64), &written, raw, n) != 0) { stream.active = false; return; }
        b64[written] = 0;
        snprintf(line, sizeof(line), "{\"v\":1,\"type\":\"a\",\"seq\":%" PRIu32 ",\"off\":%" PRIu32 ",\"d\":\"%s\"}\n",
                 stream.seq, stream.off, (const char *)b64);
        if (send_line(line) != ESP_OK) return;  /* buffers full: same chunk next loop */
        stream.off += n;
        last_send = esp_timer_get_time();
    }
}
static void transmit(void) {
    if (!storage_ok || !synced || !buddy_ble_is_generation_secure(session)) return;
    if (stream.active) { pump_audio(); return; }
    int64_t wait = pending && pending_event.kind == KNOT_KIND_VOICE ? VOICE_RESEND_US : EVENT_RESEND_US;
    if (esp_timer_get_time() - last_send < wait) return;
    knot_event_t event;
    esp_err_t err = knot_store_first(&event);
    if (err == ESP_ERR_NOT_FOUND) return;
    if (err != ESP_OK) { storage_failed("Record read error"); return; }
    if (event.kind == KNOT_KIND_VOICE && !(event.flags & KNOT_FLAG_AUDIO_DONE)) return;  /* still recording */
    last_send = esp_timer_get_time();
    char timestamp[32], audio[64] = "", line[360];
    if (event.utc_ms) snprintf(timestamp, sizeof(timestamp), "%" PRId64, event.utc_ms);
    else snprintf(timestamp, sizeof(timestamp), "null");
    if (event.kind == KNOT_KIND_VOICE)
        snprintf(audio, sizeof(audio), ",\"audio_bytes\":%" PRIu32 ",\"audio_crc\":%" PRIu32, event.audio_bytes, event.audio_crc);
    snprintf(line, sizeof(line),
             "{\"v\":1,\"type\":\"event\",\"device\":\"%s\",\"seq\":%" PRIu32 ",\"boot\":%" PRIu32
             ",\"mono_us\":%" PRIu64 ",\"utc_ms\":%s,\"kind\":\"%s\"%s}\n",
             device, event.seq, event.boot, event.mono_us, timestamp, kind_name(event.kind), audio);
    pending = event.seq; pending_event = event;
    send_line(line);
}

/* ---------- screen ---------- */
static void create_ui(void) {
    if (!bsp_lvgl_lock(1000)) return;
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101b28), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xf3e5c8), 0);
    lv_obj_set_style_text_font(screen, &lv_font_montserrat_14, 0);
    lv_obj_t *title = lv_label_create(screen);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_label_set_text(title, "KNOT"); lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 8);
    battery_label = lv_label_create(screen); lv_obj_align(battery_label, LV_ALIGN_TOP_RIGHT, -12, 12);
    count_label = lv_label_create(screen); lv_obj_set_width(count_label, 216);
    lv_obj_align(count_label, LV_ALIGN_TOP_LEFT, 12, 40);
    status_label = lv_label_create(screen); lv_obj_set_width(status_label, 216);
    lv_obj_align(status_label, LV_ALIGN_TOP_LEFT, 12, 64);
    history_label = lv_label_create(screen); lv_obj_set_width(history_label, 216);
    lv_obj_set_style_text_color(history_label, lv_color_hex(COLOR_DATE), 0);
    lv_obj_align(history_label, LV_ALIGN_TOP_LEFT, 12, 120);
    for (unsigned i = 0; i < HISTORY_ROWS; ++i) {
        row_a[i] = lv_label_create(screen); lv_label_set_text(row_a[i], "");
        row_b[i] = lv_label_create(screen); lv_label_set_text(row_b[i], "");
        lv_obj_set_pos(row_a[i], 12, 142 + 21 * (int)i);
        lv_obj_set_pos(row_b[i], 112, 142 + 21 * (int)i);
    }
    lv_screen_load(screen); ui_ok = true;
    bsp_lvgl_unlock();
}
static struct tm local_time(int64_t utc_ms) {
    time_t seconds = (time_t)(utc_ms / 1000 + 8 * 3600);
    struct tm local; gmtime_r(&seconds, &local);
    return local;
}
static int day_key(const knot_history_t *h) {
    if (!h->utc_ms) return -1;
    struct tm t = local_time(h->utc_ms);
    return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
}
static const char *tag_text(uint8_t kind) {
    return kind == KNOT_KIND_START ? "START" : kind == KNOT_KIND_END ? "END" : kind == KNOT_KIND_VOICE ? "MIC" : "";
}
typedef struct { bool header; unsigned index; } line_t;
/* Date headers + indented entries; a page that starts mid-day repeats the date. Returns page count. */
static unsigned layout(const knot_history_t *h, unsigned total, unsigned want, line_t *out, unsigned *count) {
    unsigned current = 0, used = 0;
    int previous = -2;
    *count = 0;
    for (unsigned i = 0; i < total; ++i) {
        int day = day_key(&h[i]);
        bool header = day != previous || used == 0;
        unsigned need = header ? 2 : 1;
        if (used + need > HISTORY_ROWS) { ++current; used = 0; header = true; need = 2; }
        if (current == want) {
            if (header) out[(*count)++] = (line_t){ true, i };
            out[(*count)++] = (line_t){ false, i };
        }
        used += need; previous = day;
    }
    return total ? current + 1 : 1;
}
static void update_ui(void) {
    if (!ui_ok) return;
    int battery = bsp_battery_soc();
    knot_history_t history[KNOT_HISTORY];
    unsigned total = knot_store_history(history, KNOT_HISTORY), lines = 0;
    line_t view[HISTORY_ROWS];
    unsigned pages = layout(history, total, page, view, &lines);
    if (page >= pages) { page = pages - 1; layout(history, total, page, view, &lines); }
    if (!bsp_lvgl_lock(100)) return;
    lv_label_set_text_fmt(count_label, "BT %s  |  waiting %u  |  mic %u",
                          buddy_ble_is_encrypted() ? "on" : "off", knot_store_count(),
                          audio_store_ok ? knot_audio_ready_count() : 0U);
    if (overflow) lv_label_set_text(status_label, "INPUT OVERFLOW\nSome presses NOT saved");
    else if (knot_recorder_active()) {
        uint32_t s = knot_recorder_elapsed_ms() / 1000;
        lv_label_set_text_fmt(status_label, "REC  %" PRIu32 ":%02" PRIu32 " / 1:00\nOK = stop", s / 60, s % 60);
    } else lv_label_set_text(status_label, notice);
    if (battery >= 0) lv_label_set_text_fmt(battery_label, "%d%%", battery);
    else lv_label_set_text(battery_label, "--%");
    if (!total) lv_label_set_text(history_label, "History: none yet");
    else if (browse) lv_label_set_text_fmt(history_label, "History %u/%u  UP/DOWN page, hold=exit", page + 1, pages);
    else lv_label_set_text(history_label, "History   hold UP/DOWN to browse");
    for (unsigned r = 0; r < HISTORY_ROWS; ++r) {
        if (r >= lines) { lv_label_set_text(row_a[r], ""); lv_label_set_text(row_b[r], ""); continue; }
        const knot_history_t *h = &history[view[r].index];
        if (view[r].header) {
            if (h->utc_ms) {
                struct tm t = local_time(h->utc_ms);
                lv_label_set_text_fmt(row_a[r], "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
            } else lv_label_set_text(row_a[r], "time pending");
            lv_obj_set_x(row_a[r], 12);
            lv_obj_set_style_text_color(row_a[r], lv_color_hex(COLOR_DATE), 0);
            lv_label_set_text(row_b[r], "");
            continue;
        }
        uint32_t color = KNOT_HISTORY_SYNCED(h) ? COLOR_SYNCED : COLOR_WAITING;
        if (h->utc_ms) {
            struct tm t = local_time(h->utc_ms);
            lv_label_set_text_fmt(row_a[r], "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
        } else lv_label_set_text(row_a[r], "--:--:--");
        lv_label_set_text(row_b[r], tag_text(KNOT_HISTORY_KIND(h)));
        lv_obj_set_x(row_a[r], 32);
        lv_obj_set_style_text_color(row_a[r], lv_color_hex(color), 0);
        lv_obj_set_style_text_color(row_b[r], lv_color_hex(color), 0);
    }
    bsp_lvgl_unlock();
}

/* ---------- recording actions ---------- */
static void wake(void) {
    last_activity = esp_timer_get_time(); ui_dirty = true;
    if (!screen_on) { bsp_display_backlight(55); screen_on = true; }
}
static void saved_notice(const knot_event_t *event) {
    const char *tag = event->kind == KNOT_KIND_START ? "  #START" : event->kind == KNOT_KIND_END ? "  #END" :
                      event->kind == KNOT_KIND_VOICE ? "  voice" : "";
    if (event->utc_ms) {
        struct tm t = local_time(event->utc_ms);
        snprintf(notice, sizeof(notice), "Saved%s\n%04d-%02d-%02d %02d:%02d:%02d", tag, t.tm_year + 1900,
                 t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    } else snprintf(notice, sizeof(notice), "Saved%s\nTime pending sync", tag);
}
static bool record(uint64_t mono_us, uint8_t kind, uint8_t slot, knot_event_t *event) {
    if (!storage_ok || overflow) return false;
    esp_err_t err = knot_store_add_kind(mono_us, kind, slot, event);
    if (err == ESP_OK) { saved_notice(event); page = 0; ui_dirty = true; return true; }
    if (err == ESP_ERR_NO_MEM) snprintf(notice, sizeof(notice), "Storage full\nThis press NOT saved");
    else storage_failed("Write failed\nRestart required");
    return false;
}
static void start_voice(uint64_t mono_us) {
    int slot = audio_ok && audio_store_ok ? knot_audio_claim() : -1;
    knot_event_t event;
    if (slot < 0) {  /* never lose the moment itself */
        if (record(mono_us, KNOT_KIND_TIME, KNOT_NO_SLOT, &event)) {
            size_t used = strlen(notice);
            snprintf(notice + used, sizeof(notice) - used, "\nNo audio space");
        }
        return;
    }
    if (!record(mono_us, KNOT_KIND_VOICE, (uint8_t)slot, &event)) { knot_audio_release((unsigned)slot); return; }
    if (knot_recorder_start((unsigned)slot, event.seq) != ESP_OK) {
        (void)knot_store_finish_audio(event.seq, 0, 0);
        snprintf(notice, sizeof(notice), "Mic busy\nTime saved");
    }
}
static void handle_key(const message_t *msg) {
    int b = msg->button;
    if (b < 0 || b >= BSP_BTN_COUNT) return;
    wake();
    knot_event_t event;
    if (msg->event == BSP_BTN_PRESS) {
        press_mono[b] = msg->mono_us; long_fired[b] = false; consumed[b] = false;
        if (b == BSP_BTN_OK && knot_recorder_active()) { knot_recorder_stop(); consumed[b] = true; }
    } else if (msg->event == BSP_BTN_LONG) {
        long_fired[b] = true;
        if (consumed[b]) return;
        if (b == BSP_BTN_OK) { if (!knot_recorder_active()) start_voice(press_mono[b]); }
        else { browse = !browse; page = 0; }  /* hold UP/DOWN again to leave browse mode */
    } else if (msg->event == BSP_BTN_RELEASE) {
        if (consumed[b] || long_fired[b]) return;
        if (b == BSP_BTN_OK) record(press_mono[b], KNOT_KIND_TIME, KNOT_NO_SLOT, &event);
        else if (browse) { if (b == BSP_BTN_UP) { if (page) --page; } else ++page; }
        else record(press_mono[b], b == BSP_BTN_UP ? KNOT_KIND_START : KNOT_KIND_END, KNOT_NO_SLOT, &event);
    }
}

/* ---------- boot-time audio reconciliation ---------- */
typedef struct { uint32_t seq; uint8_t slot; } open_voice_t;
static struct { open_voice_t items[KNOT_AUDIO_SLOTS_MAX]; unsigned count; } open_voices;
static void visit_event(const knot_event_t *event, void *context) {
    (void)context;
    if (event->kind != KNOT_KIND_VOICE || event->audio_slot == KNOT_NO_SLOT) return;
    knot_audio_mark_used(event->audio_slot);
    if (!(event->flags & KNOT_FLAG_AUDIO_DONE) && open_voices.count < KNOT_AUDIO_SLOTS_MAX)
        open_voices.items[open_voices.count++] = (open_voice_t){ event->seq, event->audio_slot };
}
static void init_audio_store(void) {
    if (knot_audio_init() != ESP_OK || knot_store_scan(visit_event, NULL) != ESP_OK) return;
    knot_audio_finish_init();
    /* A recording cut by power loss: keep sealed audio, otherwise mark it interrupted. */
    for (unsigned i = 0; i < open_voices.count; ++i) {
        uint32_t bytes = 0, crc = 0;
        if (knot_audio_header(open_voices.items[i].slot, open_voices.items[i].seq, &bytes, &crc) != ESP_OK) bytes = crc = 0;
        if (knot_store_finish_audio(open_voices.items[i].seq, bytes, crc) != ESP_OK) return;
    }
    audio_store_ok = true;
}

void app_main(void) {
    queue = xQueueCreate(24, sizeof(message_t));
    if (!queue) return;
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) return;
    snprintf(device, sizeof(device), "%02x%02x%02x%02x%02x%02x", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    storage_ok = knot_store_init(&boot) == ESP_OK;
    if (storage_ok) init_audio_store();
    (void)bsp_i2c_init(); (void)bsp_battery_init();
    if (bsp_display_init() == ESP_OK && bsp_lvgl_init()) { create_ui(); bsp_display_backlight(55); }
    audio_ok = bsp_audio_init() == ESP_OK && bsp_audio_set_format(KNOT_AUDIO_RATE, 16, 1) == ESP_OK &&
               knot_recorder_init() == ESP_OK;
    snprintf(notice, sizeof(notice), storage_ok ? "Clock pending\nPress key to save offline" : "Storage error\nRecording disabled");
    if (nvs_flash_init() == ESP_OK) {
        buddy_ble_config_t config = { .event_cb = on_ble };
        if (buddy_ble_init(&config) != ESP_OK || buddy_ble_start() != ESP_OK)
            snprintf(notice, sizeof(notice), "Bluetooth error\nOffline recording only");
    } else snprintf(notice, sizeof(notice), "Pairing storage error\nOffline recording only");
    if (bsp_button_init(on_key, NULL) != ESP_OK) snprintf(notice, sizeof(notice), "Button error\nRestart required");
    last_activity = esp_timer_get_time();
    int64_t last_ui = 0;
    for (;;) {
        message_t msg;
        TickType_t wait = stream.active ? pdMS_TO_TICKS(2) : pdMS_TO_TICKS(100);
        if (xQueueReceive(queue, &msg, wait) == pdTRUE) {
            if (msg.kind == MSG_KEY) handle_key(&msg);
            else if (msg.kind == MSG_RX) handle_line(&msg);
            else if (msg.kind == MSG_PIN) { wake(); snprintf(notice, sizeof(notice), "Pairing PIN\n%06" PRIu32, msg.passkey); }
        }
        uint32_t seq, bytes, crc;
        if (knot_recorder_poll(&seq, &bytes, &crc)) {
            if (knot_store_finish_audio(seq, bytes, crc) != ESP_OK) storage_failed("Audio save failed");
            else if (bytes) snprintf(notice, sizeof(notice), "Voice saved\n%" PRIu32 " s", bytes / (KNOT_AUDIO_RATE / 2U));
            else snprintf(notice, sizeof(notice), "Voice failed\nTime kept");
            ui_dirty = true;
        }
        if (knot_recorder_active()) last_activity = esp_timer_get_time();
        transmit();
        if (audio_store_ok && !knot_recorder_active() && !stream.active) (void)knot_audio_maintain();
        if (ui_dirty || esp_timer_get_time() - last_ui > 1000000) {
            update_ui(); last_ui = esp_timer_get_time(); ui_dirty = false;
        }
        if (screen_on && !overflow && esp_timer_get_time() - last_activity > 30000000) {
            bsp_display_backlight(0); screen_on = false; browse = false; page = 0;
        }
    }
}
