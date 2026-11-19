
#include "version.h"
#include "protocol/protocol.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_timer.h"

#include "ble/ble_transport.h"
#include "protocol/frame_buf.h"
#include "scripting/berry_bindings.h"
#include "can/can.h"
#include "can/can_driver.h"

#include "tesla_ble/tesla_ble.h"
#include "tesla_ble/tesla_central.h"
#include "tesla_ble/tesla_vehicle.h"
#include "wifi/wifi.h"
#include "ota/ota.h"
#include "storage/state.h"
#include "storage/fs.h"
#include "nvs.h"

static const char *TAG = "proto";

#define RX_BUF_MAX          (32 * 1024)
#define FRAME_HDR_LEN       4
#define DISPATCH_QUEUE_LEN  8
#define NOTIFY_CHUNK        200

typedef struct {
    uint8_t *data;
    size_t len;
    uint8_t type;
} frame_item_t;

static script_loader_t *s_loader;
static uint8_t s_rx_buf[RX_BUF_MAX];
static frame_buf_t s_rx_fb;
static QueueHandle_t s_frame_queue;
static SemaphoreHandle_t s_send_mutex;

/* ---- trace state ---- */
#define TRACE_QUEUE_LEN 64
#define TRACE_NOTIFY_RETRIES 3

typedef struct {
    uint32_t ts;
    uint32_t id;
    uint8_t bus;
    uint8_t dlc;
    uint8_t data[8];
} trace_item_t;

static volatile uint8_t s_trace_buses;
static QueueHandle_t s_trace_queue;

/* ---- bus status monitor ---- */
static const char *const BUS_STATUS_STR[] = {
    "idle", "good", "tx_error", "rx_error", "error"
};

static uint8_t s_bus_status[CAN_BUS_COUNT];
static bool s_ble_was_connected;
static int s_connect_pushes;

/* ---- helpers ---- */

static int find_script_idx(const char *filename)
{
    for (int i = 0; i < s_loader->count; i++) {
        if (strcmp(s_loader->scripts[i].filename, filename) == 0) {
            return i;
        }
    }
    return -1;
}

static void send_frame_retry(cJSON *resp, int max_retries)
{
    char *json = cJSON_PrintUnformatted(resp);
    if (!json) return;

    size_t jlen = strlen(json);
    uint8_t *frame = malloc(FRAME_HDR_LEN + jlen);
    if (!frame) {
        free(json);
        return;
    }

    frame[0] = (uint8_t)(jlen & 0xFF);
    frame[1] = (uint8_t)((jlen >> 8) & 0xFF);
    frame[2] = (uint8_t)((jlen >> 16) & 0xFF);
    frame[3] = (uint8_t)((jlen >> 24) & 0xFF);
    memcpy(frame + FRAME_HDR_LEN, json, jlen);

    size_t total = FRAME_HDR_LEN + jlen;
    size_t off = 0;
    int retries = 0;

    uint16_t mtu = dorky_ble_mtu();
    uint16_t chunk = (mtu > 203) ? mtu - 3 : NOTIFY_CHUNK;

    xSemaphoreTake(s_send_mutex, portMAX_DELAY);

    while (off < total) {
        if (!dorky_ble_connected()) break;

        size_t n = total - off;
        if (n > chunk) n = chunk;

        if (dorky_ble_notify(frame + off, n) != 0) {
            if (++retries >= max_retries) break;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        retries = 0;
        off += n;

        if (off < total) taskYIELD();
    }

    xSemaphoreGive(s_send_mutex);

    free(frame);
    free(json);
}

static void send_frame(cJSON *resp)
{
    send_frame_retry(resp, 100);
}

static void send_ok(int id, cJSON *result)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "id", id);
    cJSON_AddBoolToObject(resp, "ok", true);

    if (result) {
        cJSON_AddItemToObject(resp, "result", result);
    }

    send_frame(resp);
    cJSON_Delete(resp);
}

static void send_err(int id, const char *msg)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "id", id);
    cJSON_AddBoolToObject(resp, "ok", false);
    cJSON_AddStringToObject(resp, "error", msg);

    send_frame(resp);
    cJSON_Delete(resp);
}

/* ---- unsolicited push ---- */

static void log_push(const char *level, const char *src, const char *msg)
{
    cJSON *push = cJSON_CreateObject();
    cJSON_AddStringToObject(push, "type", "log");
    cJSON_AddStringToObject(push, "level", level);
    cJSON_AddStringToObject(push, "src", src);
    cJSON_AddStringToObject(push, "msg", msg);

    send_frame(push);
    cJSON_Delete(push);
}

static void berry_log_handler(const char *msg)
{
    log_push("info", "berry", msg);
}

static cJSON *script_to_json(const script_entry_t *s);

static void script_timer_error_handler(
    int script_id,
    const char *error_type,
    const char *msg)
{
    if (!s_loader) return;

    const char *fname = NULL;
    int line = berry_error_line(&fname);

    for (int i = 0; i < s_loader->count; i++) {
        script_entry_t *s = &s_loader->scripts[i];
        if (s->script_id != script_id) continue;

        char fullmsg[192];

        berry_format_error(
            fullmsg,
            sizeof(fullmsg),
            s->filename,
            line,
            fname,
            error_type ? error_type : "error",
            msg ? msg : "unknown error"
        );

        snprintf(s->error, sizeof(s->error), "%s", fullmsg);
        s->errored = true;

        script_loader_disable(s_loader, i);
        log_push("error", "scripts", fullmsg);

        cJSON *push = cJSON_CreateObject();
        cJSON_AddStringToObject(push, "type", "script_update");
        cJSON_AddItemToObject(push, "script", script_to_json(s));

        send_frame(push);
        cJSON_Delete(push);
        return;
    }
}

/* ---- vprintf hook ---- */

static vprintf_like_t s_orig_vprintf;
static bool s_hook_active;

