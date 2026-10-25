#include "portal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "adsb.h"
#include "cJSON.h"
#include "config_store.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "map_store.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "mdns.h"
#include "radar_ui.h"
#include "wifi_mgr.h"

static const char *TAG = "portal";

extern const char index_html_start[] asm("_binary_index_html_start");

#define BODY_MAX 1536
#define OTA_CHUNK 2048
#define PASS_MIN 6
#define PASS_MAX 64
#define AP_IP "192.168.4.1"

static bool s_captive;

/* ------------------------------------------------------------------------- */
/* Captive DNS: answers every A query with the AP address                     */
/* ------------------------------------------------------------------------- */

static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t buf[300];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, 256, 0, (struct sockaddr *)&from, &fl);
        if (n < 17 || (buf[2] & 0x80) || ((buf[4] << 8) | buf[5]) != 1) {
            continue;
        }
        int i = 12;
        while (i < n && buf[i] != 0) {
            if (buf[i] & 0xC0) { /* compression pointer in a question: not valid */
                i = n;
                break;
            }
            i += buf[i] + 1;
        }
        if (i + 5 > n) {
            continue;
        }
        int qend = i + 5;
        uint16_t qtype = (uint16_t)((buf[i + 1] << 8) | buf[i + 2]);

        buf[2] = 0x84 | (buf[2] & 0x01); /* QR, AA, keep RD */
        buf[3] = 0x80;                   /* RA */
        buf[6] = 0;
        buf[7] = (qtype == 1) ? 1 : 0;
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        int out = qend;
        if (qtype == 1) {
            static const uint8_t ans[] = {0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x04,
                                          192,  168,  4,    1};
            memcpy(buf + out, ans, sizeof(ans));
            out += sizeof(ans);
        }
        sendto(sock, buf, out, 0, (struct sockaddr *)&from, fl);
    }
}

/* ------------------------------------------------------------------------- */
/* HTTP helpers                                                              */
/* ------------------------------------------------------------------------- */

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t send_json(httpd_req_t *req, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
    free(s);
    return err;
}

static esp_err_t send_result(httpd_req_t *req, bool ok, const char *error, bool reboot)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", ok);
    if (error) {
        cJSON_AddStringToObject(j, "error", error);
    }
    cJSON_AddBoolToObject(j, "reboot", reboot);
    return send_json(req, j);
}

/* ------------------------------------------------------------------------- */
/* Web password (HTTP Basic auth; any user name)                             */
/* ------------------------------------------------------------------------- */

static void pass_hash(const uint8_t salt[16], const char *pw, uint8_t out[32])
{
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, salt, 16);
    mbedtls_sha256_update(&ctx, (const unsigned char *)pw, strlen(pw));
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

static bool authorized(httpd_req_t *req)
{
    if (s_captive) {
        return true; /* setup access point: protected by its own WPA2 password */
    }
    radar_config_t c;
    cfg_get(&c);
    if (!c.portal_pass) {
        return true;
    }
    char hdr[200];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        return false; /* first request: the browser asks for the password after the 401 */
    }
    bool ok = false;
    unsigned char dec[160];
    size_t n = 0;
    if (strncasecmp(hdr, "Basic ", 6) == 0 &&
        mbedtls_base64_decode(dec, sizeof(dec) - 1, &n, (const unsigned char *)hdr + 6, strlen(hdr + 6)) == 0) {
        dec[n] = 0;
        char *colon = strchr((char *)dec, ':');
        if (colon) {
            uint8_t h[32];
            pass_hash(c.portal_salt, colon + 1, h);
            uint8_t diff = 0;
            for (int i = 0; i < 32; i++) {
                diff |= h[i] ^ c.portal_hash[i];
            }
            ok = diff == 0;
        }
        memset(dec, 0, sizeof(dec));
    }
    if (!ok) {
        vTaskDelay(pdMS_TO_TICKS(1000)); /* slow down guessing */
    }
    return ok;
}

static esp_err_t send_unauthorized(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Radar\", charset=\"UTF-8\"");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "Password required", HTTPD_RESP_USE_STRLEN);
}

#define REQUIRE_AUTH(req)                \
    do {                                 \
        if (!authorized(req)) {          \
            return send_unauthorized(req); \
        }                                \
    } while (0)

