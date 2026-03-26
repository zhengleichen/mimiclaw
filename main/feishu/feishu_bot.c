#include "feishu_bot.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "proxy/http_proxy.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_websocket_client.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "feishu";

/* ── Static state ──────────────────────────────────────────────── */

static char s_app_id[64]      = MIMI_SECRET_FEISHU_APP_ID;
static char s_app_secret[64]  = MIMI_SECRET_FEISHU_APP_SECRET;
static char s_tenant_token[256] = {0};
static time_t s_token_expire = 0;
static int s_service_id = 0;
static esp_websocket_client_handle_t s_ws_client = NULL;

/* Dedup ring buffer */
#define DEDUP_SIZE 64
static char s_dedup[DEDUP_SIZE][32];
static int  s_dedup_idx = 0;

/* ── HTTP response accumulator ────────────────────────────────── */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_resp_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_t *resp = (http_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && resp) {
        if (resp->len + evt->data_len < resp->cap) {
            memcpy(resp->buf + resp->len, evt->data, evt->data_len);
            resp->len += evt->data_len;
            resp->buf[resp->len] = '\0';
        }
    }
    return ESP_OK;
}

/* ── Minimal Protobuf codec for Frame/Header ──────────────────── */

/*
 * Feishu WebSocket uses protobuf-encoded frames:
 *
 * message Header { required string key=1; required string value=2; }
 * message Frame {
 *   required uint64 SeqID=1; required uint64 LogID=2;
 *   required int32 service=3; required int32 method=4;
 *   repeated Header headers=5;
 *   optional string payload_encoding=6; optional string payload_type=7;
 *   optional bytes payload=8; optional string LogIDNew=9;
 * }
 *
 * method: 0=CONTROL, 1=DATA
 * header "type": "event"|"ping"|"pong"
 */

#define PB_VARINT   0
#define PB_LENDELIM 2

#define MAX_HEADERS 16

typedef struct {
    char key[32];
    char value[64];
} pb_header_t;

typedef struct {
    uint64_t seq_id;
    uint64_t log_id;
    int32_t  service;
    int32_t  method;     /* 0=CONTROL, 1=DATA */
    pb_header_t headers[MAX_HEADERS];
    int      header_count;
    uint8_t *payload;
    size_t   payload_len;
} pb_frame_t;

