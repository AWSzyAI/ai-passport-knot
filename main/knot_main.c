#include "knot_store.h"
#include "buddy_ble.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_battery.h"
#include "bsp_audio.h"
#include "bsp_i2c.h"
#include "cJSON.h"
#include "esp_mac.h"
#include "esp_timer.h"
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

typedef struct { int kind, button; uint64_t mono_us; uint32_t generation, passkey; char line[256]; } message_t;
static QueueHandle_t queue;
static volatile bool overflow;
static bool storage_ok, audio_ok, ui_ok, synced;
static uint32_t boot, session, pending;
static char device[13];
static char notice[80] = "Starting...";
#define HISTORY_ROWS 8
#define COLOR_SYNCED 0x4ade80
#define COLOR_WAITING 0xfacc15
static lv_obj_t *status_label, *count_label, *battery_label, *history_label, *rows[HISTORY_ROWS];
static bool screen_on = true, ui_dirty;
static unsigned history_offset;
static int64_t last_activity, last_send;

static void on_key(bsp_btn_t button, bsp_btn_ev_t event, void *context) {
    (void)context;
    if (event != BSP_BTN_PRESS) return;
    message_t msg = { .kind = button == CONFIG_KNOT_RECORD_BUTTON ? 1 : 2, .button = (int)button,
                      .mono_us = (uint64_t)esp_timer_get_time() };
    if (xQueueSend(queue, &msg, 0) != pdTRUE) overflow = true;
}
static void on_ble(const buddy_ble_event_t *event, void *context) {
    (void)context;
    message_t msg = {0};
    if (event->type == BUDDY_BLE_EVENT_RX_LINE) {
        if (event->data.rx_line.length >= sizeof(msg.line)) return;
        msg.kind = 3; msg.generation = event->data.rx_line.connection_generation;
        memcpy(msg.line, event->data.rx_line.data, event->data.rx_line.length);
    } else if (event->type == BUDDY_BLE_EVENT_PASSKEY) {
        msg.kind = 4; msg.passkey = event->data.passkey.value;
    } else return;
    if (xQueueSend(queue, &msg, 0) != pdTRUE) overflow = true;
}
static void send_line(const char *line) {
    (void)buddy_ble_send_for_generation(line, strlen(line), session);
}
static bool number(const cJSON *json, const char *key, int64_t min, int64_t max, int64_t *out) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(json, key);
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) ||
        value->valuedouble < (double)min || value->valuedouble > (double)max ||
        floor(value->valuedouble) != value->valuedouble) return false;
    *out = (int64_t)value->valuedouble;
    return true;
}
static void handle_line(const message_t *msg) {
    if (!buddy_ble_is_generation_secure(msg->generation)) return;
    cJSON *json = cJSON_Parse(msg->line);
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(json, "type");
    int64_t version;
    if (!json || !cJSON_IsString(type) || !number(json, "v", 1, 1, &version)) goto done;
    char reply[240];
    if (!strcmp(type->valuestring, "hello")) {
        session = msg->generation; synced = false; pending = 0;
        snprintf(reply, sizeof(reply), "{\"v\":1,\"type\":\"hello\",\"device\":\"%s\",\"boot\":%" PRIu32 ",\"pending\":%u}\n",
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
        if (knot_store_sync((uint64_t)mono, utc) != ESP_OK) {
            storage_ok = false; snprintf(notice, sizeof(notice), "Storage error. Restart required."); goto done;
        }
        synced = true; last_send = 0;
        send_line("{\"v\":1,\"type\":\"synced\"}\n");
        snprintf(notice, sizeof(notice), "Clock synced\nReady to record");
    } else if (storage_ok && session == msg->generation && synced && !strcmp(type->valuestring, "ack")) {
        int64_t seq;
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(json, "device");
        if (!cJSON_IsString(id) || strcmp(id->valuestring, device) ||
            !number(json, "seq", 1, UINT32_MAX - 1, &seq) || seq != pending) goto done;
        if (knot_store_ack(pending) != ESP_OK) {
            storage_ok = false; snprintf(notice, sizeof(notice), "Storage error. Restart required."); goto done;
        }
        pending = 0; last_send = 0; ui_dirty = true;
    }
done:
    cJSON_Delete(json);
}
static void transmit(void) {
    if (!storage_ok || !synced || !buddy_ble_is_generation_secure(session)) return;
    if (esp_timer_get_time() - last_send < 2000000) return;
    last_send = esp_timer_get_time();
    knot_event_t event;
    esp_err_t err = knot_store_first(&event);
    if (err == ESP_ERR_NOT_FOUND) return;
    if (err != ESP_OK) { storage_ok = false; snprintf(notice, sizeof(notice), "Record read error"); return; }
    char timestamp[32], line[320];
    if (event.utc_ms) snprintf(timestamp, sizeof(timestamp), "%" PRId64, event.utc_ms);
    else snprintf(timestamp, sizeof(timestamp), "null");
    snprintf(line, sizeof(line),
             "{\"v\":1,\"type\":\"event\",\"device\":\"%s\",\"seq\":%" PRIu32
             ",\"boot\":%" PRIu32 ",\"mono_us\":%" PRIu64 ",\"utc_ms\":%s}\n",
             device, event.seq, event.boot, event.mono_us, timestamp);
    pending = event.seq;
    send_line(line);
}
static void beep(void) {
    if (!audio_ok) return;
    int16_t pcm[400];
    for (unsigned i = 0; i < 400; ++i) pcm[i] = (i % 16 < 8) ? 900 : -900;
    (void)bsp_audio_write(pcm, sizeof(pcm));
}
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
    lv_obj_set_style_text_color(history_label, lv_color_hex(0x8fa3b8), 0);
    lv_obj_align(history_label, LV_ALIGN_TOP_LEFT, 12, 120);
    for (unsigned i = 0; i < HISTORY_ROWS; ++i) {
        rows[i] = lv_label_create(screen); lv_obj_set_width(rows[i], 220);
        lv_label_set_text(rows[i], "");
        lv_obj_align(rows[i], LV_ALIGN_TOP_LEFT, 12, 142 + 21 * (int)i);
    }
    lv_screen_load(screen); ui_ok = true;
    bsp_lvgl_unlock();
}
static void format_local(int64_t utc_ms, char *out, size_t size) {
    time_t seconds = (time_t)(utc_ms / 1000 + 8 * 3600);
    struct tm local; gmtime_r(&seconds, &local);
    snprintf(out, size, "%04d-%02d-%02d %02d:%02d:%02d", local.tm_year + 1900, local.tm_mon + 1,
             local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
}
static void update_ui(void) {
    if (!ui_ok) return;
    int battery = bsp_battery_soc();
    knot_history_t history[KNOT_HISTORY];
    unsigned total = knot_store_history(history, KNOT_HISTORY);
    /* Page-aligned: offset is always a multiple of HISTORY_ROWS, clamped to the last page. */
    unsigned last_page = total ? (total - 1) / HISTORY_ROWS : 0;
    if (history_offset / HISTORY_ROWS > last_page) history_offset = last_page * HISTORY_ROWS;
    if (!bsp_lvgl_lock(100)) return;
    lv_label_set_text_fmt(count_label, "BT %s  |  waiting %u",
                          buddy_ble_is_encrypted() ? "connected" : "offline", knot_store_count());
    lv_label_set_text(status_label, overflow ? "INPUT OVERFLOW\nSome presses NOT saved" : notice);
    if (battery >= 0) lv_label_set_text_fmt(battery_label, "%d%%", battery);
    else lv_label_set_text(battery_label, "--%");
    if (!total) lv_label_set_text(history_label, "History: none yet");
    else lv_label_set_text_fmt(history_label, "History  page %u/%u   UP/DOWN",
                               history_offset / HISTORY_ROWS + 1, last_page + 1);
    for (unsigned i = 0; i < HISTORY_ROWS; ++i) {
        unsigned index = history_offset + i;
        if (index >= total) { lv_label_set_text(rows[i], ""); continue; }
        char when[24];
        if (history[index].utc_ms) format_local(history[index].utc_ms, when, sizeof(when));
        else snprintf(when, sizeof(when), "time pending");
        lv_label_set_text_fmt(rows[i], "#%-4" PRIu32 " %s", history[index].seq, when);
        lv_obj_set_style_text_color(rows[i], lv_color_hex(history[index].synced ? COLOR_SYNCED : COLOR_WAITING), 0);
    }
    bsp_lvgl_unlock();
}
void app_main(void) {
    queue = xQueueCreate(24, sizeof(message_t));
    if (!queue) return;
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) return;
    snprintf(device, sizeof(device), "%02x%02x%02x%02x%02x%02x", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    storage_ok = knot_store_init(&boot) == ESP_OK;
    (void)bsp_i2c_init(); (void)bsp_battery_init();
    if (bsp_display_init() == ESP_OK && bsp_lvgl_init()) { create_ui(); bsp_display_backlight(55); }
    audio_ok = bsp_audio_init() == ESP_OK && bsp_audio_set_format(16000, 16, 1) == ESP_OK;
    if (audio_ok) bsp_audio_set_volume(35);
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
        if (xQueueReceive(queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
            bool was_on = screen_on;
            if (msg.kind == 1 || msg.kind == 2 || msg.kind == 4) {
                last_activity = esp_timer_get_time(); bsp_display_backlight(55); screen_on = true;
                ui_dirty = true;
            }
            /* UP/DOWN scroll history; a press on a dark screen only wakes it. */
            if (msg.kind == 2 && was_on) {
                if (msg.button == BSP_BTN_UP)
                    history_offset = history_offset >= HISTORY_ROWS ? history_offset - HISTORY_ROWS : 0;
                else if (msg.button == BSP_BTN_DOWN) history_offset += HISTORY_ROWS;
            }
            if (msg.kind == 1) {
                knot_event_t event;
                if (storage_ok && !overflow) {
                    esp_err_t err = knot_store_add(msg.mono_us, &event);
                    if (err == ESP_OK) {
                        history_offset = 0;
                        if (event.utc_ms) {
                            char when[24]; format_local(event.utc_ms, when, sizeof(when));
                            snprintf(notice, sizeof(notice), "Saved #%" PRIu32 "\n%s", event.seq, when);
                        } else snprintf(notice, sizeof(notice), "Saved # %" PRIu32 "\nTime pending sync", event.seq);
                        beep();
                    } else if (err == ESP_ERR_NO_MEM) snprintf(notice, sizeof(notice), "Storage full\nThis press NOT saved");
                    else { storage_ok = false; snprintf(notice, sizeof(notice), "Write failed\nRestart required"); }
                }
            } else if (msg.kind == 3) handle_line(&msg);
            else if (msg.kind == 4) snprintf(notice, sizeof(notice), "Pairing PIN\n%06" PRIu32, msg.passkey);
        }
        transmit();
        if (ui_dirty || esp_timer_get_time() - last_ui > 1000000) {
            update_ui(); last_ui = esp_timer_get_time(); ui_dirty = false;
        }
        if (screen_on && !overflow && esp_timer_get_time() - last_activity > 30000000) {
            bsp_display_backlight(0); screen_on = false;
        }
    }
}