static size_t strip_ansi(const char *src, char *dst, size_t dst_sz)
{
    size_t i = 0;
    size_t j = 0;

    while (src[i] && j + 1 < dst_sz) {
        if (src[i] == '\033') {
            i++;
            while (src[i] && src[i] != 'm') i++;
            if (src[i]) i++;
        } else {
            dst[j++] = src[i++];
        }
    }

    while (j > 0 && (dst[j - 1] == '\n' || dst[j - 1] == '\r')) {
        j--;
    }

    dst[j] = '\0';
    return j;
}

static void log_push_raw(const char *raw)
{
    cJSON *push = cJSON_CreateObject();
    cJSON_AddStringToObject(push, "type", "log");
    cJSON_AddStringToObject(push, "raw", raw);

    send_frame(push);
    cJSON_Delete(push);
}

static bool is_nimble_noise(const char *s)
{
    return strstr(s, "NimBLE: 0x") != NULL
        || strstr(s, "GATT procedure initiated: notify") != NULL
        || strstr(s, "NimBLE: att_handle=") != NULL;
}

static bool is_nimble_noise_ble(const char *s)
{
    return strstr(s, ") NimBLE:") != NULL;
}

static int log_vprintf_hook(const char *fmt, va_list args)
{
    va_list args2;
    va_copy(args2, args);

    char buf[320];
    char raw[320];

    vsnprintf(buf, sizeof(buf), fmt, args2);
    size_t raw_len = strip_ansi(buf, raw, sizeof(raw));

    int ret;

    if (raw_len > 0 && is_nimble_noise(raw)) {
        ret = (int)raw_len;
    } else {
        ret = s_orig_vprintf(fmt, args);
    }

    if (!s_hook_active && dorky_ble_connected()) {
        s_hook_active = true;

        if (raw_len > 0 && !is_nimble_noise_ble(raw)) {
            log_push_raw(raw);
        }

        s_hook_active = false;
    }

    va_end(args2);
    return ret;
}

static const char HEX[] = "0123456789ABCDEF";

static void trace_observer(
    int bus_id,
    const twai_message_t *frame,
    uint32_t now_ms)
{
    if (!s_trace_buses) return;
    if (bus_id < 0 || bus_id > 7) return;
    if (!(s_trace_buses & (1u << bus_id))) return;
    if (!dorky_ble_connected() || !s_trace_queue) return;

    trace_item_t item = {0};

    item.ts = now_ms;
    item.id = frame->identifier;
    item.bus = (uint8_t)bus_id;

    int dlc = frame->data_length_code;
    if (dlc > 8) dlc = 8;

    item.dlc = (uint8_t)dlc;
    memcpy(item.data, frame->data, dlc);

    (void)xQueueSend(s_trace_queue, &item, 0);
}