/* Read a varint from buf, advance *pos. Returns the value. */
static uint64_t pb_read_varint(const uint8_t *buf, size_t len, size_t *pos)
{
    uint64_t val = 0;
    int shift = 0;
    while (*pos < len) {
        uint8_t b = buf[(*pos)++];
        val |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    return val;
}

/* Write a varint to buf, return bytes written */
static size_t pb_write_varint(uint8_t *buf, uint64_t val)
{
    size_t n = 0;
    do {
        uint8_t b = val & 0x7F;
        val >>= 7;
        if (val) b |= 0x80;
        buf[n++] = b;
    } while (val);
    return n;
}

/* Write a field tag */
static size_t pb_write_tag(uint8_t *buf, int field, int wire_type)
{
    return pb_write_varint(buf, ((uint64_t)field << 3) | wire_type);
}

/* Parse a Header sub-message from bytes */
static void pb_parse_header(const uint8_t *buf, size_t len, pb_header_t *hdr)
{
    size_t pos = 0;
    memset(hdr, 0, sizeof(*hdr));
    while (pos < len) {
        uint64_t tag = pb_read_varint(buf, len, &pos);
        int field = (int)(tag >> 3);
        int wtype = (int)(tag & 7);
        if (wtype == PB_LENDELIM) {
            size_t slen = (size_t)pb_read_varint(buf, len, &pos);
            if (pos + slen > len) break;
            if (field == 1) { /* key */
                size_t copy = slen < sizeof(hdr->key) - 1 ? slen : sizeof(hdr->key) - 1;
                memcpy(hdr->key, buf + pos, copy);
                hdr->key[copy] = '\0';
            } else if (field == 2) { /* value */
                size_t copy = slen < sizeof(hdr->value) - 1 ? slen : sizeof(hdr->value) - 1;
                memcpy(hdr->value, buf + pos, copy);
                hdr->value[copy] = '\0';
            }
            pos += slen;
        } else if (wtype == PB_VARINT) {
            pb_read_varint(buf, len, &pos);
        } else {
            break;
        }
    }
}

/* Parse a Frame from binary protobuf */
static bool pb_parse_frame(const uint8_t *buf, size_t len, pb_frame_t *frame)
{
    memset(frame, 0, sizeof(*frame));
    size_t pos = 0;
    while (pos < len) {
        uint64_t tag = pb_read_varint(buf, len, &pos);
        int field = (int)(tag >> 3);
        int wtype = (int)(tag & 7);
        if (wtype == PB_VARINT) {
            uint64_t val = pb_read_varint(buf, len, &pos);
            switch (field) {
                case 1: frame->seq_id = val; break;
                case 2: frame->log_id = val; break;
                case 3: frame->service = (int32_t)val; break;
                case 4: frame->method  = (int32_t)val; break;
                default: break;
            }
        } else if (wtype == PB_LENDELIM) {
            size_t slen = (size_t)pb_read_varint(buf, len, &pos);
            if (pos + slen > len) return false;
            if (field == 5 && frame->header_count < MAX_HEADERS) {
                pb_parse_header(buf + pos, slen, &frame->headers[frame->header_count++]);
            } else if (field == 8) {
                frame->payload = (uint8_t *)(buf + pos);
                frame->payload_len = slen;
            }
            /* skip fields 6,7,9 */
            pos += slen;
        } else {
            break;
        }
    }
    return true;
}

/* Encode a ping frame. Returns bytes written to buf. */
static size_t pb_encode_ping(uint8_t *buf, size_t cap, int service_id)
{
    size_t pos = 0;

    /* SeqID = 0 (field 1, varint) */
    pos += pb_write_tag(buf + pos, 1, PB_VARINT);
    pos += pb_write_varint(buf + pos, 0);

    /* LogID = 0 (field 2, varint) */
    pos += pb_write_tag(buf + pos, 2, PB_VARINT);
    pos += pb_write_varint(buf + pos, 0);

    /* service (field 3, varint) */
    pos += pb_write_tag(buf + pos, 3, PB_VARINT);
    pos += pb_write_varint(buf + pos, (uint64_t)service_id);

    /* method = 0 (CONTROL) (field 4, varint) */
    pos += pb_write_tag(buf + pos, 4, PB_VARINT);
    pos += pb_write_varint(buf + pos, 0);

    /* header: {key:"type", value:"ping"} (field 5, lendelim) */
    /* Build sub-message first */
    uint8_t hdr_buf[64];
    size_t hpos = 0;
    hpos += pb_write_tag(hdr_buf + hpos, 1, PB_LENDELIM);
    hpos += pb_write_varint(hdr_buf + hpos, 4); /* "type" length */
    memcpy(hdr_buf + hpos, "type", 4); hpos += 4;
    hpos += pb_write_tag(hdr_buf + hpos, 2, PB_LENDELIM);
    hpos += pb_write_varint(hdr_buf + hpos, 4); /* "ping" length */
    memcpy(hdr_buf + hpos, "ping", 4); hpos += 4;

    pos += pb_write_tag(buf + pos, 5, PB_LENDELIM);
    pos += pb_write_varint(buf + pos, hpos);
    memcpy(buf + pos, hdr_buf, hpos);
    pos += hpos;

    return pos;
}

/* Get header value by key from parsed frame */
static const char *pb_get_header(const pb_frame_t *frame, const char *key)
{
    for (int i = 0; i < frame->header_count; i++) {
        if (strcmp(frame->headers[i].key, key) == 0)
            return frame->headers[i].value;
    }
    return NULL;
}

/* ── Dedup ────────────────────────────────────────────────────── */

static bool dedup_check_and_add(const char *msg_id)
{
    for (int i = 0; i < DEDUP_SIZE; i++) {
        if (strcmp(s_dedup[i], msg_id) == 0) return true; /* already seen */
    }
    strncpy(s_dedup[s_dedup_idx], msg_id, 31);
    s_dedup[s_dedup_idx][31] = '\0';
    s_dedup_idx = (s_dedup_idx + 1) % DEDUP_SIZE;
    return false;
}

/* ── Tenant access token ──────────────────────────────────────── */

static esp_err_t feishu_refresh_token(void)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "app_id", s_app_id);
    cJSON_AddStringToObject(body, "app_secret", s_app_secret);
    char *post_data = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!post_data) return ESP_ERR_NO_MEM;

    http_resp_t resp = { .buf = calloc(1, 4096), .len = 0, .cap = 4096 };
    if (!resp.buf) { free(post_data); return ESP_ERR_NO_MEM; }

    esp_http_client_config_t config = {
        .url = "https://open.feishu.cn/open-apis/auth/v3/tenant_access_token/internal",
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { free(post_data); free(resp.buf); return ESP_FAIL; }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(post_data);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "Token request failed: err=%s status=%d", esp_err_to_name(err), status);
        free(resp.buf);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp.buf);
    free(resp.buf);
    if (!root) return ESP_FAIL;

    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (!code || code->valueint != 0) {
        cJSON *msg = cJSON_GetObjectItem(root, "msg");
        ESP_LOGE(TAG, "Token API error: %s", msg ? msg->valuestring : "unknown");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *token = cJSON_GetObjectItem(root, "tenant_access_token");
    cJSON *expire = cJSON_GetObjectItem(root, "expire");
    if (!token || !cJSON_IsString(token)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    strncpy(s_tenant_token, token->valuestring, sizeof(s_tenant_token) - 1);
    s_token_expire = time(NULL) + (expire ? expire->valueint : 7200) - 300; /* refresh 5 min early */

    ESP_LOGI(TAG, "Tenant token acquired (expires in %ds)", expire ? expire->valueint : 7200);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t feishu_ensure_token(void)
{
    if (s_tenant_token[0] && time(NULL) < s_token_expire) return ESP_OK;
    return feishu_refresh_token();
}

/* ── Get WebSocket endpoint ───────────────────────────────────── */

static char *feishu_get_ws_url(void)
{
    if (feishu_ensure_token() != ESP_OK) return NULL;

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "AppID", s_app_id);
    cJSON_AddStringToObject(body, "AppSecret", s_app_secret);
    char *post_data = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!post_data) return NULL;

    http_resp_t resp = { .buf = calloc(1, 4096), .len = 0, .cap = 4096 };
    if (!resp.buf) { free(post_data); return NULL; }

    esp_http_client_config_t config = {
        .url = "https://open.feishu.cn/callback/ws/endpoint",
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { free(post_data); free(resp.buf); return NULL; }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(post_data);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "WS endpoint request failed: err=%s status=%d", esp_err_to_name(err), status);
        free(resp.buf);
        return NULL;
    }

    cJSON *root = cJSON_Parse(resp.buf);
    free(resp.buf);
    if (!root) return NULL;

    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (!code || code->valueint != 0) {
        cJSON *msg = cJSON_GetObjectItem(root, "msg");
        ESP_LOGE(TAG, "WS endpoint error: %s", msg ? msg->valuestring : "unknown");
        cJSON_Delete(root);
        return NULL;
    }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *url = data ? cJSON_GetObjectItem(data, "URL") : NULL;
    if (!url || !cJSON_IsString(url)) {
        cJSON_Delete(root);
        return NULL;
    }

    /* Extract service_id from URL query param */
    const char *sid = strstr(url->valuestring, "service_id=");
    if (sid) s_service_id = atoi(sid + 11);

    /* Parse ClientConfig if present */
    cJSON *cc = data ? cJSON_GetObjectItem(data, "ClientConfig") : NULL;
    if (cc) {
        cJSON *pi = cJSON_GetObjectItem(cc, "PingInterval");
        if (pi && cJSON_IsNumber(pi)) {
            ESP_LOGI(TAG, "PingInterval: %d", pi->valueint);
        }
    }

    char *ws_url = strdup(url->valuestring);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "WebSocket URL acquired");
    return ws_url;
}