/* ------------------------------------------------------------------------- */
/* Handlers                                                                  */
/* ------------------------------------------------------------------------- */

static esp_err_t index_get(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
                       "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self' https://cdn.jsdelivr.net");
    return httpd_resp_send(req, index_html_start, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t config_get(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    radar_config_t c;
    cfg_get(&c);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "ssid", c.wifi_ssid);
    cJSON_AddBoolToObject(j, "has_pass", c.wifi_pass[0] != 0);
    cJSON_AddBoolToObject(j, "has_portal_pass", c.portal_pass != 0);
    cJSON_AddNumberToObject(j, "led", c.led);
    cJSON_AddNumberToObject(j, "map", c.map);
    cJSON_AddNumberToObject(j, "map_layers", c.map_layers);
    cJSON_AddNumberToObject(j, "map_brightness", c.map_brightness);
    cJSON_AddNumberToObject(j, "map_color", c.map_color);
    cJSON_AddNumberToObject(j, "lat", c.lat);
    cJSON_AddNumberToObject(j, "lon", c.lon);
    cJSON_AddNumberToObject(j, "source", c.source);
    cJSON_AddNumberToObject(j, "units", c.units);
    cJSON_AddNumberToObject(j, "zoom", c.default_zoom);
    cJSON_AddNumberToObject(j, "routes", c.lookup_routes);
    cJSON_AddNumberToObject(j, "sweep", c.sweep);
    cJSON_AddNumberToObject(j, "rotation", c.rotation);
    cJSON_AddNumberToObject(j, "label", c.label_fields);
    cJSON_AddNumberToObject(j, "show_count", c.show_count);
    cJSON_AddNumberToObject(j, "refresh", c.refresh_s);
    cJSON_AddStringToObject(j, "base_url", c.base_url);
    /* the portal has no authentication: never hand out the key itself */
    char hint[24] = "";
    size_t kl = strlen(c.api_key);
    if (kl > 14) {
        snprintf(hint, sizeof(hint), "%.7s...%s", c.api_key, c.api_key + kl - 4);
    } else if (kl) {
        strlcpy(hint, "(set)", sizeof(hint));
    }
    cJSON_AddStringToObject(j, "api_key_hint", hint);
    cJSON_AddNumberToObject(j, "trails", c.trails);
    cJSON_AddNumberToObject(j, "alt_colors", c.alt_colors);
    cJSON_AddNumberToObject(j, "hide_ground", c.hide_ground);
    cJSON_AddNumberToObject(j, "leader", c.leader_s);
    cJSON_AddNumberToObject(j, "max_alt", c.max_alt_ft);
    cJSON_AddNumberToObject(j, "brightness", c.brightness);
    cJSON_AddNumberToObject(j, "dim_level", c.dim_level);
    cJSON_AddNumberToObject(j, "dim_after", c.dim_after_min);
    cJSON_AddNumberToObject(j, "night", c.night);
    cJSON_AddNumberToObject(j, "night_start", c.night_start_h);
    cJSON_AddNumberToObject(j, "night_end", c.night_end_h);
    cJSON_AddStringToObject(j, "tz", c.tz);
    cJSON_AddNumberToObject(j, "alert", c.alert);
    cJSON_AddNumberToObject(j, "alert_dist", c.alert_dist_nm10 / 10.0);
    cJSON_AddNumberToObject(j, "alert_alt", c.alert_alt_ft);
    cJSON *refs = cJSON_AddArrayToObject(j, "ref");
    for (int i = 0; i < CFG_REF_POINTS; i++) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "name", c.ref[i].name);
        cJSON_AddNumberToObject(r, "lat", c.ref[i].lat);
        cJSON_AddNumberToObject(r, "lon", c.ref[i].lon);
        cJSON_AddItemToArray(refs, r);
    }
    return send_json(req, j);
}

static bool json_num(const cJSON *o, const char *k, double lo, double hi, double *out)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!i) {
        return true; /* absent: keep the current value */
    }
    if (!cJSON_IsNumber(i) || !(i->valuedouble >= lo && i->valuedouble <= hi)) {
        return false;
    }
    *out = i->valuedouble;
    return true;
}