static void trace_tx_worker(void *arg)
{
    (void)arg;

    trace_item_t item;

    for (;;) {
        if (xQueueReceive(s_trace_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!(s_trace_buses & (1u << item.bus))) continue;
        if (!dorky_ble_connected()) continue;

        char hex[17];

        for (int i = 0; i < item.dlc; i++) {
            hex[2 * i] = HEX[(item.data[i] >> 4) & 0xF];
            hex[2 * i + 1] = HEX[item.data[i] & 0xF];
        }

        hex[2 * item.dlc] = '\0';

        cJSON *push = cJSON_CreateObject();

        if (push) {
            cJSON_AddStringToObject(push, "type", "trace");
            cJSON_AddNumberToObject(push, "bus", item.bus);
            cJSON_AddNumberToObject(push, "id", (double)item.id);
            cJSON_AddNumberToObject(push, "ts", (double)item.ts);
            cJSON_AddStringToObject(push, "data", hex);

            send_frame_retry(push, TRACE_NOTIFY_RETRIES);
            cJSON_Delete(push);
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void push_bus_status(int bus, bus_status_t status, uint16_t rate)
{
    cJSON *push = cJSON_CreateObject();

    cJSON_AddStringToObject(push, "type", "bus_status");
    cJSON_AddNumberToObject(push, "bus", bus);
    cJSON_AddStringToObject(push, "status", BUS_STATUS_STR[status]);
    cJSON_AddNumberToObject(push, "rate", rate);

    send_frame(push);
    cJSON_Delete(push);
}

#define MAX_TASKS 16

static void push_cpu_status(void)
{
    static TaskStatus_t s_task_status[MAX_TASKS];

    uint32_t total_runtime;
    UBaseType_t count = uxTaskGetSystemState(
        s_task_status,
        MAX_TASKS,
        &total_runtime
    );

    if (count == 0 || count > MAX_TASKS) return;

    uint32_t idle_runtime = 0;

    for (UBaseType_t i = 0; i < count; i++) {
        if (strcmp(s_task_status[i].pcTaskName, "IDLE") == 0 ||
            strcmp(s_task_status[i].pcTaskName, "IDLE0") == 0) {
            idle_runtime = s_task_status[i].ulRunTimeCounter;
            break;
        }
    }

    static uint32_t s_prev_total = 0;
    static uint32_t s_prev_idle = 0;

    if (s_prev_total == 0) {
        s_prev_total = total_runtime;
        s_prev_idle = idle_runtime;
        return;
    }

    uint32_t delta_total = total_runtime - s_prev_total;
    uint32_t delta_idle = idle_runtime - s_prev_idle;

    s_prev_total = total_runtime;
    s_prev_idle = idle_runtime;

    int load = 0;

    if (delta_total > 0) {
        load = 100 - (int)(delta_idle * 100ULL / delta_total);

        if (load < 0) load = 0;
        if (load > 100) load = 100;
    }

    cJSON *push = cJSON_CreateObject();
    cJSON_AddStringToObject(push, "type", "cpu_status");
    cJSON_AddNumberToObject(push, "load", load);

    send_frame(push);
    cJSON_Delete(push);
}

/* ---- command handlers ---- */

static void handle_ping(int id)
{
    send_ok(id, cJSON_CreateString("pong"));
}

void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void handle_system_info(int id)
{
    cJSON *result = cJSON_CreateObject();

    cJSON_AddStringToObject(result, "fw_version", DORKY_FIRMWARE_VERSION);
    cJSON_AddNumberToObject(result, "proto_version", 6);
    cJSON_AddNumberToObject(result, "scripting_api_version", SCRIPTING_API_VERSION);

    send_ok(id, result);
}

static void factory_reset_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(300));

    ESP_LOGW(TAG, "Factory reset: wiping NVS + LittleFS");

    fs_format();
    state_nvs_erase_all();

    vTaskDelay(pdMS_TO_TICKS(50));
    esp_restart();
}

static void handle_system_factory_reset(int id)
{
    if (ota_in_progress()) {
        ota_abort();
    }

    send_ok(id, NULL);
    xTaskCreate(factory_reset_task, "factory_rst", 4096, NULL, 5, NULL);
}

static void handle_system_reboot(int id)
{
    if (ota_in_progress()) {
        ota_abort();
    }

    send_ok(id, NULL);
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
}

static cJSON *script_to_json(const script_entry_t *s)
{
    cJSON *o = cJSON_CreateObject();

    cJSON_AddStringToObject(o, "filename", s->filename);
    cJSON_AddBoolToObject(o, "enabled", s->enabled);
    cJSON_AddBoolToObject(o, "errored", s->errored);

    if (s->errored) {
        cJSON_AddStringToObject(o, "error", s->error);
    }

    return o;
}

static void handle_script_list(int id)
{
    script_loader_scan(s_loader, s_loader->vm);

    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < s_loader->count; i++) {
        cJSON_AddItemToArray(arr, script_to_json(&s_loader->scripts[i]));
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "scripts", arr);

    send_ok(id, result);
}

static void handle_script_write(int id, cJSON *req)
{
    const cJSON *fn = cJSON_GetObjectItem(req, "filename");
    const cJSON *code = cJSON_GetObjectItem(req, "code");
    const cJSON *runtime = cJSON_GetObjectItem(req, "runtime");

    if (!cJSON_IsString(fn) || !cJSON_IsString(code)) {
        send_err(id, "missing filename or code");
        return;
    }

    if (script_loader_write(
            fn->valuestring,
            code->valuestring,
            strlen(code->valuestring)) != 0) {
        send_err(id, "write failed");
        return;
    }

    if (cJSON_IsString(runtime)) {
        if (script_loader_write_runtime(
                fn->valuestring,
                runtime->valuestring,
                strlen(runtime->valuestring)) != 0) {
            send_err(id, "runtime write failed");
            return;
        }
    } else {
        script_loader_delete_runtime(fn->valuestring);
    }

    int idx = find_script_idx(fn->valuestring);

    if (idx >= 0) {
        script_loader_disable(s_loader, idx);
    }

    int n = script_loader_scan(s_loader, s_loader->vm);

    int new_idx = find_script_idx(fn->valuestring);

    if (new_idx >= 0) {
        s_loader->scripts[new_idx].errored = false;
        s_loader->scripts[new_idx].error[0] = '\0';
    }

    ESP_LOGD(TAG, "script.write %s: %d scripts total", fn->valuestring, n);
    send_ok(id, NULL);
}

static void handle_script_read(int id, cJSON *req)
{
    const cJSON *fn = cJSON_GetObjectItem(req, "filename");

    if (!cJSON_IsString(fn)) {
        send_err(id, "missing filename");
        return;
    }

    static char buf[16 * 1024];

    int n = script_loader_read(fn->valuestring, buf, sizeof(buf));

    if (n < 0) {
        send_err(id, "read failed");
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "code", buf);

    send_ok(id, result);
}

static void handle_script_enable(int id, cJSON *req)
{
    const cJSON *fn = cJSON_GetObjectItem(req, "filename");

    if (!cJSON_IsString(fn)) {
        send_err(id, "missing filename");
        return;
    }

    int idx = find_script_idx(fn->valuestring);

    if (idx < 0) {
        send_err(id, "no such script");
        return;
    }

    if (script_loader_enable(s_loader, idx) != 0) {
        send_err(id, s_loader->scripts[idx].error);
        return;
    }

    script_loader_save_enabled(s_loader);
    send_ok(id, NULL);
}

static void handle_script_disable(int id, cJSON *req)
{
    const cJSON *fn = cJSON_GetObjectItem(req, "filename");

    if (!cJSON_IsString(fn)) {
        send_err(id, "missing filename");
        return;
    }

    int idx = find_script_idx(fn->valuestring);

    if (idx < 0) {
        send_err(id, "no such script");
        return;
    }

    script_loader_disable(s_loader, idx);
    script_loader_save_enabled(s_loader);
    send_ok(id, NULL);
}

static void handle_script_delete(int id, cJSON *req)
{
    const cJSON *fn = cJSON_GetObjectItem(req, "filename");

    if (!cJSON_IsString(fn)) {
        send_err(id, "missing filename");
        return;
    }

    if (script_loader_delete(s_loader, fn->valuestring) != 0) {
        send_err(id, "delete failed");
        return;
    }

    send_ok(id, NULL);
}

static void handle_action_list(int id)
{
    const char *names[64];
    int n = berry_actions_snapshot(names, 64);

    if (n > 64) n = 64;

    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < n; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(names[i]));
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "actions", arr);

    send_ok(id, result);
}

static void handle_action_invoke(int id, cJSON *req)
{
    const cJSON *name = cJSON_GetObjectItem(req, "name");

    if (!cJSON_IsString(name)) {
        send_err(id, "missing name");
        return;
    }

    int rc = berry_action_invoke(name->valuestring);

    if (rc == -1) {
        send_err(id, "no such action");
    } else if (rc == -2) {
        send_err(id, "action raised");
    } else {
        send_ok(id, NULL);
    }
}