/* ── Handle incoming event ────────────────────────────────────── */

static void handle_feishu_event(const uint8_t *payload, size_t payload_len)
{
    /* payload is the Feishu event JSON, e.g.:
     * {"schema":"2.0","header":{"event_type":"im.message.receive_v1",...},
     *  "event":{"message":{"message_type":"text","content":"{\"text\":\"hello\"}",
     *           "chat_id":"oc_xxx","message_id":"om_xxx"},
     *           "sender":{"sender_id":{"open_id":"ou_xxx"},"sender_type":"user"}}}
     */
    cJSON *root = cJSON_ParseWithLength((const char *)payload, payload_len);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse event JSON");
        return;
    }

    /* Check event type */
    cJSON *header = cJSON_GetObjectItem(root, "header");
    cJSON *event_type = header ? cJSON_GetObjectItem(header, "event_type") : NULL;
    if (!event_type || !cJSON_IsString(event_type) ||
        strcmp(event_type->valuestring, "im.message.receive_v1") != 0) {
        cJSON_Delete(root);
        return;
    }

    cJSON *event = cJSON_GetObjectItem(root, "event");
    if (!event) { cJSON_Delete(root); return; }

    /* Get sender info */
    cJSON *sender = cJSON_GetObjectItem(event, "sender");
    cJSON *sender_type = sender ? cJSON_GetObjectItem(sender, "sender_type") : NULL;
    if (sender_type && cJSON_IsString(sender_type) &&
        strcmp(sender_type->valuestring, "bot") == 0) {
        cJSON_Delete(root);
        return; /* skip bot messages */
    }

    cJSON *sender_id_obj = sender ? cJSON_GetObjectItem(sender, "sender_id") : NULL;
    cJSON *open_id = sender_id_obj ? cJSON_GetObjectItem(sender_id_obj, "open_id") : NULL;

    /* Get message info */
    cJSON *message = cJSON_GetObjectItem(event, "message");
    if (!message) { cJSON_Delete(root); return; }

    cJSON *msg_id = cJSON_GetObjectItem(message, "message_id");
    cJSON *msg_type = cJSON_GetObjectItem(message, "message_type");
    cJSON *chat_id = cJSON_GetObjectItem(message, "chat_id");
    cJSON *chat_type = cJSON_GetObjectItem(message, "chat_type");
    cJSON *content_str = cJSON_GetObjectItem(message, "content");

    if (!msg_id || !cJSON_IsString(msg_id)) { cJSON_Delete(root); return; }

    /* Dedup */
    if (dedup_check_and_add(msg_id->valuestring)) {
        cJSON_Delete(root);
        return;
    }

    /* Parse text content */
    char text[1024] = {0};
    if (msg_type && cJSON_IsString(msg_type) &&
        strcmp(msg_type->valuestring, "text") == 0 &&
        content_str && cJSON_IsString(content_str)) {
        cJSON *content = cJSON_Parse(content_str->valuestring);
        if (content) {
            cJSON *t = cJSON_GetObjectItem(content, "text");
            if (t && cJSON_IsString(t)) {
                strncpy(text, t->valuestring, sizeof(text) - 1);
            }
            cJSON_Delete(content);
        }
    } else {
        /* Non-text message */
        snprintf(text, sizeof(text), "[%s]",
                 (msg_type && cJSON_IsString(msg_type)) ? msg_type->valuestring : "unknown");
    }

    if (text[0] == '\0') { cJSON_Delete(root); return; }

    /* Determine reply target: group → chat_id, p2p → sender open_id */
    const char *reply_to = NULL;
    if (chat_type && cJSON_IsString(chat_type) &&
        strcmp(chat_type->valuestring, "group") == 0 &&
        chat_id && cJSON_IsString(chat_id)) {
        reply_to = chat_id->valuestring;
    } else if (open_id && cJSON_IsString(open_id)) {
        reply_to = open_id->valuestring;
    }

    if (!reply_to) { cJSON_Delete(root); return; }

    ESP_LOGI(TAG, "Message from %s: %.40s...", reply_to, text);

    /* Push to inbound bus */
    mimi_msg_t msg = {0};
    strncpy(msg.channel, MIMI_CHAN_FEISHU, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, reply_to, sizeof(msg.chat_id) - 1);
    msg.content = strdup(text);
    if (msg.content) {
        message_bus_push_inbound(&msg);
    }

    cJSON_Delete(root);
}