/* Printable ASCII only (what the display fonts can show and the TZ parser expects). */
static bool printable(const char *t)
{
    for (; *t; t++) {
        if ((unsigned char)*t < 0x20 || (unsigned char)*t >= 0x7f) {
            return false;
        }
    }
    return true;
}

/* Optional numeric field into a config member of any arithmetic type; absent keeps the value. */
#define NUM_FIELD(key, field, lo, hi, msg)              \
    do {                                                \
        double v_ = c.field;                            \
        if (!err && !json_num(j, key, lo, hi, &v_)) {   \
            err = msg;                                  \
        }                                               \
        c.field = (__typeof__(c.field))v_;              \
    } while (0)

static const char *parse_refs(const cJSON *arr, radar_config_t *c)
{
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) > CFG_REF_POINTS) {
        return "Invalid reference points";
    }
    ref_point_t refs[CFG_REF_POINTS] = {0};
    int i = 0;
    const cJSON *r;
    cJSON_ArrayForEach(r, arr) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(r, "name");
        double lat = 0, lon = 0;
        if (!cJSON_IsString(name) || strlen(name->valuestring) >= sizeof(refs[i].name) ||
            !printable(name->valuestring)) {
            return "Reference point names: up to 7 plain characters";
        }
        if (name->valuestring[0] && (!json_num(r, "lat", -90, 90, &lat) || !json_num(r, "lon", -180, 180, &lon))) {
            return "Invalid reference point coordinates";
        }
        strlcpy(refs[i].name, name->valuestring, sizeof(refs[i].name));
        refs[i].lat = (float)lat;
        refs[i].lon = (float)lon;
        i++;
    }
    memcpy(c->ref, refs, sizeof(refs));
    return NULL;
}

static bool valid_url(const char *u)
{
    size_t l = strlen(u);
    if (l == 0) {
        return true;
    }
    if (l >= sizeof(((radar_config_t *)0)->base_url)) {
        return false;
    }
    if (strncmp(u, "http://", 7) != 0 && strncmp(u, "https://", 8) != 0) {
        return false;
    }
    for (size_t i = 0; i < l; i++) {
        if ((unsigned char)u[i] <= 0x20 || (unsigned char)u[i] >= 0x7f) {
            return false;
        }
    }
    return true;
}

