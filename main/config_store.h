#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    SRC_ADSBLOL = 0,  /* https://api.adsb.lol/v2/point/{lat}/{lon}/{nm} */
    SRC_TAR1090 = 1,  /* <base_url>/data/aircraft.json (tar1090 / readsb / dump1090-fa) */
} data_source_t;

typedef enum {
    UNITS_AVIATION = 0,  /* nm, ft / FL, kt */
    UNITS_METRIC = 1,    /* km, m, km/h */
    UNITS_IMPERIAL = 2,  /* mi, ft, mph */
} units_t;

#define CFG_REF_POINTS 4

typedef struct {
    float lat;
    float lon;
    char name[8];  /* empty = unused slot */
} ref_point_t;

enum {
    LABEL_CALLSIGN = 1 << 0,
    LABEL_TYPE = 1 << 1,
    LABEL_ALT = 1 << 2,
};

typedef struct {
    uint32_t version;
    char wifi_ssid[33];
    char wifi_pass[65];
    double lat;
    double lon;
    uint8_t source;        /* data_source_t */
    uint8_t units;         /* units_t */
    uint8_t default_zoom;  /* index into the zoom table (0..5) */
    uint8_t lookup_routes; /* look up origin/destination for the selected aircraft */
    uint16_t refresh_s;    /* seconds between fetches (1..60) */
    char base_url[96];     /* tar1090 base URL, e.g. http://192.168.1.20/tar1090 */
    uint8_t sweep;         /* draw the rotating radar sweep */
    /* v4: everything below is absent from v1-3 blobs and filled with defaults on load */
    uint8_t trails;        /* short history trail behind each aircraft */
    uint8_t alt_colors;    /* colour aircraft by altitude band */
    uint8_t hide_ground;   /* hide aircraft on the ground */
    uint8_t leader_s;      /* predicted track line length in seconds (0 = off) */
    uint16_t max_alt_ft;   /* hide aircraft above this altitude (0 = no limit) */
    uint8_t brightness;    /* backlight percent (5..100) */
    uint8_t dim_level;     /* backlight percent when dimmed (0 = off) */
    uint8_t dim_after_min; /* dim after this many minutes without input (0 = never) */
    uint8_t night;         /* dim during the night hours (needs SNTP time) */
    uint8_t night_start_h;
    uint8_t night_end_h;
    char tz[40];           /* POSIX TZ string, e.g. CET-1CEST,M3.5.0,M10.5.0/3 */
    uint8_t alert;         /* proximity alert */
    uint16_t alert_dist_nm10; /* alert radius in 0.1 nm */
    uint16_t alert_alt_ft;    /* alert only for aircraft at or below this altitude */
    ref_point_t ref[CFG_REF_POINTS];
    /* v5 */
    char api_key[76];      /* adsb.lol API key (empty = none) */
    /* v6 */
    uint8_t rotation;      /* screen rotation clockwise in 90 degree steps (0..3) */
    /* v7 */
    uint8_t label_fields;  /* LABEL_* shown next to each aircraft */
    /* v8 */
    uint8_t show_count;    /* aircraft count on the top line */
} radar_config_t;

#define CFG_ZOOM_LEVELS 6

/* Load from NVS (or defaults). Must be called after nvs_flash_init(). */
void cfg_init(void);

/* Thread-safe copy of the current configuration. */
void cfg_get(radar_config_t *out);

/* Validate-free store: caller must validate. Persists to NVS and bumps the generation. */
esp_err_t cfg_set(const radar_config_t *in);

/* Incremented every time cfg_set() succeeds. */
uint32_t cfg_generation(void);

bool cfg_has_wifi(void);