static void handle_trace_start(int id, cJSON *req)
{
    uint8_t mask = 0;

    const cJSON *buses = cJSON_GetObjectItem(req, "buses");

    if (cJSON_IsArray(buses)) {
        cJSON *b;

        cJSON_ArrayForEach(b, buses) {
            if (cJSON_IsNumber(b)) {
                int idx = b->valueint;

                if (idx >= 0 && idx <= 7) {
                    mask |= (1u << idx);
                }
            }
        }
    }

    if (mask == 0) mask = 0x03;

    if (s_trace_queue) {
        xQueueReset(s_trace_queue);
    }

    s_trace_buses = mask;

    ESP_LOGI(TAG, "trace.start mask=0x%02x", mask);
    send_ok(id, NULL);
}

static void handle_trace_stop(int id)
{
    s_trace_buses = 0;

    if (s_trace_queue) {
        xQueueReset(s_trace_queue);
    }

    ESP_LOGI(TAG, "trace.stop");
    send_ok(id, NULL);
}

static void handle_ble_status(int id)
{
    cJSON *result = cJSON_CreateObject();

    cJSON_AddBoolToObject(result, "pairing_open", dorky_ble_pairing_open());
    cJSON_AddNumberToObject(result, "bond_count", dorky_ble_bond_count());

    send_ok(id, result);
}

static void handle_ble_unlock_pairing(int id)
{
    dorky_ble_unlock_pairing();
    send_ok(id, NULL);
}

static void handle_ble_mtu(int id)
{
    cJSON *result = cJSON_CreateObject();

    cJSON_AddNumberToObject(result, "mtu", dorky_ble_mtu());

    send_ok(id, result);
}

static void handle_ble_reset_pairs(int id)
{
    send_ok(id, NULL);

    vTaskDelay(pdMS_TO_TICKS(50));
    dorky_ble_reset_pairings();
}

/* ---- Tesla BLE ops ---- */

static cJSON *tesla_ble_status_obj(void)
{
    cJSON *r = cJSON_CreateObject();

    cJSON_AddBoolToObject(r, "has_key", tesla_ble_has_key());

    if (tesla_ble_has_key()) {
        uint8_t pub[TESLA_BLE_PUBKEY_LEN];

        if (tesla_ble_get_public_key(pub) == ESP_OK) {
            char hex[TESLA_BLE_PUBKEY_LEN * 2 + 1];
            static const char H[] = "0123456789abcdef";

            for (int i = 0; i < TESLA_BLE_PUBKEY_LEN; i++) {
                hex[i * 2] = H[pub[i] >> 4];
                hex[i * 2 + 1] = H[pub[i] & 0xF];
            }

            hex[sizeof(hex) - 1] = '\0';
            cJSON_AddStringToObject(r, "public_key_hex", hex);
        }
    }

    char vin[TESLA_VIN_LEN + 2];

    if (tesla_ble_get_vin(vin, sizeof vin) == ESP_OK) {
        cJSON_AddStringToObject(r, "vin", vin);
    }

    return r;
}

static void handle_tesla_set_vin(int id, cJSON *req)
{
    cJSON *vin_item = cJSON_GetObjectItem(req, "vin");

    if (!cJSON_IsString(vin_item)) {
        send_err(id, "missing vin");
        return;
    }

    esp_err_t err = tesla_ble_set_vin(vin_item->valuestring);

    if (err == ESP_ERR_INVALID_ARG) {
        send_err(id, "vin must be 17 characters");
        return;
    }

    if (err != ESP_OK) {
        send_err(id, "failed to store vin");
        return;
    }

    send_ok(id, tesla_ble_status_obj());
}

static void handle_tesla_status(int id)
{
    send_ok(id, tesla_ble_status_obj());
}

static void handle_tesla_gen_key(int id)
{
    if (tesla_ble_generate_key() != ESP_OK) {
        send_err(id, "gen_key failed");
        return;
    }

    send_ok(id, tesla_ble_status_obj());
}

static void handle_tesla_reset(int id)
{
    tesla_ble_reset();
    send_ok(id, NULL);
}

typedef struct {
    int id;
} tesla_scan_ctx_t;

static void tesla_scan_done_cb(
    const tesla_scan_result_t *results,
    size_t count,
    void *ctx)
{
    tesla_scan_ctx_t *sc = (tesla_scan_ctx_t *)ctx;

    cJSON *arr = cJSON_CreateArray();

    for (size_t i = 0; i < count; i++) {
        const tesla_scan_result_t *r = &results[i];

        char addr_str[18];

        snprintf(
            addr_str,
            sizeof(addr_str),
            "%02X:%02X:%02X:%02X:%02X:%02X",
            r->addr[5], r->addr[4], r->addr[3],
            r->addr[2], r->addr[1], r->addr[0]
        );

        cJSON *o = cJSON_CreateObject();

        cJSON_AddStringToObject(o, "addr", addr_str);
        cJSON_AddStringToObject(o, "name", r->name);
        cJSON_AddNumberToObject(o, "rssi", r->rssi);
        cJSON_AddNumberToObject(o, "addr_type", r->addr_type);

        cJSON_AddItemToArray(arr, o);
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "devices", arr);

    send_ok(sc->id, result);
    free(sc);
}

static void handle_tesla_scan(int id, cJSON *req)
{
    cJSON *dur_item = cJSON_GetObjectItem(req, "duration_ms");

    uint32_t duration_ms = cJSON_IsNumber(dur_item)
        ? (uint32_t)dur_item->valueint
        : 5000;

    if (duration_ms < 500) duration_ms = 500;
    if (duration_ms > 30000) duration_ms = 30000;

    tesla_scan_ctx_t *sc = malloc(sizeof(*sc));

    if (!sc) {
        send_err(id, "oom");
        return;
    }

    sc->id = id;

    esp_err_t err = tesla_central_scan_start(
        duration_ms,
        tesla_scan_done_cb,
        sc
    );

    if (err == ESP_ERR_INVALID_STATE) {
        free(sc);
        send_err(id, "scan already in progress");
    } else if (err != ESP_OK) {
        free(sc);
        send_err(id, "scan failed to start — BLE host not ready?");
    }
}