static esp_err_t config_post(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    /* Require a JSON content type: cross-site <form>/no-cors posts cannot set it without a CORS
     * preflight, which this server never answers. */
    char ct[40] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct)) != ESP_OK ||
        strncasecmp(ct, "application/json", 16) != 0) {
        return send_result(req, false, "Content-Type must be application/json", false);
    }
    if (req->content_len == 0 || req->content_len > BODY_MAX) {
        return send_result(req, false, "Bad request size", false);
    }
    char *body = malloc(req->content_len + 1);
    if (!body) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            free(body);
            return ESP_FAIL;
        }
        got += r;
    }
    body[got] = 0;
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        return send_result(req, false, "Invalid JSON", false);
    }

    radar_config_t c;
    cfg_get(&c);
    const char *err = NULL;
    bool wifi_changed = false;

    NUM_FIELD("lat", lat, -90, 90, "Latitude must be between -90 and 90");
    NUM_FIELD("lon", lon, -180, 180, "Longitude must be between -180 and 180");
    NUM_FIELD("source", source, 0, 1, "Invalid source");
    NUM_FIELD("units", units, 0, 2, "Invalid units");
    NUM_FIELD("zoom", default_zoom, 0, CFG_ZOOM_LEVELS - 1, "Invalid zoom");
    NUM_FIELD("routes", lookup_routes, 0, 1, "Invalid route flag");
    NUM_FIELD("sweep", sweep, 0, 1, "Invalid sweep flag");
    NUM_FIELD("rotation", rotation, 0, 3, "Invalid rotation");
    NUM_FIELD("label", label_fields, 0, LABEL_CALLSIGN | LABEL_TYPE | LABEL_ALT, "Invalid label fields");
    NUM_FIELD("show_count", show_count, 0, 1, "Invalid count flag");
    NUM_FIELD("led", led, 0, 2, "Invalid LED mode");
    NUM_FIELD("map", map, 0, 1, "Invalid map flag");
    NUM_FIELD("map_layers", map_layers, 0, MAP_L_ALL, "Invalid map layers");
    NUM_FIELD("map_brightness", map_brightness, 10, 100, "Map brightness must be 10-100 %");
    NUM_FIELD("map_color", map_color, 0, MAP_COLORS - 1, "Invalid map colour");

    /* portal_pass: a string sets it, "" keeps it, null removes it */
    const cJSON *pp = cJSON_GetObjectItemCaseSensitive(j, "portal_pass");
    if (!err && cJSON_IsNull(pp)) {
        c.portal_pass = 0;
        memset(c.portal_salt, 0, sizeof(c.portal_salt));
        memset(c.portal_hash, 0, sizeof(c.portal_hash));
    } else if (!err && pp) {
        size_t pl = cJSON_IsString(pp) ? strlen(pp->valuestring) : 0;
        if (!cJSON_IsString(pp) || (pl && (pl < PASS_MIN || pl > PASS_MAX)) || !printable(pp->valuestring)) {
            err = "Web password: 6-64 plain characters";
        } else if (pl) {
            esp_fill_random(c.portal_salt, sizeof(c.portal_salt));
            pass_hash(c.portal_salt, pp->valuestring, c.portal_hash);
            c.portal_pass = 1;
        }
    }
    NUM_FIELD("refresh", refresh_s, 1, 60, "Refresh must be 1-60 seconds");
    NUM_FIELD("trails", trails, 0, 1, "Invalid trails flag");
    NUM_FIELD("alt_colors", alt_colors, 0, 1, "Invalid altitude colour flag");
    NUM_FIELD("hide_ground", hide_ground, 0, 1, "Invalid ground filter flag");
    NUM_FIELD("leader", leader_s, 0, 240, "Track line must be 0-240 seconds");
    NUM_FIELD("max_alt", max_alt_ft, 0, 60000, "Altitude limit must be 0-60000 ft");
    NUM_FIELD("brightness", brightness, 5, 100, "Brightness must be 5-100 %");
    NUM_FIELD("dim_level", dim_level, 0, 100, "Dimmed brightness must be 0-100 %");
    NUM_FIELD("dim_after", dim_after_min, 0, 240, "Dim delay must be 0-240 minutes");
    NUM_FIELD("night", night, 0, 1, "Invalid night flag");
    NUM_FIELD("night_start", night_start_h, 0, 23, "Night hours must be 0-23");
    NUM_FIELD("night_end", night_end_h, 0, 23, "Night hours must be 0-23");
    NUM_FIELD("alert", alert, 0, 1, "Invalid alert flag");
    NUM_FIELD("alert_alt", alert_alt_ft, 100, 60000, "Alert altitude must be 100-60000 ft");
    double alert_nm = c.alert_dist_nm10 / 10.0;
    if (!err && !json_num(j, "alert_dist", 0.1, 50, &alert_nm)) {
        err = "Alert distance must be 0.1-50 nm";
    }
    c.alert_dist_nm10 = (uint16_t)lround(alert_nm * 10.0);

    const cJSON *tz = cJSON_GetObjectItemCaseSensitive(j, "tz");
    if (!err && tz) {
        if (!cJSON_IsString(tz) || strlen(tz->valuestring) >= sizeof(c.tz) || !printable(tz->valuestring)) {
            err = "Invalid time zone";
        } else {
            strlcpy(c.tz, tz->valuestring, sizeof(c.tz));
        }
    }
    /* api_key: a string sets it, "" keeps it, null removes it */
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(j, "api_key");
    if (!err && cJSON_IsNull(key)) {
        c.api_key[0] = 0;
    } else if (!err && key) {
        if (!cJSON_IsString(key) || strlen(key->valuestring) >= sizeof(c.api_key) || !printable(key->valuestring) ||
            strchr(key->valuestring, ' ')) {
            err = "API key: up to 75 characters, no spaces";
        } else if (key->valuestring[0]) {
            strlcpy(c.api_key, key->valuestring, sizeof(c.api_key));
        }
    }
    const cJSON *refs = cJSON_GetObjectItemCaseSensitive(j, "ref");
    if (!err && refs) {
        err = parse_refs(refs, &c);
    }

    const cJSON *url = cJSON_GetObjectItemCaseSensitive(j, "base_url");
    if (!err && url) {
        if (!cJSON_IsString(url) || !valid_url(url->valuestring)) {
            err = "tar1090 URL must start with http:// or https://";
        } else if (url->valuestring[0]) {
            strlcpy(c.base_url, url->valuestring, sizeof(c.base_url));
        }
    }
    if (!err && c.source == SRC_TAR1090 && c.base_url[0] == 0) {
        err = "tar1090 base URL is required for this source";
    }

    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(j, "ssid");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(j, "pass");
    if (!err && ssid) {
        if (!cJSON_IsString(ssid) || strlen(ssid->valuestring) > 32) {
            err = "SSID must be at most 32 characters";
        } else if (ssid->valuestring[0] && strcmp(ssid->valuestring, c.wifi_ssid) != 0) {
            strlcpy(c.wifi_ssid, ssid->valuestring, sizeof(c.wifi_ssid));
            c.wifi_pass[0] = 0; /* a new network never inherits the old password */
            wifi_changed = true;
        }
    }
    if (!err && pass && cJSON_IsString(pass) && pass->valuestring[0]) {
        size_t pl = strlen(pass->valuestring);
        if (pl < 8 || pl > 64) {
            err = "WiFi password must be 8-64 characters";
        } else {
            strlcpy(c.wifi_pass, pass->valuestring, sizeof(c.wifi_pass));
            wifi_changed = true;
        }
    }
    cJSON_Delete(j);

    if (err) {
        return send_result(req, false, err, false);
    }
    if (cfg_set(&c) != ESP_OK) {
        return send_result(req, false, "Could not store settings", false);
    }
    ESP_LOGI(TAG, "config saved (wifi %s)", wifi_changed ? "changed" : "unchanged");

    if (wifi_changed) {
        xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    } else {
        adsb_kick();
    }
    return send_result(req, true, NULL, wifi_changed);
}