/* ── WebSocket event handler ──────────────────────────────────── */

static void feishu_ws_event_handler(void *arg, esp_event_base_t base,
                                     int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected to Feishu");
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected from Feishu");
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x02 && data->data_len > 0) {
            /* Binary frame = protobuf Frame */
            pb_frame_t frame;
            if (!pb_parse_frame((const uint8_t *)data->data_ptr, data->data_len, &frame))
                break;

            const char *type = pb_get_header(&frame, "type");
            if (!type) break;

            if (frame.method == 0) {
                /* CONTROL frame */
                if (strcmp(type, "pong") == 0) {
                    ESP_LOGD(TAG, "Received pong");
                }
            } else if (frame.method == 1) {
                /* DATA frame */
                if (strcmp(type, "event") == 0 && frame.payload && frame.payload_len > 0) {
                    handle_feishu_event(frame.payload, frame.payload_len);

                    /* Send ack response: rewrite payload in original frame */
                    const char *resp_json = "{\"code\":200}";
                    uint8_t resp_buf[512];
                    size_t rpos = 0;

                    /* Rebuild frame with response payload */
                    rpos += pb_write_tag(resp_buf + rpos, 1, PB_VARINT);
                    rpos += pb_write_varint(resp_buf + rpos, frame.seq_id);
                    rpos += pb_write_tag(resp_buf + rpos, 2, PB_VARINT);
                    rpos += pb_write_varint(resp_buf + rpos, frame.log_id);
                    rpos += pb_write_tag(resp_buf + rpos, 3, PB_VARINT);
                    rpos += pb_write_varint(resp_buf + rpos, (uint64_t)frame.service);
                    rpos += pb_write_tag(resp_buf + rpos, 4, PB_VARINT);
                    rpos += pb_write_varint(resp_buf + rpos, (uint64_t)frame.method);

                    /* Copy original headers + add biz_rt */
                    for (int i = 0; i < frame.header_count; i++) {
                        uint8_t hdr_buf[128];
                        size_t hlen = 0;
                        hlen += pb_write_tag(hdr_buf + hlen, 1, PB_LENDELIM);
                        size_t klen = strlen(frame.headers[i].key);
                        hlen += pb_write_varint(hdr_buf + hlen, klen);
                        memcpy(hdr_buf + hlen, frame.headers[i].key, klen); hlen += klen;
                        hlen += pb_write_tag(hdr_buf + hlen, 2, PB_LENDELIM);
                        size_t vlen = strlen(frame.headers[i].value);
                        hlen += pb_write_varint(hdr_buf + hlen, vlen);
                        memcpy(hdr_buf + hlen, frame.headers[i].value, vlen); hlen += vlen;

                        rpos += pb_write_tag(resp_buf + rpos, 5, PB_LENDELIM);
                        rpos += pb_write_varint(resp_buf + rpos, hlen);
                        memcpy(resp_buf + rpos, hdr_buf, hlen); rpos += hlen;
                    }

                    /* payload = response JSON (field 8) */
                    size_t plen = strlen(resp_json);
                    rpos += pb_write_tag(resp_buf + rpos, 8, PB_LENDELIM);
                    rpos += pb_write_varint(resp_buf + rpos, plen);
                    memcpy(resp_buf + rpos, resp_json, plen); rpos += plen;

                    if (s_ws_client) {
                        esp_websocket_client_send_bin(s_ws_client, (const char *)resp_buf, rpos, pdMS_TO_TICKS(5000));
                    }
                }
            }
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        break;

    default:
        break;
    }
}