static bool parse_mac(const char *s, uint8_t out[6])
{
    unsigned v[6];

    if (sscanf(
            s,
            "%02X:%02X:%02X:%02X:%02X:%02X",
            &v[5], &v[4], &v[3], &v[2], &v[1], &v[0]) != 6) {
        return false;
    }

    for (int i = 0; i < 6; i++) {
        out[i] = (uint8_t)v[i];
    }

    return true;
}

typedef struct {
    int id;
} tesla_pair_ctx_t;

static int tesla_role_from_string(const char *s)
{
    if (!s) return 3;

    char lower[24];
    size_t i = 0;

    while (s[i] != '\0' && i < sizeof(lower) - 1) {
        char c = s[i];
        lower[i] = (c >= 'A' && c <= 'Z')
            ? (char)(c + ('a' - 'A'))
            : c;
        i++;
    }

    lower[i] = '\0';

    if (strcmp(lower, "owner") == 0) return 2;
    if (strcmp(lower, "driver") == 0) return 3;

    if (strcmp(lower, "vehicle_monitor") == 0 ||
        strcmp(lower, "monitor") == 0) {
        return 5;
    }

    if (strcmp(lower, "charging_manager") == 0 ||
        strcmp(lower, "charging") == 0) {
        return 6;
    }

    if (strcmp(lower, "guest") == 0) return 8;

    return -1;
}

static void tesla_pair_done_cb(
    bool success,
    const char *error,
    void *vctx)
{
    tesla_pair_ctx_t *ctx = (tesla_pair_ctx_t *)vctx;

    if (success) {
        send_ok(ctx->id, NULL);
    } else {
        send_err(ctx->id, error ? error : "pair failed");
    }

    free(ctx);
}

static void handle_tesla_pair(int id, cJSON *req)
{
    cJSON *addr_item = cJSON_GetObjectItem(req, "addr");
    cJSON *type_item = cJSON_GetObjectItem(req, "addr_type");
    cJSON *role_item = cJSON_GetObjectItem(req, "role");

    if (!cJSON_IsString(addr_item)) {
        send_err(id, "missing addr");
        return;
    }

    uint8_t addr[6];

    if (!parse_mac(addr_item->valuestring, addr)) {
        send_err(id, "addr must be AA:BB:CC:DD:EE:FF");
        return;
    }

    uint8_t addr_type = cJSON_IsNumber(type_item)
        ? (uint8_t)type_item->valueint
        : 0;

    int role = 3;

    if (cJSON_IsString(role_item)) {
        role = tesla_role_from_string(role_item->valuestring);

        if (role < 0) {
            send_err(
                id,
                "role must be one of: driver, owner, "
                "vehicle_monitor, charging_manager, guest"
            );
            return;
        }
    }

    tesla_pair_ctx_t *ctx = malloc(sizeof(*ctx));

    if (!ctx) {
        send_err(id, "oom");
        return;
    }

    ctx->id = id;

    esp_err_t err = tesla_vehicle_pair(
        addr,
        addr_type,
        role,
        tesla_pair_done_cb,
        ctx
    );

    if (err == ESP_ERR_NOT_FOUND) {
        free(ctx);
        send_err(id, "no keypair — run tesla.gen_key first");
        return;
    }

    if (err == ESP_ERR_INVALID_STATE) {
        free(ctx);
        send_err(id, "already connected");
        return;
    }

    if (err == ESP_ERR_INVALID_ARG) {
        free(ctx);
        send_err(id, "invalid role");
        return;
    }

    if (err != ESP_OK) {
        free(ctx);
        send_err(id, "connect failed");
        return;
    }
}

static void handle_wifi_status(int id)
{
    char ssid[33];
    char ip[16];

    wifi_get_ssid(ssid, sizeof(ssid));
    wifi_get_ip(ip, sizeof(ip));

    cJSON *result = cJSON_CreateObject();

    cJSON_AddBoolToObject(result, "connected", wifi_connected());
    cJSON_AddStringToObject(result, "ssid", ssid);
    cJSON_AddStringToObject(result, "ip", ip);

    send_ok(id, result);
}

static void handle_wifi_set_creds(int id, cJSON *req)
{
    const cJSON *ssid_j = cJSON_GetObjectItem(req, "ssid");
    const cJSON *psk_j = cJSON_GetObjectItem(req, "psk");

    if (!cJSON_IsString(ssid_j)) {
        send_err(id, "missing ssid");
        return;
    }

    const char *psk = cJSON_IsString(psk_j) ? psk_j->valuestring : "";

    if (wifi_set_creds(ssid_j->valuestring, psk) != 0) {
        send_err(id, "set failed");
        return;
    }

    send_ok(id, NULL);
}

#define SECRETS_NS "secrets"
#define BUSCONFIG_NS "busconfig"

static void handle_settings_set_secret(int id, cJSON *req)
{
    const cJSON *name_j = cJSON_GetObjectItem(req, "name");
    const cJSON *value_j = cJSON_GetObjectItem(req, "value");

    if (!cJSON_IsString(name_j) || name_j->valuestring[0] == '\0' ||
        !cJSON_IsString(value_j) || value_j->valuestring[0] == '\0') {
        send_err(id, "missing name or value");
        return;
    }

    if (strlen(name_j->valuestring) > 15) {
        send_err(id, "name too long (max 15)");
        return;
    }

    if (state_set(
            SECRETS_NS,
            name_j->valuestring,
            value_j->valuestring) != ESP_OK) {
        send_err(id, "write failed");
        return;
    }

    send_ok(id, NULL);
}

static void handle_settings_has_secret(int id, cJSON *req)
{
    const cJSON *name_j = cJSON_GetObjectItem(req, "name");

    if (!cJSON_IsString(name_j) || name_j->valuestring[0] == '\0') {
        send_err(id, "missing name");
        return;
    }

    char buf[4];

    esp_err_t err = state_get(
        SECRETS_NS,
        name_j->valuestring,
        buf,
        sizeof(buf)
    );

    bool has = (err == ESP_OK || err == ESP_ERR_NVS_INVALID_LENGTH);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "set", has);

    send_ok(id, result);
}