/* Firmware upload: raw image as the request body, written straight into the inactive OTA slot. */
static esp_err_t ota_post(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    /* same cross-site protection as the config endpoint: this type needs a CORS preflight */
    char ct[40] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct)) != ESP_OK ||
        strncasecmp(ct, "application/octet-stream", 24) != 0) {
        return send_result(req, false, "Content-Type must be application/octet-stream", false);
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        return send_result(req, false, "No OTA partition: flash the OTA partition table over USB once", false);
    }
    if (req->content_len == 0 || req->content_len > part->size) {
        return send_result(req, false, "Bad firmware size", false);
    }
    char *buf = malloc(OTA_CHUNK);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    if (!s_captive) {
        radar_ui_show_message("Firmware update\nreceiving...");
    }
    ESP_LOGI(TAG, "OTA: %u bytes to %s", (unsigned)req->content_len, part->label);

    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
    size_t got = 0;
    int timeouts = 0;
    while (err == ESP_OK && got < req->content_len) {
        size_t want = req->content_len - got;
        int r = httpd_req_recv(req, buf, want < OTA_CHUNK ? want : OTA_CHUNK);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 4) {
            continue;
        }
        if (r <= 0) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        timeouts = 0;
        err = esp_ota_write(h, buf, r);
        got += r;
    }
    free(buf);
    if (err == ESP_OK) {
        err = esp_ota_end(h); /* verifies the image (header, chip, checksum) */
    } else if (h) {
        esp_ota_abort(h);
    }
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(part);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        if (!s_captive) {
            radar_ui_show_message(NULL);
        }
        return send_result(req, false,
                           err == ESP_ERR_OTA_VALIDATE_FAILED ? "Not a valid firmware image for this device"
                                                              : "Update failed",
                           false);
    }
    ESP_LOGI(TAG, "OTA done, restarting");
    if (!s_captive) {
        radar_ui_show_message("Firmware updated\nrestarting...");
    }
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return send_result(req, true, NULL, true);
}

static esp_err_t map_get(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "capacity", (double)map_store_capacity());
    const map_hdr_t *h = map_store_lock();
    cJSON_AddBoolToObject(j, "present", h != NULL);
    if (h) {
        cJSON_AddNumberToObject(j, "lat", h->lat);
        cJSON_AddNumberToObject(j, "lon", h->lon);
        cJSON_AddNumberToObject(j, "lines", h->polylines);
        cJSON_AddNumberToObject(j, "points", h->points);
        cJSON_AddNumberToObject(j, "bytes", (double)(h->payload_len + sizeof(*h)));
        map_store_unlock();
    }
    return send_json(req, j);
}

