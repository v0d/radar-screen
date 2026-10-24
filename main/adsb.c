#include "adsb.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "config_store.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi_mgr.h"

static const char *TAG = "adsb";

#define NOTIFY_FETCH (1u << 0)
#define NOTIFY_ROUTE (1u << 1)
#define NOTIFY_RANGE (1u << 2)

#define OBJ_CAP 2048        /* max bytes of a single aircraft JSON object */
#define ROUTE_CACHE 8
#define ROUTE_TTL_US (600LL * 1000000)
#define ROUTE_RETRY_US (30LL * 1000000)
#define ROUTE_BODY_CAP 4096

#define ADSBLOL_BASE "https://api.adsb.lol"
#define ADSBLOL_KEY_HEADER "X-API-Key"
#define RATE_LIMIT_DEFAULT_S 60   /* 429 without a usable Retry-After */
#define RATE_LIMIT_MAX_S 3600     /* re-check at least hourly, whatever the server says */
#define ADSBDB_ROUTE_URL "https://api.adsbdb.com/v0/callsign/"

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

static TaskHandle_t s_task;
static SemaphoreHandle_t s_lock;
static adsb_snapshot_t s_snap;
static volatile float s_range_nm = 20.0f;
static int64_t s_retry_until;        /* adsb.lol rate limit hold (fetch task only) */
static int s_fails;                  /* consecutive failed fetches (fetch task only) */
static char s_limited_key[76];       /* API key that was rate limited */

typedef struct {
    plane_t list[ADSB_MAX_PLANES];
    int n;
    int total;
    double lat0, lon0, coslat;
    float max_nm;
} scan_ctx_t;

static scan_ctx_t s_scan;

typedef struct {
    bool used;
    char callsign[9];
    int64_t ts_us;
    route_info_t info;
} route_entry_t;

static route_entry_t s_routes[ROUTE_CACHE];
static int s_route_next;
static char s_route_req[9];

/* ------------------------------------------------------------------------- */
/* String helpers                                                            */
/* ------------------------------------------------------------------------- */

/* Copy to a display-safe ASCII string: transliterate Latin-1 letters, drop everything else,
 * trim trailing blanks and neutralise '#' (used for LVGL colour markup). */
static void clean_copy(char *dst, size_t dsz, const char *src)
{
    static const char latin1[] = "AAAAAAACEEEEIIIIDNOOOOO*OUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 1 < dsz; p++) {
        unsigned char c = *p;
        if (c >= 0x20 && c < 0x7f) {
            dst[o++] = (c == '#') ? '-' : (char)c;
        } else if (c == 0xC3 && p[1] >= 0x80 && p[1] <= 0xBF) {
            dst[o++] = latin1[p[1] - 0x80];
            p++;
        } else if (c >= 0xC0) {
            while ((p[1] & 0xC0) == 0x80) {
                p++;
            }
            dst[o++] = '?';
        }
    }
    while (o > 0 && dst[o - 1] == ' ') {
        o--;
    }
    dst[o] = 0;
}

static bool get_num(const cJSON *o, const char *k, double *v)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(i)) {
        *v = i->valuedouble;
        return true;
    }
    return false;
}

static void get_str(const cJSON *o, const char *k, char *dst, size_t n)
{
    dst[0] = 0;
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(i) && i->valuestring) {
        clean_copy(dst, n, i->valuestring);
    }
}

static int32_t to_i32(double v)
{
    if (v > 2000000000.0) {
        return 2000000000;
    }
    if (v < -2000000000.0) {
        return -2000000000;
    }
    return (int32_t)lround(v);
}

static int16_t to_i16(double v)
{
    if (v > 32000.0) {
        return 32000;
    }
    if (v < -32000.0) {
        return -32000;
    }
    return (int16_t)lround(v);
}

/* ------------------------------------------------------------------------- */
/* Aircraft parsing                                                          */
/* ------------------------------------------------------------------------- */