static void handle_settings_get_secret(int id, cJSON *req)
{
    const cJSON *name_j = cJSON_GetObjectItem(req, "name");

    if (!cJSON_IsString(name_j) || name_j->valuestring[0] == '\0') {
        send_err(id, "missing name");
        return;
    }

    char buf[256];
    cJSON *result = cJSON_CreateObject();

    esp_err_t err = state_get(
        SECRETS_NS,
        name_j->valuestring,
        buf,
        sizeof(buf)
    );

    if (err == ESP_OK) {
        cJSON_AddStringToObject(result, "value", buf);
    } else {
        cJSON_AddNullToObject(result, "value");
    }

    send_ok(id, result);
}

static void handle_settings_clear_secret(int id, cJSON *req)
{
    const cJSON *name_j = cJSON_GetObjectItem(req, "name");

    if (!cJSON_IsString(name_j) || name_j->valuestring[0] == '\0') {
        send_err(id, "missing name");
        return;
    }

    esp_err_t err = state_remove(SECRETS_NS, name_j->valuestring);

    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        send_err(id, "clear failed");
        return;
    }

    send_ok(id, NULL);
}

static void handle_bus_get_config(int id)
{
    cJSON *arr = cJSON_CreateArray();

    for (int b = 0; b < CAN_BUS_COUNT; b++) {
        cJSON *o = cJSON_CreateObject();

        cJSON_AddNumberToObject(o, "bus", b);
        cJSON_AddNumberToObject(
            o,
            "bitrate_kbps",
            (double)can_bus_get_bitrate(b)
        );

        cJSON_AddItemToArray(arr, o);
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "buses", arr);

    send_ok(id, result);
}

static void handle_bus_set_bitrate(int id, cJSON *req)
{
    const cJSON *bus_j = cJSON_GetObjectItem(req, "bus");
    const cJSON *kbps_j = cJSON_GetObjectItem(req, "bitrate_kbps");

    if (!cJSON_IsNumber(bus_j) || !cJSON_IsNumber(kbps_j)) {
        send_err(id, "missing bus or bitrate_kbps");
        return;
    }

    int bus = bus_j->valueint;
    uint32_t kbps = (uint32_t)kbps_j->valuedouble;

    char key[4];
    char val[8];

    snprintf(key, sizeof(key), "b%d", bus);
    snprintf(val, sizeof(val), "%" PRIu32, kbps);

    if (state_set(BUSCONFIG_NS, key, val) != ESP_OK) {
        send_err(id, "NVS write failed");
        return;
    }

    esp_err_t err = can_bus_set_bitrate(bus, kbps);

    if (err != ESP_OK) {
        send_err(id, esp_err_to_name(err));
        return;
    }

    send_ok(id, NULL);
}

static void handle_log_set_level(int id, cJSON *req)
{
    const cJSON *level_j = cJSON_GetObjectItem(req, "level");

    if (!cJSON_IsString(level_j)) {
        send_err(id, "missing level");
        return;
    }

    const char *lvl = level_j->valuestring;
    esp_log_level_t level;

    if (strcmp(lvl, "none") == 0) {
        level = ESP_LOG_NONE;
    } else if (strcmp(lvl, "error") == 0) {
        level = ESP_LOG_ERROR;
    } else if (strcmp(lvl, "warn") == 0) {
        level = ESP_LOG_WARN;
    } else if (strcmp(lvl, "info") == 0) {
        level = ESP_LOG_INFO;
    } else if (strcmp(lvl, "debug") == 0) {
        level = ESP_LOG_DEBUG;
    } else if (strcmp(lvl, "verbose") == 0) {
        level = ESP_LOG_VERBOSE;
    } else {
        send_err(id, "invalid level");
        return;
    }

    const cJSON *tag_j = cJSON_GetObjectItem(req, "tag");
    const char *tag = (
        cJSON_IsString(tag_j) && tag_j->valuestring[0]
    ) ? tag_j->valuestring : "*";

    esp_log_level_set(tag, level);

    if (strcmp(tag, "*") == 0) {
        esp_log_level_set("ble", ESP_LOG_WARN);
    }

    send_ok(id, NULL);
}

static void handle_log_get_level(int id)
{
    esp_log_level_t level = esp_log_level_get("*");

    const char *lvl_str =
        level <= ESP_LOG_NONE ? "none" :
        level == ESP_LOG_ERROR ? "error" :
        level == ESP_LOG_WARN ? "warn" :
        level == ESP_LOG_INFO ? "info" :
        level == ESP_LOG_DEBUG ? "debug" :
        level >= ESP_LOG_VERBOSE ? "verbose" : "info";

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "level", lvl_str);

    send_ok(id, result);
}

static void handle_sim_set(int id, cJSON *req)
{
    const cJSON *enabled_j = cJSON_GetObjectItem(req, "enabled");

    if (!cJSON_IsBool(enabled_j)) {
        send_err(id, "missing enabled");
        return;
    }

    bool enabled = cJSON_IsTrue(enabled_j);

    int bus_filter = -1;
    const cJSON *bus_j = cJSON_GetObjectItem(req, "bus");

    if (cJSON_IsNumber(bus_j)) {
        bus_filter = bus_j->valueint;
    }

    for (int b = 0; b < 2; b++) {
        if (bus_filter >= 0 && bus_filter != b) continue;

        can_t *eng = berry_get_bus(b);

        if (eng) {
            can_set_sim_mode(eng, enabled);
        }
    }

    send_ok(id, NULL);
}

/* ---- OTA handlers ---- */