/* Map data built by the settings page: raw file as the request body. */
static esp_err_t map_post(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char ct[40] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct)) != ESP_OK ||
        strncasecmp(ct, "application/octet-stream", 24) != 0) {
        return send_result(req, false, "Content-Type must be application/octet-stream", false);
    }
    if (map_store_capacity() == 0) {
        return send_result(req, false, "No map partition: flash the new partition table over USB once", false);
    }
    if (req->content_len <= sizeof(map_hdr_t) || req->content_len > map_store_capacity()) {
        return send_result(req, false, "Map data too large", false);
    }
    char *buf = malloc(OTA_CHUNK);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    esp_err_t err = map_store_begin(req->content_len);
    bool begun = err == ESP_OK;
    size_t got = 0;
    int timeouts = 0;
    while (err == ESP_OK && got < req->content_len) {
        size_t want = req->content_len - got;
        int r = httpd_req_recv(req, buf, want < OTA_CHUNK ? want : OTA_CHUNK);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 4) {
            continue;
        }
        if (r <= 0) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        timeouts = 0;
        err = map_store_write(buf, r);
        got += r;
    }
    free(buf);
    if (begun) {
        if (err == ESP_OK) {
            err = map_store_end();
        } else {
            map_store_abort();
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "map upload failed: %s", esp_err_to_name(err));
        return send_result(req, false, "Map upload failed or data invalid", false);
    }
    ESP_LOGI(TAG, "map stored (%u bytes)", (unsigned)req->content_len);
    return send_result(req, true, NULL, false);
}

static esp_err_t map_delete(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    esp_err_t err = map_store_clear();
    return send_result(req, err == ESP_OK, err == ESP_OK ? NULL : "Could not remove the map", false);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char *s = wifi_mgr_scan_json();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, s ? s : "[]", HTTPD_RESP_USE_STRLEN);
    free(s);
    return err;
}

static esp_err_t status_get(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    adsb_state_t st;
    int count, total, retry_s;
    adsb_get_stats(&st, &count, &total, &retry_s);
    char ip[16];
    wifi_mgr_get_ip(ip, sizeof(ip));

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "ip", ip);
    cJSON_AddStringToObject(j, "state", adsb_state_str(st));
    if (st == ADSB_ST_RATE_LIMITED) {
        cJSON_AddNumberToObject(j, "retry_in", retry_s);
    }
    cJSON_AddNumberToObject(j, "count", count);
    cJSON_AddNumberToObject(j, "total", total);
    cJSON_AddNumberToObject(j, "heap", (double)esp_get_free_heap_size());
    cJSON_AddStringToObject(j, "version", esp_app_get_description()->version);
    const esp_partition_t *run = esp_ota_get_running_partition();
    cJSON_AddStringToObject(j, "partition", run ? run->label : "");
    return send_json(req, j);
}

static esp_err_t catch_all(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", s_captive ? "http://" AP_IP "/" : "/");
    return httpd_resp_send(req, NULL, 0);
}

/* ------------------------------------------------------------------------- */

void portal_start(bool captive)
{
    s_captive = captive;

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(CONFIG_RADAR_HOSTNAME);
        mdns_instance_name_set("ADS-B radar");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }

    if (captive) {
        xTaskCreate(dns_task, "dns", 3072, NULL, 3, NULL);
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 12;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.lru_purge_enable = true;

    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/api/config", .method = HTTP_GET, .handler = config_get},
        {.uri = "/api/config", .method = HTTP_POST, .handler = config_post},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = ota_post},
        {.uri = "/api/map", .method = HTTP_GET, .handler = map_get},
        {.uri = "/api/map", .method = HTTP_POST, .handler = map_post},
        {.uri = "/api/map", .method = HTTP_DELETE, .handler = map_delete},
        {.uri = "/api/scan", .method = HTTP_GET, .handler = scan_get},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/*", .method = HTTP_GET, .handler = catch_all},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(srv, &uris[i]);
    }
    ESP_LOGI(TAG, "web server up (%s)", captive ? "captive" : "station");
}