static void handle_object(scan_ctx_t *c, const char *json, size_t len)
{
    cJSON *o = cJSON_ParseWithLength(json, len);
    if (!o) {
        return;
    }

    double lat, lon, v;
    if (!get_num(o, "lat", &lat) || !get_num(o, "lon", &lon)) {
        cJSON_Delete(o);
        return;
    }
    c->total++;

    double dlon = lon - c->lon0;
    if (dlon > 180.0) {
        dlon -= 360.0;
    } else if (dlon < -180.0) {
        dlon += 360.0;
    }
    float x = (float)(dlon * c->coslat * 60.0);
    float y = (float)((lat - c->lat0) * 60.0);
    float d = hypotf(x, y);
    if (d > c->max_nm) {
        cJSON_Delete(o);
        return;
    }

    /* pick a slot: append, or replace the farthest one if this plane is closer */
    int slot = -1;
    if (c->n < ADSB_MAX_PLANES) {
        slot = c->n++;
    } else {
        int far_i = 0;
        for (int i = 1; i < ADSB_MAX_PLANES; i++) {
            if (c->list[i].dist_nm > c->list[far_i].dist_nm) {
                far_i = i;
            }
        }
        if (d < c->list[far_i].dist_nm) {
            slot = far_i;
        }
    }
    if (slot < 0) {
        cJSON_Delete(o);
        return;
    }

    plane_t *p = &c->list[slot];
    memset(p, 0, sizeof(*p));
    p->x_nm = x;
    p->y_nm = y;
    p->dist_nm = d;
    p->brg_deg = (float)(atan2((double)x, (double)y) * 180.0 / M_PI);
    if (p->brg_deg < 0) {
        p->brg_deg += 360.0f;
    }

    get_str(o, "hex", p->hex, sizeof(p->hex));
    get_str(o, "flight", p->flight, sizeof(p->flight));
    get_str(o, "r", p->reg, sizeof(p->reg));
    get_str(o, "t", p->type, sizeof(p->type));
    get_str(o, "squawk", p->squawk, sizeof(p->squawk));
    get_str(o, "category", p->category, sizeof(p->category));
    get_str(o, "emergency", p->emergency, sizeof(p->emergency));
    if (strcmp(p->emergency, "none") == 0) {
        p->emergency[0] = 0;
    }

    const cJSON *alt = cJSON_GetObjectItemCaseSensitive(o, "alt_baro");
    if (cJSON_IsNumber(alt)) {
        p->alt_baro_ft = to_i32(alt->valuedouble);
        p->has |= PF_ALT;
    } else if (cJSON_IsString(alt) && alt->valuestring && strcmp(alt->valuestring, "ground") == 0) {
        p->has |= PF_GROUND;
    }
    if (get_num(o, "alt_geom", &v)) {
        p->alt_geom_ft = to_i32(v);
        p->has |= PF_ALTGEOM;
    }
    if (get_num(o, "gs", &v)) {
        p->gs_kt = to_i16(v);
        p->has |= PF_GS;
    }
    if (get_num(o, "track", &v) || get_num(o, "true_heading", &v) || get_num(o, "mag_heading", &v)) {
        p->track_deg = to_i16(v);
        p->has |= PF_TRACK;
    }
    if (get_num(o, "mag_heading", &v)) {
        p->mag_hdg_deg = to_i16(v);
        p->has |= PF_MAGHDG;
    }
    if (get_num(o, "baro_rate", &v) || get_num(o, "geom_rate", &v)) {
        p->vs_fpm = to_i16(v);
        p->has |= PF_VS;
    }
    if (get_num(o, "ias", &v)) {
        p->ias_kt = to_i16(v);
        p->has |= PF_IAS;
    }
    if (get_num(o, "tas", &v)) {
        p->tas_kt = to_i16(v);
        p->has |= PF_TAS;
    }
    if (get_num(o, "mach", &v)) {
        p->mach = (float)v;
        p->has |= PF_MACH;
    }
    if (get_num(o, "nav_altitude_mcp", &v) || get_num(o, "nav_altitude_fms", &v)) {
        p->nav_alt_ft = to_i32(v);
        p->has |= PF_NAVALT;
    }
    if (get_num(o, "rssi", &v)) {
        p->rssi = (float)v;
        p->has |= PF_RSSI;
    }
    if (get_num(o, "seen_pos", &v) || get_num(o, "seen", &v)) {
        p->age_s = (float)v;
    }

    cJSON_Delete(o);
}