static void handle_ota_begin(int id)
{
    char err[128];
    size_t max_size = 0;

    if (ota_begin(&max_size, err, sizeof(err)) != 0) {
        send_err(id, err);
        return;
    }

    cJSON *result = cJSON_CreateObject();

    cJSON_AddNumberToObject(result, "max_size", (double)max_size);
    cJSON_AddNumberToObject(result, "max_frame", (double)RX_BUF_MAX);

    send_ok(id, result);
}

static void handle_ota_end(int id, cJSON *req)
{
    bool reboot = true;

    const cJSON *reboot_j = cJSON_GetObjectItem(req, "reboot");

    if (cJSON_IsBool(reboot_j)) {
        reboot = cJSON_IsTrue(reboot_j);
    }

    size_t expected_len = 0;
    const cJSON *size_j = cJSON_GetObjectItem(req, "size");

    if (cJSON_IsNumber(size_j) && size_j->valuedouble >= 0) {
        expected_len = (size_t)size_j->valuedouble;
    }

    char err[128];

    if (ota_end(false, expected_len, err, sizeof(err)) != 0) {
        send_err(id, err);
        return;
    }

    send_ok(id, NULL);

    if (reboot) {
        xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    }
}

static void handle_ota_abort(int id)
{
    ota_abort();
    send_ok(id, NULL);
}

/* ---- frame parser / dispatch ---- */

static void dispatch_frame(const uint8_t *payload, size_t len)
{
    cJSON *req = cJSON_ParseWithLength((const char *)payload, len);

    if (!req) {
        ESP_LOGW(TAG, "invalid JSON frame (%zu bytes)", len);
        return;
    }

    const cJSON *op = cJSON_GetObjectItem(req, "op");
    const cJSON *id_node = cJSON_GetObjectItem(req, "id");

    int id = cJSON_IsNumber(id_node) ? id_node->valueint : 0;

    if (!cJSON_IsString(op)) {
        send_err(id, "missing op");
        cJSON_Delete(req);
        return;
    }

    const char *op_s = op->valuestring;

    ESP_LOGD(TAG, "op=%s id=%d", op_s, id);

    if (strcmp(op_s, "ping") == 0)
        handle_ping(id);
    else if (strcmp(op_s, "system.info") == 0)
        handle_system_info(id);
    else if (strcmp(op_s, "system.reboot") == 0)
        handle_system_reboot(id);
    else if (strcmp(op_s, "system.factory_reset") == 0)
        handle_system_factory_reset(id);
    else if (strcmp(op_s, "script.list") == 0)
        handle_script_list(id);
    else if (strcmp(op_s, "script.write") == 0)
        handle_script_write(id, req);
    else if (strcmp(op_s, "script.read") == 0)
        handle_script_read(id, req);
    else if (strcmp(op_s, "script.enable") == 0)
        handle_script_enable(id, req);
    else if (strcmp(op_s, "script.disable") == 0)
        handle_script_disable(id, req);
    else if (strcmp(op_s, "script.delete") == 0)
        handle_script_delete(id, req);
    else if (strcmp(op_s, "action.list") == 0)
        handle_action_list(id);
    else if (strcmp(op_s, "action.invoke") == 0)
        handle_action_invoke(id, req);
    else if (strcmp(op_s, "trace.start") == 0)
        handle_trace_start(id, req);
    else if (strcmp(op_s, "trace.stop") == 0)
        handle_trace_stop(id);
    else if (strcmp(op_s, "bus.get_config") == 0)
        handle_bus_get_config(id);
    else if (strcmp(op_s, "bus.set_bitrate") == 0)
        handle_bus_set_bitrate(id, req);
    else if (strcmp(op_s, "sim.set") == 0)
        handle_sim_set(id, req);
    else if (strcmp(op_s, "log.set_level") == 0)
        handle_log_set_level(id, req);
    else if (strcmp(op_s, "log.get_level") == 0)
        handle_log_get_level(id);
    else if (strcmp(op_s, "tesla.status") == 0)
        handle_tesla_status(id);
    else if (strcmp(op_s, "tesla.gen_key") == 0)
        handle_tesla_gen_key(id);
    else if (strcmp(op_s, "tesla.reset") == 0)
        handle_tesla_reset(id);
    else if (strcmp(op_s, "tesla.set_vin") == 0)
        handle_tesla_set_vin(id, req);
    else if (strcmp(op_s, "tesla.scan") == 0)
        handle_tesla_scan(id, req);
    else if (strcmp(op_s, "tesla.pair") == 0)
        handle_tesla_pair(id, req);
    else if (strcmp(op_s, "ble.status") == 0)
        handle_ble_status(id);
    else if (strcmp(op_s, "ble.unlock_pairing") == 0)
        handle_ble_unlock_pairing(id);
    else if (strcmp(op_s, "ble.reset_pairs") == 0)
        handle_ble_reset_pairs(id);
    else if (strcmp(op_s, "ble.mtu") == 0)
        handle_ble_mtu(id);
    else if (strcmp(op_s, "wifi.status") == 0)
        handle_wifi_status(id);
    else if (strcmp(op_s, "wifi.set_creds") == 0)
        handle_wifi_set_creds(id, req);
    else if (strcmp(op_s, "settings.set_secret") == 0)
        handle_settings_set_secret(id, req);
    else if (strcmp(op_s, "settings.get_secret") == 0)
        handle_settings_get_secret(id, req);
    else if (strcmp(op_s, "settings.has_secret") == 0)
        handle_settings_has_secret(id, req);
    else if (strcmp(op_s, "settings.clear_secret") == 0)
        handle_settings_clear_secret(id, req);
    else if (strcmp(op_s, "ota.begin") == 0)
        handle_ota_begin(id);
    else if (strcmp(op_s, "ota.end") == 0)
        handle_ota_end(id, req);
    else if (strcmp(op_s, "ota.abort") == 0)
        handle_ota_abort(id);
    else
        send_err(id, "unknown op");

    cJSON_Delete(req);
}