/* ── Feishu task ──────────────────────────────────────────────── */

static void feishu_task(void *arg)
{
    ESP_LOGI(TAG, "Feishu task started");

    while (1) {
        if (s_app_id[0] == '\0' || s_app_secret[0] == '\0') {
            ESP_LOGW(TAG, "No Feishu credentials, waiting...");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        /* Get WebSocket URL */
        char *ws_url = feishu_get_ws_url();
        if (!ws_url) {
            ESP_LOGE(TAG, "Failed to get WS URL, retrying in 30s...");
            vTaskDelay(pdMS_TO_TICKS(30000));
            continue;
        }

        ESP_LOGI(TAG, "Connecting to Feishu WebSocket...");

        esp_websocket_client_config_t ws_config = {
            .uri = ws_url,
            .buffer_size = 4096,
            .task_stack = 6 * 1024,
            .pingpong_timeout_sec = 0, /* we handle ping ourselves */
            .crt_bundle_attach = esp_crt_bundle_attach,
        };

        s_ws_client = esp_websocket_client_init(&ws_config);
        if (!s_ws_client) {
            ESP_LOGE(TAG, "Failed to init WS client");
            free(ws_url);
            vTaskDelay(pdMS_TO_TICKS(30000));
            continue;
        }

        esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY,
                                       feishu_ws_event_handler, NULL);
        esp_err_t err = esp_websocket_client_start(s_ws_client);
        free(ws_url);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start WS client");
            esp_websocket_client_destroy(s_ws_client);
            s_ws_client = NULL;
            vTaskDelay(pdMS_TO_TICKS(30000));
            continue;
        }

        /* Ping loop — send ping every 120s, check connection */
        while (esp_websocket_client_is_connected(s_ws_client)) {
            /* Send ping */
            uint8_t ping_buf[64];
            size_t ping_len = pb_encode_ping(ping_buf, sizeof(ping_buf), s_service_id);
            esp_websocket_client_send_bin(s_ws_client, (const char *)ping_buf, ping_len, pdMS_TO_TICKS(5000));
            ESP_LOGD(TAG, "Ping sent");

            /* Wait 120s, checking connection periodically */
            for (int i = 0; i < 120 && esp_websocket_client_is_connected(s_ws_client); i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }

        /* Disconnected — cleanup and reconnect */
        ESP_LOGW(TAG, "Feishu disconnected, reconnecting in 10s...");
        esp_websocket_client_stop(s_ws_client);
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

/* ── Send message via REST API ────────────────────────────────── */

esp_err_t feishu_send_message(const char *chat_id, const char *text)
{
    if (feishu_ensure_token() != ESP_OK) {
        ESP_LOGE(TAG, "Cannot send: no valid token");
        return ESP_ERR_INVALID_STATE;
    }

    /* Determine receive_id_type: oc_* = chat_id, ou_* = open_id */
    const char *id_type = "open_id";
    if (strncmp(chat_id, "oc_", 3) == 0) {
        id_type = "chat_id";
    }

    /* Build URL with query param */
    char url[256];
    snprintf(url, sizeof(url),
             "https://open.feishu.cn/open-apis/im/v1/messages?receive_id_type=%s", id_type);

    /* Build message body */
    cJSON *content = cJSON_CreateObject();
    cJSON_AddStringToObject(content, "text", text);
    char *content_str = cJSON_PrintUnformatted(content);
    cJSON_Delete(content);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "receive_id", chat_id);
    cJSON_AddStringToObject(body, "msg_type", "text");
    cJSON_AddStringToObject(body, "content", content_str);
    free(content_str);

    char *post_data = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!post_data) return ESP_ERR_NO_MEM;

    http_resp_t resp = { .buf = calloc(1, 2048), .len = 0, .cap = 2048 };
    if (!resp.buf) { free(post_data); return ESP_ERR_NO_MEM; }

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { free(post_data); free(resp.buf); return ESP_FAIL; }

    char auth[300];
    snprintf(auth, sizeof(auth), "Bearer %s", s_tenant_token);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(post_data);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "Send message failed: err=%s status=%d resp=%s",
                 esp_err_to_name(err), status, resp.buf);
        free(resp.buf);
        return ESP_FAIL;
    }

    /* Check API response */
    cJSON *root = cJSON_Parse(resp.buf);
    free(resp.buf);
    if (root) {
        cJSON *code = cJSON_GetObjectItem(root, "code");
        if (code && code->valueint != 0) {
            cJSON *msg = cJSON_GetObjectItem(root, "msg");
            ESP_LOGE(TAG, "Send API error: code=%d msg=%s",
                     code->valueint, msg ? msg->valuestring : "");
        } else {
            ESP_LOGD(TAG, "Message sent to %s", chat_id);
        }
        cJSON_Delete(root);
    }

    return ESP_OK;
}