/* Streaming splitter: finds the aircraft objects (depth 3: root{ array[ object{ ) in a response of
 * unbounded size and parses them one at a time, so memory use is independent of the response. */
typedef struct {
    int depth;
    bool in_str;
    bool esc;
    bool capturing;
    bool overflow;
    char *buf;
    size_t len;
} stream_t;

static inline void stream_put(stream_t *s, char c)
{
    if (!s->capturing) {
        return;
    }
    if (s->len < OBJ_CAP - 1) {
        s->buf[s->len++] = c;
    } else {
        s->overflow = true;
    }
}

static void stream_feed(const char *d, int n, void *ud)
{
    stream_t *s = ud;
    for (int i = 0; i < n; i++) {
        char c = d[i];
        if (s->in_str) {
            stream_put(s, c);
            if (s->esc) {
                s->esc = false;
            } else if (c == '\\') {
                s->esc = true;
            } else if (c == '"') {
                s->in_str = false;
            }
            continue;
        }
        switch (c) {
        case '"':
            s->in_str = true;
            stream_put(s, c);
            break;
        case '{':
        case '[':
            s->depth++;
            if (c == '{' && s->depth == 3 && !s->capturing) {
                s->capturing = true;
                s->overflow = false;
                s->len = 0;
            }
            stream_put(s, c);
            break;
        case '}':
        case ']':
            stream_put(s, c);
            if (c == '}' && s->depth == 3 && s->capturing) {
                s->capturing = false;
                if (!s->overflow) {
                    s->buf[s->len] = 0;
                    handle_object(&s_scan, s->buf, s->len);
                }
            }
            s->depth--;
            break;
        default:
            stream_put(s, c);
            break;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* HTTP                                                                      */
/* ------------------------------------------------------------------------- */

typedef void (*chunk_cb_t)(const char *data, int len, void *ud);

typedef struct {
    int retry_after_s; /* Retry-After in seconds, -1 = absent or not a number */
    int reset_s;       /* x-ratelimit-reset, -1 = absent */
} resp_hdrs_t;

static int parse_seconds(const char *v)
{
    char *end;
    long n = strtol(v, &end, 10);
    return (end != v && *end == 0 && n >= 0 && n < INT_MAX) ? (int)n : -1;
}

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    resp_hdrs_t *r = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_HEADER && r && evt->header_key && evt->header_value) {
        if (strcasecmp(evt->header_key, "Retry-After") == 0) {
            r->retry_after_s = parse_seconds(evt->header_value); /* HTTP-date form falls back */
        } else if (strcasecmp(evt->header_key, "x-ratelimit-reset") == 0) {
            r->reset_s = parse_seconds(evt->header_value);
        }
    }
    return ESP_OK;
}

/* GET url and stream the body (only for HTTP 200) to cb. *status gets the HTTP status (0 on
 * transport failure), *retry_s (optional) the server's requested wait for a 429, or -1. api_key, if
 * set, is sent as ADSBLOL_KEY_HEADER. Returns ESP_OK if the transfer completed. */
static esp_err_t http_get(const char *url, const char *api_key, int timeout_ms, chunk_cb_t cb, void *ud, int *status,
                          int *retry_s)
{
    *status = 0;
    resp_hdrs_t hdrs = {.retry_after_s = -1, .reset_s = -1};
    const esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .user_agent = "radar-esp32/1.0",
        .event_handler = on_http_event,
        .user_data = &hdrs,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(h, "Accept", "application/json");
    if (api_key && api_key[0]) {
        esp_http_client_set_header(h, ADSBLOL_KEY_HEADER, api_key);
    }

    esp_err_t err = esp_http_client_open(h, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(h);
        *status = esp_http_client_get_status_code(h);
        if (*status == 200) {
            char *chunk = malloc(1024);
            if (!chunk) {
                err = ESP_ERR_NO_MEM;
            } else {
                int n;
                while ((n = esp_http_client_read(h, chunk, 1024)) > 0) {
                    cb(chunk, n, ud);
                }
                if (n < 0) {
                    err = ESP_FAIL;
                } else if (!esp_http_client_is_complete_data_received(h)) {
                    err = ESP_ERR_INVALID_RESPONSE;
                }
                free(chunk);
            }
        } else if (*status == 0) {
            err = ESP_FAIL;
        }
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    if (retry_s) {
        *retry_s = hdrs.retry_after_s >= 0 ? hdrs.retry_after_s : hdrs.reset_s;
    }
    return err;
}

/* ------------------------------------------------------------------------- */
/* Plane fetching                                                            */
/* ------------------------------------------------------------------------- */

static int cmp_dist(const void *a, const void *b)
{
    float da = ((const plane_t *)a)->dist_nm;
    float db = ((const plane_t *)b)->dist_nm;
    return (da > db) - (da < db);
}

static void publish(adsb_state_t st, int http_status, bool have_data)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (have_data) {
        memcpy(s_snap.planes, s_scan.list, sizeof(plane_t) * s_scan.n);
        s_snap.count = s_scan.n;
        s_snap.total_seen = s_scan.total;
        s_snap.t_us = esp_timer_get_time();
    }
    s_snap.state = st;
    s_snap.http_status = http_status;
    s_snap.retry_until_us = s_retry_until;
    s_snap.fails = s_fails;
    s_snap.version++;
    xSemaphoreGive(s_lock);
}

static void build_url(const radar_config_t *cfg, float radius_nm, char *url, size_t n)
{
    if (cfg->source == SRC_TAR1090) {
        size_t l = strlen(cfg->base_url);
        while (l > 0 && cfg->base_url[l - 1] == '/') {
            l--;
        }
        if (strstr(cfg->base_url, ".json")) {
            snprintf(url, n, "%s", cfg->base_url);
        } else {
            snprintf(url, n, "%.*s/data/aircraft.json", (int)l, cfg->base_url);
        }
    } else {
        snprintf(url, n, ADSBLOL_BASE "/v2/point/%.5f/%.5f/%d", cfg->lat, cfg->lon, (int)ceilf(radius_nm));
    }
}

/* returns true when the fetch succeeded */
static bool do_fetch(void)
{
    if (!wifi_mgr_is_connected()) {
        publish(ADSB_ST_NO_WIFI, 0, false);
        return false;
    }

    radar_config_t cfg;
    cfg_get(&cfg);

    float radius = s_range_nm * 1.1f + 1.0f;
    if (radius > 250.0f) {
        radius = 250.0f;
    }

    char url[192];
    build_url(&cfg, radius, url, sizeof(url));

    memset(&s_scan, 0, sizeof(s_scan));
    s_scan.lat0 = cfg.lat;
    s_scan.lon0 = cfg.lon;
    s_scan.coslat = cos(cfg.lat * M_PI / 180.0);
    s_scan.max_nm = radius;

    stream_t *st = calloc(1, sizeof(*st));
    if (!st) {
        return false;
    }
    st->buf = malloc(OBJ_CAP);
    if (!st->buf) {
        free(st);
        return false;
    }

    int status = 0;
    int retry_s = -1;
    int64_t t0 = esp_timer_get_time();
    const bool adsblol = cfg.source == SRC_ADSBLOL;
    esp_err_t err = http_get(url, adsblol ? cfg.api_key : NULL, 10000, stream_feed, st, &status, &retry_s);
    free(st->buf);
    free(st);

    if (status == 429 && adsblol) {
        if (retry_s < 1) {
            retry_s = RATE_LIMIT_DEFAULT_S;
        } else if (retry_s > RATE_LIMIT_MAX_S) {
            retry_s = RATE_LIMIT_MAX_S;
        }
        s_retry_until = esp_timer_get_time() + retry_s * 1000000LL;
        strlcpy(s_limited_key, cfg.api_key, sizeof(s_limited_key));
        ESP_LOGW(TAG, "rate limited, next request in %d s", retry_s);
        publish(ADSB_ST_RATE_LIMITED, status, false);
        return false;
    }
    if (err != ESP_OK || status != 200) {
        s_fails++;
        ESP_LOGW(TAG, "fetch failed: %s (http %d)", esp_err_to_name(err), status);
        publish(ADSB_ST_HTTP_ERR, status, false);
        return false;
    }

    s_fails = 0;
    qsort(s_scan.list, s_scan.n, sizeof(plane_t), cmp_dist);
    ESP_LOGI(TAG, "%d/%d aircraft in %.1f nm (%lld ms, heap %u)", s_scan.n, s_scan.total, radius,
             (long long)((esp_timer_get_time() - t0) / 1000), (unsigned)esp_get_free_heap_size());
    publish(ADSB_ST_OK, status, true);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Route lookup (adsbdb.com)                                                 */
/* ------------------------------------------------------------------------- */

typedef struct {
    char *buf;
    size_t len;
} body_t;

static void body_collect(const char *d, int n, void *ud)
{
    body_t *b = ud;
    if (b->len + n < ROUTE_BODY_CAP) {
        memcpy(b->buf + b->len, d, n);
        b->len += n;
        b->buf[b->len] = 0;
    }
}

static void fill_airport(const cJSON *ap, char *iata, size_t ni, char *icao, size_t nc, char *name, size_t nn,
                         char *city, size_t ncity)
{
    iata[0] = icao[0] = name[0] = city[0] = 0;
    if (!cJSON_IsObject(ap)) {
        return;
    }
    get_str(ap, "iata_code", iata, ni);
    get_str(ap, "icao_code", icao, nc);
    get_str(ap, "name", name, nn);
    get_str(ap, "municipality", city, ncity);
}

static route_entry_t *route_find_locked(const char *cs)
{
    for (int i = 0; i < ROUTE_CACHE; i++) {
        if (s_routes[i].used && strcmp(s_routes[i].callsign, cs) == 0) {
            return &s_routes[i];
        }
    }
    return NULL;
}

static void route_store(const char *cs, const route_info_t *info)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    route_entry_t *e = route_find_locked(cs);
    if (!e) {
        e = &s_routes[s_route_next];
        s_route_next = (s_route_next + 1) % ROUTE_CACHE;
    }
    e->used = true;
    strlcpy(e->callsign, cs, sizeof(e->callsign));
    e->ts_us = esp_timer_get_time();
    e->info = *info;
    xSemaphoreGive(s_lock);
}

static void process_route(void)
{
    char cs[9];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(cs, s_route_req, sizeof(cs));
    s_route_req[0] = 0;
    xSemaphoreGive(s_lock);
    if (!cs[0]) {
        return;
    }

    route_info_t info;
    memset(&info, 0, sizeof(info));
    info.state = ROUTE_ERROR;

    char url[96];
    snprintf(url, sizeof(url), ADSBDB_ROUTE_URL "%s", cs);

    body_t body = {.buf = malloc(ROUTE_BODY_CAP), .len = 0};
    if (!body.buf) {
        return;
    }
    body.buf[0] = 0;

    int status = 0;
    esp_err_t err =
        wifi_mgr_is_connected() ? http_get(url, NULL, 8000, body_collect, &body, &status, NULL) : ESP_FAIL;
    if (err == ESP_OK && status == 200) {
        info.state = ROUTE_UNKNOWN;
        cJSON *root = cJSON_ParseWithLength(body.buf, body.len);
        const cJSON *resp = root ? cJSON_GetObjectItemCaseSensitive(root, "response") : NULL;
        const cJSON *fr = cJSON_IsObject(resp) ? cJSON_GetObjectItemCaseSensitive(resp, "flightroute") : NULL;
        if (cJSON_IsObject(fr)) {
            info.state = ROUTE_FOUND;
            const cJSON *al = cJSON_GetObjectItemCaseSensitive(fr, "airline");
            if (cJSON_IsObject(al)) {
                get_str(al, "name", info.airline, sizeof(info.airline));
            }
            fill_airport(cJSON_GetObjectItemCaseSensitive(fr, "origin"), info.orig_iata, sizeof(info.orig_iata),
                         info.orig_icao, sizeof(info.orig_icao), info.orig_name, sizeof(info.orig_name),
                         info.orig_city, sizeof(info.orig_city));
            fill_airport(cJSON_GetObjectItemCaseSensitive(fr, "destination"), info.dest_iata,
                         sizeof(info.dest_iata), info.dest_icao, sizeof(info.dest_icao), info.dest_name,
                         sizeof(info.dest_name), info.dest_city, sizeof(info.dest_city));
        }
        cJSON_Delete(root);
    } else if (err == ESP_OK && status == 404) {
        info.state = ROUTE_UNKNOWN;
    } else {
        ESP_LOGW(TAG, "route lookup %s failed: %s (http %d)", cs, esp_err_to_name(err), status);
    }
    free(body.buf);
    route_store(cs, &info);
}

void adsb_request_route(const char *callsign)
{
    if (!callsign || !callsign[0]) {
        return;
    }
    /* callsigns come from the network: only allow plain alphanumerics into the URL */
    char cs[9];
    size_t o = 0;
    for (const char *p = callsign; *p && o < sizeof(cs) - 1; p++) {
        if (isalnum((unsigned char)*p)) {
            cs[o++] = (char)toupper((unsigned char)*p);
        }
    }
    cs[o] = 0;
    if (!o) {
        return;
    }

    if (!s_lock) {
        return; /* adsb_init() not called (setup mode) */
    }
    bool need;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    route_entry_t *e = route_find_locked(cs);
    int64_t age = e ? now - e->ts_us : 0;
    need = !e || age > ROUTE_TTL_US || (e->info.state == ROUTE_ERROR && age > ROUTE_RETRY_US);
    if (need) {
        strlcpy(s_route_req, cs, sizeof(s_route_req));
    }
    xSemaphoreGive(s_lock);
    if (need && s_task) {
        xTaskNotify(s_task, NOTIFY_ROUTE, eSetBits);
    }
}

route_state_t adsb_get_route(const char *callsign, route_info_t *out)
{
    route_state_t st = ROUTE_NONE;
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!callsign || !callsign[0]) {
        return st;
    }
    char cs[9];
    size_t o = 0;
    for (const char *p = callsign; *p && o < sizeof(cs) - 1; p++) {
        if (isalnum((unsigned char)*p)) {
            cs[o++] = (char)toupper((unsigned char)*p);
        }
    }
    cs[o] = 0;

    if (!s_lock) {
        return st;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    route_entry_t *e = route_find_locked(cs);
    if (e) {
        st = e->info.state;
        if (out) {
            *out = e->info;
        }
    }
    xSemaphoreGive(s_lock);
    return st;
}

/* ------------------------------------------------------------------------- */
/* Task / public API                                                         */
/* ------------------------------------------------------------------------- */

static void adsb_task(void *arg)
{
    (void)arg;
    int64_t next_fetch = 0;

    for (;;) {
        int64_t now = esp_timer_get_time();
        TickType_t wait = next_fetch > now ? pdMS_TO_TICKS((next_fetch - now) / 1000) + 1 : 0;
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, wait);

        if (bits & NOTIFY_RANGE) {
            next_fetch = esp_timer_get_time() + 600000; /* debounce knob turns */
        }
        if (bits & NOTIFY_FETCH) {
            next_fetch = 0;
        }
        if (bits & NOTIFY_ROUTE) {
            radar_config_t cfg;
            cfg_get(&cfg);
            if (cfg.lookup_routes) {
                process_route();
            }
        }
        if (esp_timer_get_time() >= next_fetch) {
            radar_config_t cfg;
            cfg_get(&cfg);
            /* A 429 hold applies to adsb.lol with the same key; switching source or key ends it. */
            if (s_retry_until && (cfg.source != SRC_ADSBLOL || strcmp(cfg.api_key, s_limited_key) != 0)) {
                s_retry_until = 0;
            }
            if (s_retry_until > esp_timer_get_time()) {
                next_fetch = s_retry_until; /* zoom changes and config saves must not bypass it */
                continue;
            }
            s_retry_until = 0;
            bool ok = do_fetch();
            int64_t delay_us = ok ? (int64_t)cfg.refresh_s * 1000000 : 2000000;
            next_fetch = s_retry_until ? s_retry_until : esp_timer_get_time() + delay_us;
        }
    }
}