void protocol_on_ble_write(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;

    if (frame_buf_append(&s_rx_fb, data, len) != 0) {
        ESP_LOGE(TAG, "rx buffer overflow, resetting");
        return;
    }

    for (;;) {
        const uint8_t *payload;
        size_t plen;

        int rc = frame_buf_next(&s_rx_fb, &payload, &plen);

        if (rc == 0) break;

        if (rc < 0) {
            uint32_t hdr = (uint32_t)s_rx_buf[0]
                         | ((uint32_t)s_rx_buf[1] << 8)
                         | ((uint32_t)s_rx_buf[2] << 16)
                         | ((uint32_t)s_rx_buf[3] << 24);

            ESP_LOGE(
                TAG,
                "frame length %u exceeds buffer (%d), resetting",
                (unsigned)(hdr & 0x0FFFFFFFu),
                RX_BUF_MAX
            );

            return;
        }

        uint8_t *copy = malloc(plen);

        if (copy) {
            memcpy(copy, payload, plen);

            frame_item_t fi = {
                copy,
                plen,
                frame_buf_last_type(&s_rx_fb)
            };

            if (xQueueSend(s_frame_queue, &fi, 0) != pdTRUE) {
                ESP_LOGW(TAG, "dispatch queue full, frame dropped");
                free(copy);
            }
        }

        frame_buf_consume(&s_rx_fb);
    }
}

static void dispatch_ota_write_binary(const uint8_t *payload, size_t len)
{
    if (len < 4) {
        ESP_LOGW(TAG, "binary ota.write frame too short (%zu)", len);
        return;
    }

    int id = (int)((uint32_t)payload[0]
                 | ((uint32_t)payload[1] << 8)
                 | ((uint32_t)payload[2] << 16)
                 | ((uint32_t)payload[3] << 24));

    char err[128];

    if (ota_write(payload + 4, len - 4, err, sizeof(err)) != 0) {
        send_err(id, err);
        return;
    }

    send_ok(id, NULL);
}

void protocol_tick(void)
{
    frame_item_t fi;

    while (xQueueReceive(s_frame_queue, &fi, 0) == pdTRUE) {
        switch (fi.type) {
            case FRAME_TYPE_OTA_WRITE:
                dispatch_ota_write_binary(fi.data, fi.len);
                break;

            case FRAME_TYPE_JSON:
            default:
                dispatch_frame(fi.data, fi.len);
                break;
        }

        free(fi.data);
    }

    static uint32_t s_status_ms = 0;
    static uint32_t s_heartbeat_ms = 0;

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    bool ble_up = dorky_ble_connected();

    if (ble_up && !s_ble_was_connected) {
        memset(s_bus_status, 0xFF, sizeof(s_bus_status));

        s_status_ms = now;
        s_heartbeat_ms = now;
        s_connect_pushes = 5;
    }

    s_ble_was_connected = ble_up;

    if (ble_up && (now - s_status_ms) >= 200) {
        s_status_ms = now;

        bool heartbeat = (now - s_heartbeat_ms) >= 1000;

        if (heartbeat) {
            s_heartbeat_ms = now;
        }

        for (int b = 0; b < CAN_BUS_COUNT; b++) {
            bus_status_t ns = can_bus_health(
                b,
                now,
                can_last_rx_ms(b)
            );

            bool changed = (uint8_t)ns != s_bus_status[b];

            if (changed || heartbeat || s_connect_pushes > 0) {
                s_bus_status[b] = (uint8_t)ns;

                push_bus_status(b, ns, can_bus_rx_rate(b));

                if (changed) {
                    if (ns == BUS_GOOD) {
                        ESP_LOGD(
                            TAG,
                            "bus%d status -> %s",
                            b,
                            BUS_STATUS_STR[ns]
                        );
                    } else {
                        ESP_LOGI(
                            TAG,
                            "bus%d status -> %s",
                            b,
                            BUS_STATUS_STR[ns]
                        );
                    }
                }
            }
        }

        if (s_connect_pushes > 0) {
            s_connect_pushes--;
        }
    }

    #define CPU_PUSH_INTERVAL_MS 5000

    static uint32_t s_cpu_ms = 0;

    if (ble_up && (now - s_cpu_ms) >= CPU_PUSH_INTERVAL_MS) {
        s_cpu_ms = now;
        push_cpu_status();
    }
}

static const char *script_name_by_id(int script_id)
{
    if (!s_loader) return NULL;

    for (int i = 0; i < s_loader->count; i++) {
        if (s_loader->scripts[i].script_id == script_id) {
            return s_loader->scripts[i].filename;
        }
    }

    return NULL;
}

void protocol_init(script_loader_t *loader)
{
    s_send_mutex = xSemaphoreCreateMutex();

    if (!s_send_mutex) {
        ESP_LOGE(TAG, "send mutex creation failed");
        abort();
    }

    s_loader = loader;

    frame_buf_init(&s_rx_fb, s_rx_buf, sizeof(s_rx_buf));

    s_frame_queue = xQueueCreate(
        DISPATCH_QUEUE_LEN,
        sizeof(frame_item_t)
    );

    berry_set_log_handler(berry_log_handler);
    berry_set_timer_error_handler(script_timer_error_handler);
    berry_set_script_name_callback(script_name_by_id);

    s_trace_queue = xQueueCreate(
        TRACE_QUEUE_LEN,
        sizeof(trace_item_t)
    );

    if (s_trace_queue) {
        if (xTaskCreate(
                trace_tx_worker,
                "trace_tx",
                4096,
                NULL,
                2,
                NULL) != pdPASS) {
            ESP_LOGE(TAG, "trace TX worker creation failed");

            vQueueDelete(s_trace_queue);
            s_trace_queue = NULL;
        }
    }

    if (s_trace_queue) {
        can_set_raw_observer(trace_observer);
    } else {
        ESP_LOGE(TAG, "trace disabled: queue unavailable");
    }

    memset(s_bus_status, 0xFF, sizeof(s_bus_status));

    s_orig_vprintf = esp_log_set_vprintf(log_vprintf_hook);
}