/* ── Public API ───────────────────────────────────────────────── */

esp_err_t feishu_bot_init(void)
{
    /* NVS overrides take highest priority */
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_FEISHU, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[64] = {0};
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_FEISHU_APP_ID, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_app_id, tmp, sizeof(s_app_id) - 1);
        }
        len = sizeof(tmp);
        memset(tmp, 0, sizeof(tmp));
        if (nvs_get_str(nvs, MIMI_NVS_KEY_FEISHU_SECRET, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_app_secret, tmp, sizeof(s_app_secret) - 1);
        }
        nvs_close(nvs);
    }

    if (s_app_id[0] && s_app_secret[0]) {
        ESP_LOGI(TAG, "Feishu credentials loaded (app_id=%.8s...)", s_app_id);
    } else {
        ESP_LOGW(TAG, "No Feishu credentials. Use CLI: set_feishu_id + set_feishu_secret");
    }

    return ESP_OK;
}

esp_err_t feishu_bot_start(void)
{
    if (s_app_id[0] == '\0' || s_app_secret[0] == '\0') {
        ESP_LOGW(TAG, "Feishu not started: no credentials configured");
        return ESP_OK; /* not an error, just skip */
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        feishu_task, "feishu",
        MIMI_FEISHU_STACK, NULL,
        MIMI_FEISHU_PRIO, NULL, MIMI_FEISHU_CORE);

    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t feishu_set_app_id(const char *app_id)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_FEISHU, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_FEISHU_APP_ID, app_id));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_app_id, app_id, sizeof(s_app_id) - 1);
    ESP_LOGI(TAG, "Feishu App ID saved");
    return ESP_OK;
}

esp_err_t feishu_set_app_secret(const char *app_secret)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_FEISHU, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_FEISHU_SECRET, app_secret));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_app_secret, app_secret, sizeof(s_app_secret) - 1);
    ESP_LOGI(TAG, "Feishu App Secret saved");
    return ESP_OK;
}