void adsb_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    xTaskCreate(adsb_task, "adsb", 10240, NULL, 4, &s_task);
}

void adsb_set_range_nm(float nm)
{
    if (nm == s_range_nm) {
        return;
    }
    s_range_nm = nm;
    if (s_task) {
        xTaskNotify(s_task, NOTIFY_RANGE, eSetBits);
    }
}

void adsb_kick(void)
{
    if (s_task) {
        xTaskNotify(s_task, NOTIFY_FETCH, eSetBits);
    }
}

uint32_t adsb_version(void)
{
    return s_snap.version;
}

/* Both getters may be called before adsb_init() (e.g. UI timer in setup mode). Before init there is no
 * writer, so reading the zero-initialised snapshot without the lock is safe. The handle is copied so
 * take/give always use the same one. */
void adsb_get_snapshot(adsb_snapshot_t *out)
{
    SemaphoreHandle_t l = s_lock;
    if (l) {
        xSemaphoreTake(l, portMAX_DELAY);
    }
    memcpy(out, &s_snap, sizeof(*out));
    if (l) {
        xSemaphoreGive(l);
    }
}

void adsb_get_stats(adsb_state_t *state, int *count, int *total, int *retry_s)
{
    SemaphoreHandle_t l = s_lock;
    if (l) {
        xSemaphoreTake(l, portMAX_DELAY);
    }
    *state = s_snap.state;
    *count = s_snap.count;
    *total = s_snap.total_seen;
    int64_t left = s_snap.retry_until_us - esp_timer_get_time();
    *retry_s = left > 0 ? (int)(left / 1000000) : 0;
    if (l) {
        xSemaphoreGive(l);
    }
}

void adsb_get_health(adsb_health_t *out)
{
    SemaphoreHandle_t l = s_lock;
    if (l) {
        xSemaphoreTake(l, portMAX_DELAY);
    }
    out->state = s_snap.state;
    out->http_status = s_snap.http_status;
    out->fails = s_snap.fails;
    out->t_us = s_snap.t_us;
    if (l) {
        xSemaphoreGive(l);
    }
}

const char *adsb_state_str(adsb_state_t s)
{
    switch (s) {
    case ADSB_ST_OK:
        return "ok";
    case ADSB_ST_NO_WIFI:
        return "no wifi";
    case ADSB_ST_HTTP_ERR:
        return "http error";
    case ADSB_ST_PARSE_ERR:
        return "bad data";
    case ADSB_ST_RATE_LIMITED:
        return "rate limited";
    default:
        return "starting";
    }
}
