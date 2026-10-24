#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ADSB_MAX_PLANES 48

enum {
    PF_ALT = 1 << 0,      /* alt_baro_ft valid */
    PF_GROUND = 1 << 1,   /* on ground */
    PF_GS = 1 << 2,
    PF_TRACK = 1 << 3,
    PF_VS = 1 << 4,
    PF_IAS = 1 << 5,
    PF_TAS = 1 << 6,
    PF_MACH = 1 << 7,
    PF_NAVALT = 1 << 8,
    PF_ALTGEOM = 1 << 9,
    PF_MAGHDG = 1 << 10,
    PF_RSSI = 1 << 11,
};

typedef struct {
    char hex[7];
    char flight[9];
    char reg[11];
    char type[6];
    char squawk[5];
    char category[3];
    char emergency[10];
    uint16_t has;
    float x_nm;    /* east offset from the radar centre */
    float y_nm;    /* north offset */
    float dist_nm;
    float brg_deg;
    int32_t alt_baro_ft;
    int32_t alt_geom_ft;
    int32_t nav_alt_ft;
    int16_t gs_kt;
    int16_t ias_kt;
    int16_t tas_kt;
    int16_t track_deg;
    int16_t mag_hdg_deg;
    int16_t vs_fpm;
    float mach;
    float rssi;
    float age_s;   /* age of the position at fetch time */
} plane_t;

typedef enum {
    ADSB_ST_INIT = 0,
    ADSB_ST_OK,
    ADSB_ST_NO_WIFI,
    ADSB_ST_HTTP_ERR,
    ADSB_ST_PARSE_ERR,
    ADSB_ST_RATE_LIMITED, /* HTTP 429: waiting until retry_until_us */
} adsb_state_t;

typedef struct {
    plane_t planes[ADSB_MAX_PLANES]; /* sorted by distance, nearest first */
    int count;
    int total_seen;      /* aircraft with a position in the response (before filtering) */
    int64_t t_us;        /* esp_timer time of the last successful fetch (0 = never) */
    adsb_state_t state;
    int http_status;
    int64_t retry_until_us; /* esp_timer time of the next allowed request (ADSB_ST_RATE_LIMITED) */
    int fails;              /* consecutive failed fetches */
    uint32_t version;
} adsb_snapshot_t;

typedef struct {
    adsb_state_t state;
    int http_status;
    int fails;
    int64_t t_us;
} adsb_health_t;

typedef enum {
    ROUTE_NONE = 0,   /* not requested / in flight */
    ROUTE_FOUND,
    ROUTE_UNKNOWN,    /* lookup succeeded but callsign has no known route */
    ROUTE_ERROR,      /* lookup failed (offline etc.) */
} route_state_t;

typedef struct {
    route_state_t state;
    char airline[28];
    char orig_iata[5];
    char orig_icao[5];
    char orig_name[32];
    char orig_city[20];
    char dest_iata[5];
    char dest_icao[5];
    char dest_name[32];
    char dest_city[20];
} route_info_t;

/* Starts the background fetch task. */
void adsb_init(void);

/* Tell the fetcher how far (nm) it has to look. Triggers a debounced refetch. */
void adsb_set_range_nm(float nm);

/* Config changed (location, source ...): refetch now. */
void adsb_kick(void);

uint32_t adsb_version(void);
void adsb_get_snapshot(adsb_snapshot_t *out);

/* Cheap summary for the status page. *retry_s: seconds until the next request while rate limited. */
void adsb_get_stats(adsb_state_t *state, int *count, int *total, int *retry_s);

/* Cheap state summary for the status LED (safe before adsb_init: reports ADSB_ST_INIT). */
void adsb_get_health(adsb_health_t *out);

/* Ask for origin/destination of a callsign (async, result via adsb_get_route). */
void adsb_request_route(const char *callsign);
route_state_t adsb_get_route(const char *callsign, route_info_t *out);

/* Short text for status/debug page. */
const char *adsb_state_str(adsb_state_t s);
