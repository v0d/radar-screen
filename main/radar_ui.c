#include "radar_ui.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "adsb.h"
#include "board.h"
#include "config_store.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"

/* ------------------------------------------------------------------------- */
/* Geometry / palette                                                        */
/* ------------------------------------------------------------------------- */

#define CX 120
#define CY 120
#define R_OUT 108
#define DEG2RAD 0.017453292f

#define TICK_MS 80
#define SWEEP_PERIOD_MS 4000
#define TRAIL_SLICES 5
#define TRAIL_STEP_DEG 6
#define OVERLAY_PERIOD_US 250000
#define SELECT_TIMEOUT_US (30LL * 1000000)
#define LABELS_MAX_PLANES 10
#define BLINK_PERIOD_US 500000
#define ALERT_MAX_AGE_US (60LL * 1000000) /* no alerts from data older than this */

#define TRAIL_POINTS 8
#define TRAIL_SLOTS 64                      /* more than ADSB_MAX_PLANES so brief dropouts keep their trail */
#define TRAIL_INTERVAL_US (8LL * 1000000)   /* min. time between two trail points */
#define TRAIL_FORGET_US (300LL * 1000000)

#define COL_BG lv_color_hex(0x04140A)
#define COL_RING lv_color_hex(0x14602C)
#define COL_RING_OUT lv_color_hex(0x2DBE5A)
#define COL_GREEN lv_color_hex(0x3DFF6B)
#define COL_DIM lv_color_hex(0x4C7A58)
#define COL_AMBER lv_color_hex(0xFFC400)
#define COL_RED lv_color_hex(0xFF4040)
#define COL_TEXT lv_color_hex(0xC8F5D2)
#define COL_REF lv_color_hex(0x8FB0C8)
#define COL_LABEL2 lv_color_hex(0x8FC29C)  /* second label line (type / altitude) */

/* altitude bands: < 5000, < 10000, < 20000, < 30000, above (ft) */
#define COL_ALT1 lv_color_hex(0xFF8A3D)
#define COL_ALT2 lv_color_hex(0xF2E24A)
#define COL_ALT3 lv_color_hex(0x3DFF6B)
#define COL_ALT4 lv_color_hex(0x3DD8FF)
#define COL_ALT5 lv_color_hex(0xC490FF)

/* Zoom ranges expressed in the display unit; nm_per_unit converts them for the data fetch. */
static const float ZOOM_UNITS[3][CFG_ZOOM_LEVELS] = {
    {5, 10, 20, 40, 80, 160},     /* aviation: nm */
    {10, 25, 50, 100, 200, 300},  /* metric: km */
    {5, 10, 25, 50, 100, 150},    /* imperial: mi */
};
static const float NM_PER_UNIT[3] = {1.0f, 0.539957f, 0.868976f};
static const char *const DIST_UNIT[3] = {"nm", "km", "mi"};

/* Clockwise rotation setting -> LVGL rotation. The panel does the rotation (MADCTL); with this panel's
 * axis setup LVGL's 90 degree step turns the image counter-clockwise. */
static const lv_disp_rot_t ROTATION[4] = {LV_DISP_ROT_NONE, LV_DISP_ROT_270, LV_DISP_ROT_180, LV_DISP_ROT_90};

typedef enum {
    MODE_RADAR = 0,
    MODE_SELECT,
} ui_mode_t;

typedef struct {
    int16_t x, y;      /* screen position */
    float ux, uy;      /* heading unit vector in screen space */
    float x_nm, y_nm;  /* dead-reckoned position relative to the centre */
    bool visible;
} blip_t;

/* Position history of one aircraft, kept across fetches by hex code. */
typedef struct {
    char hex[7];
    uint8_t n;      /* points stored */
    uint8_t head;   /* next write index */
    int64_t last_us; /* time of the newest point */
    int64_t seen_us; /* last snapshot that contained this aircraft */
    int16_t x[TRAIL_POINTS], y[TRAIL_POINTS]; /* 0.01 nm */
} trail_t;

/* ------------------------------------------------------------------------- */
/* State (only touched with the LVGL lock held)                              */
/* ------------------------------------------------------------------------- */

static lv_obj_t *s_radar;
static lv_obj_t *s_top;
static lv_obj_t *s_bottom;
static lv_obj_t *s_card;
static lv_obj_t *s_card_lbl;
static lv_obj_t *s_info;
static lv_obj_t *s_info_lbl;
static lv_obj_t *s_msg;
static lv_obj_t *s_msg_lbl;

static adsb_snapshot_t s_snap;
static uint32_t s_snap_ver = UINT32_MAX;
static radar_config_t s_cfg;
static uint32_t s_cfg_gen = UINT32_MAX;
static lv_disp_t *s_disp;

static int s_zoom = 2;
static ui_mode_t s_mode = MODE_RADAR;
static int s_page;  /* 0 = card, 1 = full details */
static char s_sel_hex[7];
static int s_vis[ADSB_MAX_PLANES];
static int s_nvis;
static blip_t s_blip[ADSB_MAX_PLANES];
static float s_sweep_deg;
static int64_t s_last_input_us;
static int64_t s_last_overlay_us;
static bool s_msg_visible;
static bool s_blink;
static int s_alert = -1;            /* snapshot index of the aircraft in the alert zone */
static volatile bool s_alert_active;

static trail_t s_trails[TRAIL_SLOTS];
static int8_t s_trail_of[ADSB_MAX_PLANES]; /* trail slot of each snapshot plane, -1 = none */
static int64_t s_trail_t_us;               /* snapshot time already recorded */
static double s_trail_lat = NAN, s_trail_lon = NAN;

static float s_ref_x[CFG_REF_POINTS], s_ref_y[CFG_REF_POINTS]; /* nm from the centre */

static char s_txt_top[32];
static char s_txt_bottom[24];
static char s_txt_card[200];
static char s_txt_info[520];
static char s_tmp[520];

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static float range_units(void)
{
    return ZOOM_UNITS[s_cfg.units % 3][s_zoom];
}

static float range_nm(void)
{
    return range_units() * NM_PER_UNIT[s_cfg.units % 3];
}

static void set_label(lv_obj_t *lbl, char *cache, size_t cache_sz, const char *txt)
{
    if (strncmp(cache, txt, cache_sz) != 0) {
        strlcpy(cache, txt, cache_sz);
        lv_label_set_text(lbl, txt);
    }
}

static void set_hidden(lv_obj_t *o, bool hidden)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) != hidden) {
        if (hidden) {
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static const char *plane_name(const plane_t *p)
{
    return p->flight[0] ? p->flight : p->hex;
}

static bool plane_emergency(const plane_t *p)
{
    return p->emergency[0] || strcmp(p->squawk, "7700") == 0 || strcmp(p->squawk, "7600") == 0 ||
           strcmp(p->squawk, "7500") == 0;
}

static lv_color_t plane_color(const plane_t *p)
{
    if (plane_emergency(p)) {
        return COL_RED;
    }
    if (p->has & PF_GROUND) {
        return COL_DIM;
    }
    if (!s_cfg.alt_colors || !(p->has & PF_ALT)) {
        return COL_GREEN;
    }
    const int32_t ft = p->alt_baro_ft;
    return ft < 5000 ? COL_ALT1 : ft < 10000 ? COL_ALT2 : ft < 20000 ? COL_ALT3 : ft < 30000 ? COL_ALT4 : COL_ALT5;
}

/* Display filters; emergencies are always shown. */
static bool plane_hidden(const plane_t *p)
{
    if (plane_emergency(p)) {
        return false;
    }
    if (s_cfg.hide_ground && (p->has & PF_GROUND)) {
        return true;
    }
    return s_cfg.max_alt_ft && (p->has & PF_ALT) && p->alt_baro_ft > s_cfg.max_alt_ft;
}

static float blip_bearing(const blip_t *b)
{
    float brg = atan2f(b->x_nm, b->y_nm) / DEG2RAD;
    return brg < 0 ? brg + 360.0f : brg;
}

static void fmt_alt_ft(char *b, size_t n, int32_t ft)
{
    switch (s_cfg.units) {
    case UNITS_METRIC:
        snprintf(b, n, "%ldm", (long)lroundf(ft * 0.3048f));
        break;
    case UNITS_AVIATION:
        if (ft >= 6000) {
            snprintf(b, n, "FL%03ld", (long)((ft + 50) / 100));
        } else {
            snprintf(b, n, "%ldft", (long)ft);
        }
        break;
    default:
        snprintf(b, n, "%ldft", (long)ft);
        break;
    }
}

static void fmt_alt(char *b, size_t n, const plane_t *p)
{
    if (p->has & PF_GROUND) {
        snprintf(b, n, "GND");
    } else if (p->has & PF_ALT) {
        fmt_alt_ft(b, n, p->alt_baro_ft);
    } else {
        snprintf(b, n, "alt --");
    }
}

static void fmt_speed(char *b, size_t n, int kt)
{
    switch (s_cfg.units) {
    case UNITS_METRIC:
        snprintf(b, n, "%dkm/h", (int)lroundf(kt * 1.852f));
        break;
    case UNITS_IMPERIAL:
        snprintf(b, n, "%dmph", (int)lroundf(kt * 1.15078f));
        break;
    default:
        snprintf(b, n, "%dkt", kt);
        break;
    }
}

static void fmt_vs(char *b, size_t n, int fpm)
{
    if (s_cfg.units == UNITS_METRIC) {
        snprintf(b, n, "%+.1fm/s", fpm * 0.00508f);
    } else {
        snprintf(b, n, "%+dfpm", fpm);
    }
}

static void fmt_dist(char *b, size_t n, float nm)
{
    float u = nm / NM_PER_UNIT[s_cfg.units % 3];
    snprintf(b, n, "%.1f%s", u, DIST_UNIT[s_cfg.units % 3]);
}

/* Short countdown: 45s, 12m, 3h */
static void fmt_wait(char *b, size_t n, int64_t us)
{
    long s = (long)((us + 999999) / 1000000);
    if (s < 0) {
        s = 0;
    }
    if (s < 100) {
        snprintf(b, n, "%lds", s);
    } else if (s < 6000) {
        snprintf(b, n, "%ldm", (s + 59) / 60);
    } else {
        snprintf(b, n, "%ldh", (s + 3599) / 3600);
    }
}

#define APP(...)                                         \
    do {                                                 \
        if (off < sizeof(s_tmp)) {                       \
            off += snprintf(s_tmp + off, sizeof(s_tmp) - off, __VA_ARGS__); \
        }                                                \
    } while (0)

/* ------------------------------------------------------------------------- */
/* Selection                                                                 */
/* ------------------------------------------------------------------------- */

static int find_vis_by_hex(void)
{
    for (int k = 0; k < s_nvis; k++) {
        if (strcmp(s_snap.planes[s_vis[k]].hex, s_sel_hex) == 0) {
            return k;
        }
    }
    return -1;
}

static void select_vis(int k)
{
    const plane_t *p = &s_snap.planes[s_vis[k]];
    if (strcmp(p->hex, s_sel_hex) != 0) {
        strlcpy(s_sel_hex, p->hex, sizeof(s_sel_hex));
        s_page = 0;
    }
    if (s_cfg.lookup_routes && p->flight[0]) {
        adsb_request_route(p->flight);
    }
}

/* Step to the next aircraft clockwise (dir > 0) or counter-clockwise by bearing. Unlike the distance
 * order this matches the scope and does not shuffle between fetches. */
static void cycle_selection(int dir)
{
    if (s_nvis == 0) {
        return;
    }
    int cur = find_vis_by_hex();
    if (cur < 0) {
        select_vis(0);
        return;
    }
    const float b0 = blip_bearing(&s_blip[s_vis[cur]]);
    int best = -1;
    float best_d = 0;
    for (int k = 0; k < s_nvis; k++) {
        if (k == cur) {
            continue;
        }
        float d = fmodf((blip_bearing(&s_blip[s_vis[k]]) - b0) * dir + 720.0f, 360.0f);
        if (d == 0.0f) { /* same bearing: order by index so every aircraft is still reached */
            d = ((k > cur) == (dir > 0)) ? 1e-3f : 360.0f - 1e-3f;
        }
        if (best < 0 || d < best_d) {
            best = k;
            best_d = d;
        }
    }
    if (best >= 0) {
        select_vis(best);
    }
}

static void set_zoom(int z)
{
    if (z < 0) {
        z = 0;
    } else if (z >= CFG_ZOOM_LEVELS) {
        z = CFG_ZOOM_LEVELS - 1;
    }
    if (z != s_zoom) {
        s_zoom = z;
        adsb_set_range_nm(range_nm());
        s_last_overlay_us = 0;
    }
}

/* ------------------------------------------------------------------------- */
/* Per-tick update                                                           */
/* ------------------------------------------------------------------------- */

static void apply_config(void)
{
    bool first = (s_cfg_gen == UINT32_MAX);
    cfg_get(&s_cfg);
    s_cfg_gen = cfg_generation();
    if (first) {
        s_zoom = s_cfg.default_zoom < CFG_ZOOM_LEVELS ? s_cfg.default_zoom : 2;
    }
    adsb_set_range_nm(range_nm());
    s_last_overlay_us = 0;

    lv_disp_rot_t rot = ROTATION[s_cfg.rotation & 3];
    if (s_disp && lv_disp_get_rotation(s_disp) != rot) {
        lv_disp_set_rotation(s_disp, rot); /* re-programs the panel and redraws everything */
    }

    /* trails are offsets from the centre: a new location invalidates them */
    if (s_cfg.lat != s_trail_lat || s_cfg.lon != s_trail_lon) {
        memset(s_trails, 0, sizeof(s_trails));
        memset(s_trail_of, -1, sizeof(s_trail_of));
        s_trail_t_us = 0;
        s_trail_lat = s_cfg.lat;
        s_trail_lon = s_cfg.lon;
    }

    /* same flat-earth projection as the aircraft positions in adsb.c */
    const double coslat = cos(s_cfg.lat * M_PI / 180.0);
    for (int i = 0; i < CFG_REF_POINTS; i++) {
        double dlon = s_cfg.ref[i].lon - s_cfg.lon;
        if (dlon > 180.0) {
            dlon -= 360.0;
        } else if (dlon < -180.0) {
            dlon += 360.0;
        }
        s_ref_x[i] = (float)(dlon * coslat * 60.0);
        s_ref_y[i] = (float)((s_cfg.ref[i].lat - s_cfg.lat) * 60.0);
    }
}

static int16_t to_cnm(float nm)
{
    float v = nm * 100.0f;
    return (int16_t)(v > 32000.0f ? 32000 : v < -32000.0f ? -32000 : lroundf(v));
}

static int trail_slot(const char *hex, int64_t now)
{
    int free_i = -1;
    int old_i = -1;
    for (int i = 0; i < TRAIL_SLOTS; i++) {
        trail_t *t = &s_trails[i];
        if (t->hex[0] && strcmp(t->hex, hex) == 0) {
            return i;
        }
        if (!t->hex[0] || now - t->seen_us > TRAIL_FORGET_US) {
            if (free_i < 0) {
                free_i = i;
            }
        } else if (old_i < 0 || t->seen_us < s_trails[old_i].seen_us) {
            old_i = i;
        }
    }
    int i = free_i >= 0 ? free_i : old_i;
    memset(&s_trails[i], 0, sizeof(s_trails[i]));
    strlcpy(s_trails[i].hex, hex, sizeof(s_trails[i].hex));
    return i;
}

/* Called for every new snapshot: map planes to trail slots and record a point per successful fetch,
 * at most one per TRAIL_INTERVAL_US so the trail length does not depend on the refresh rate. */
static void update_trails(void)
{
    const int64_t now = esp_timer_get_time();
    const bool fresh = s_snap.t_us && s_snap.t_us != s_trail_t_us;
    s_trail_t_us = s_snap.t_us;
    for (int i = 0; i < s_snap.count; i++) {
        const plane_t *p = &s_snap.planes[i];
        int k = p->hex[0] ? trail_slot(p->hex, now) : -1;
        s_trail_of[i] = (int8_t)k;
        if (k < 0) {
            continue;
        }
        trail_t *t = &s_trails[k];
        t->seen_us = now;
        if (!fresh || (t->n && now - t->last_us < TRAIL_INTERVAL_US)) {
            continue;
        }
        t->x[t->head] = to_cnm(p->x_nm);
        t->y[t->head] = to_cnm(p->y_nm);
        t->head = (t->head + 1) % TRAIL_POINTS;
        if (t->n < TRAIL_POINTS) {
            t->n++;
        }
        t->last_us = now;
    }
}

static void compute_blips(void)
{
    const float rng = range_nm();
    const float scale = R_OUT / rng;
    const int64_t now = esp_timer_get_time();
    const float dt_base = s_snap.t_us ? (float)(now - s_snap.t_us) / 1e6f : 0.0f;

    s_nvis = 0;
    for (int i = 0; i < s_snap.count; i++) {
        const plane_t *p = &s_snap.planes[i];
        blip_t *b = &s_blip[i];
        float x = p->x_nm;
        float y = p->y_nm;
        float th = ((p->has & PF_TRACK) ? p->track_deg : 0) * DEG2RAD;
        b->ux = sinf(th);
        b->uy = -cosf(th);

        /* dead reckoning between two fetches */
        if ((p->has & PF_GS) && (p->has & PF_TRACK) && !(p->has & PF_GROUND)) {
            float dt = dt_base + p->age_s;
            if (dt > 30.0f) {
                dt = 30.0f;
            }
            float d = p->gs_kt * dt / 3600.0f;
            x += b->ux * d;
            y -= b->uy * d; /* uy is screen-space (north = -y) */
        }
        b->x_nm = x;
        b->y_nm = y;
        b->visible = (x * x + y * y) <= rng * rng && !plane_hidden(p);
        if (b->visible) {
            b->x = (int16_t)lroundf(CX + x * scale);
            b->y = (int16_t)lroundf(CY - y * scale);
            s_vis[s_nvis++] = i;
        }
    }
}

/* Nearest airborne aircraft inside the alert radius and below the alert altitude (independent of the
 * display filters and the zoom level). */
static void update_alert(void)
{
    s_alert = -1;
    if (s_cfg.alert && s_snap.t_us && esp_timer_get_time() - s_snap.t_us < ALERT_MAX_AGE_US) {
        float best = s_cfg.alert_dist_nm10 / 10.0f;
        for (int i = 0; i < s_snap.count; i++) {
            const plane_t *p = &s_snap.planes[i];
            if (!(p->has & PF_ALT) || (p->has & PF_GROUND) || p->alt_baro_ft > s_cfg.alert_alt_ft) {
                continue;
            }
            float d = hypotf(s_blip[i].x_nm, s_blip[i].y_nm);
            if (d <= best) {
                best = d;
                s_alert = i;
            }
        }
    }
    s_alert_active = s_alert >= 0;
}

static void build_card(const plane_t *p, const blip_t *b)
{
    char alt[16], spd[16], vs[16], dist[16];
    size_t off = 0;

    route_info_t ri;
    route_state_t rs = (s_cfg.lookup_routes && p->flight[0]) ? adsb_get_route(p->flight, &ri) : ROUTE_NONE;
    char route[24] = "";
    if (rs == ROUTE_FOUND) {
        const char *o = ri.orig_iata[0] ? ri.orig_iata : ri.orig_icao;
        const char *d = ri.dest_iata[0] ? ri.dest_iata : ri.dest_icao;
        snprintf(route, sizeof(route), "%s>%s", o[0] ? o : "?", d[0] ? d : "?");
    } else if (rs == ROUTE_NONE && s_cfg.lookup_routes && p->flight[0]) {
        snprintf(route, sizeof(route), "...");
    } else if (rs == ROUTE_ERROR) {
        snprintf(route, sizeof(route), "n/a");
    }

    fmt_alt(alt, sizeof(alt), p);
    if (p->has & PF_GS) {
        fmt_speed(spd, sizeof(spd), p->gs_kt);
    } else {
        snprintf(spd, sizeof(spd), "-- kt");
    }
    float dist_nm = hypotf(b->x_nm, b->y_nm);
    float brg = atan2f(b->x_nm, b->y_nm) / DEG2RAD;
    if (brg < 0) {
        brg += 360.0f;
    }
    fmt_dist(dist, sizeof(dist), dist_nm);

    APP("#FFC400 %s# %s\n", plane_name(p), p->type);
    APP("%s  %s\n", p->reg[0] ? p->reg : p->hex, route);
    if (p->has & PF_TRACK) {
        APP("%s  %s  trk%03d\n", alt, spd, p->track_deg);
    } else {
        APP("%s  %s\n", alt, spd);
    }
    if (p->has & PF_VS) {
        fmt_vs(vs, sizeof(vs), p->vs_fpm);
        APP("%s  %s\n", vs, dist);
    } else {
        APP("%s\n", dist);
    }
    if (plane_emergency(p)) {
        APP("#FF4040 EMERG %s#", p->squawk);
    } else {
        APP("brg%03d  sq%s", (int)lroundf(brg), p->squawk[0] ? p->squawk : "----");
    }
    set_label(s_card_lbl, s_txt_card, sizeof(s_txt_card), s_tmp);
}

static void build_info(const plane_t *p, const blip_t *b)
{
    char t1[20], t2[20], t3[20], dist[16];
    size_t off = 0;

    route_info_t ri;
    route_state_t rs = (s_cfg.lookup_routes && p->flight[0]) ? adsb_get_route(p->flight, &ri) : ROUTE_NONE;

    APP("#FFC400 %s#  %s\n", plane_name(p), p->reg);
    APP("%s  cat %s  %s\n", p->type[0] ? p->type : "----", p->category[0] ? p->category : "-", p->hex);
    if (rs == ROUTE_FOUND) {
        APP("%s %.14s\n", ri.orig_iata[0] ? ri.orig_iata : ri.orig_icao, ri.orig_city[0] ? ri.orig_city : ri.orig_name);
        APP("%s %.14s\n", ri.dest_iata[0] ? ri.dest_iata : ri.dest_icao, ri.dest_city[0] ? ri.dest_city : ri.dest_name);
    } else if (rs == ROUTE_UNKNOWN) {
        APP("no route known\n\n");
    } else if (rs == ROUTE_ERROR) {
        APP("route lookup failed\n\n");
    } else {
        APP("%s\n\n", s_cfg.lookup_routes ? "looking up route..." : "route lookup off");
    }

    fmt_alt(t1, sizeof(t1), p);
    if (p->has & PF_ALTGEOM) {
        fmt_alt_ft(t2, sizeof(t2), p->alt_geom_ft);
        APP("baro %s  gps %s\n", t1, t2);
    } else {
        APP("baro %s\n", t1);
    }

    t1[0] = t2[0] = t3[0] = 0;
    if (p->has & PF_GS) {
        fmt_speed(t1, sizeof(t1), p->gs_kt);
    }
    if (p->has & PF_IAS) {
        fmt_speed(t2, sizeof(t2), p->ias_kt);
    }
    if (p->has & PF_TAS) {
        fmt_speed(t3, sizeof(t3), p->tas_kt);
    }
    APP("gs %s ias %s tas %s\n", t1[0] ? t1 : "--", t2[0] ? t2 : "--", t3[0] ? t3 : "--");

    APP("trk %03d  hdg ", (p->has & PF_TRACK) ? p->track_deg : 0);
    if (p->has & PF_MAGHDG) {
        APP("%03d", p->mag_hdg_deg);
    } else {
        APP("---");
    }
    if (p->has & PF_MACH) {
        APP("  M%.2f", p->mach);
    }
    APP("\n");

    if (p->has & PF_VS) {
        fmt_vs(t1, sizeof(t1), p->vs_fpm);
    } else {
        snprintf(t1, sizeof(t1), "--");
    }
    if (p->has & PF_NAVALT) {
        fmt_alt_ft(t2, sizeof(t2), p->nav_alt_ft);
    } else {
        snprintf(t2, sizeof(t2), "--");
    }
    APP("vs %s  sel %s\n", t1, t2);

    float brg = atan2f(b->x_nm, b->y_nm) / DEG2RAD;
    if (brg < 0) {
        brg += 360.0f;
    }
    fmt_dist(dist, sizeof(dist), hypotf(b->x_nm, b->y_nm));
    APP("%s  brg %03d  sq %s", dist, (int)lroundf(brg), p->squawk[0] ? p->squawk : "----");

    set_label(s_info_lbl, s_txt_info, sizeof(s_txt_info), s_tmp);
}

static void update_overlays(void)
{
    char buf[32];
    lv_color_t col = COL_GREEN;

    /* top status */
    int64_t now = esp_timer_get_time();
    if (s_alert >= 0) {
        char dist[16];
        fmt_dist(dist, sizeof(dist), hypotf(s_blip[s_alert].x_nm, s_blip[s_alert].y_nm));
        snprintf(buf, sizeof(buf), "%s %s", plane_name(&s_snap.planes[s_alert]), dist);
        col = s_blink ? COL_RED : COL_AMBER;
    } else if (s_snap.state == ADSB_ST_RATE_LIMITED) {
        /* keep the scope usable: count plus a quiet pause marker and the time until the next request */
        char wait[12];
        fmt_wait(wait, sizeof(wait), s_snap.retry_until_us - now);
        if (s_snap.t_us && s_cfg.show_count) {
            snprintf(buf, sizeof(buf), "%d ac " LV_SYMBOL_PAUSE " %s", s_nvis, wait);
        } else {
            snprintf(buf, sizeof(buf), LV_SYMBOL_PAUSE " %s", wait);
        }
        col = COL_AMBER;
    } else if (s_snap.state == ADSB_ST_NO_WIFI) {
        snprintf(buf, sizeof(buf), "no WiFi");
        col = COL_RED;
    } else if (s_snap.t_us == 0) {
        snprintf(buf, sizeof(buf), s_snap.state == ADSB_ST_HTTP_ERR ? "fetch error" : "loading...");
        col = s_snap.state == ADSB_ST_HTTP_ERR ? COL_RED : COL_AMBER;
    } else if (s_snap.state != ADSB_ST_OK || (now - s_snap.t_us) > (int64_t)(s_cfg.refresh_s * 3 + 5) * 1000000) {
        if (s_cfg.show_count) {
            snprintf(buf, sizeof(buf), "%d ac (stale)", s_nvis);
        } else {
            snprintf(buf, sizeof(buf), "stale");
        }
        col = COL_AMBER;
    } else if (s_cfg.show_count) {
        snprintf(buf, sizeof(buf), "%d ac", s_nvis);
    } else {
        buf[0] = 0; /* all well: keep the scope clean */
    }
    set_label(s_top, s_txt_top, sizeof(s_txt_top), buf);
    lv_obj_set_style_text_color(s_top, col, 0);

    /* range */
    snprintf(buf, sizeof(buf), "%g %s", range_units(), DIST_UNIT[s_cfg.units % 3]);
    set_label(s_bottom, s_txt_bottom, sizeof(s_txt_bottom), buf);

    /* selection card / details page */
    bool show_card = false;
    bool show_info = false;
    if (s_mode == MODE_SELECT) {
        int k = find_vis_by_hex();
        if (k >= 0) {
            const plane_t *p = &s_snap.planes[s_vis[k]];
            const blip_t *b = &s_blip[s_vis[k]];
            if (s_page == 0) {
                build_card(p, b);
                show_card = true;
            } else {
                build_info(p, b);
                show_info = true;
            }
        }
    }
    set_hidden(s_card, !show_card);
    set_hidden(s_info, !show_info);
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    bool dirty = false;
    if (cfg_generation() != s_cfg_gen) {
        apply_config();
        dirty = true;
    }
    uint32_t v = adsb_version();
    if (v != s_snap_ver) {
        adsb_get_snapshot(&s_snap);
        s_snap_ver = s_snap.version;
        update_trails();
        dirty = true;
    }

    int64_t now = esp_timer_get_time();
    s_sweep_deg = (float)((now / 1000) % SWEEP_PERIOD_MS) * 360.0f / SWEEP_PERIOD_MS;
    s_blink = (now / BLINK_PERIOD_US) & 1;

    compute_blips();
    update_alert();

    if (s_mode == MODE_SELECT) {
        if (find_vis_by_hex() < 0) {
            if (s_nvis > 0) {
                select_vis(0);
            } else {
                s_mode = MODE_RADAR;
            }
        }
        if (now - s_last_input_us > SELECT_TIMEOUT_US) {
            s_mode = MODE_RADAR;
            s_page = 0;
        }
    }

    if (now - s_last_overlay_us > OVERLAY_PERIOD_US) {
        s_last_overlay_us = now;
        update_overlays();
        dirty = true;
    }
    /* without the sweep nothing animates, so only redraw when something may have changed */
    if (s_cfg.sweep || dirty) {
        lv_obj_invalidate(s_radar);
    }
}

/* ------------------------------------------------------------------------- */
/* Drawing                                                                   */
/* ------------------------------------------------------------------------- */

/* The draw callback runs once per buffer strip: skip lines that do not touch the strip being drawn. */
static void draw_seg(lv_draw_ctx_t *ctx, const lv_draw_line_dsc_t *ld, int x1, int y1, int x2, int y2)
{
    const lv_area_t *clip = ctx->clip_area;
    const int w = ld->width;
    if (LV_MAX(y1, y2) + w < clip->y1 || LV_MIN(y1, y2) - w > clip->y2 || LV_MAX(x1, x2) + w < clip->x1 ||
        LV_MIN(x1, x2) - w > clip->x2) {
        return;
    }
    lv_point_t a = {x1, y1};
    lv_point_t b = {x2, y2};
    lv_draw_line(ctx, ld, &a, &b);
}

/* Trail from the oldest recorded point to the current (dead-reckoned) position, fading with age.
 * Segments with an end outside the scope are skipped. */
static void draw_trail(lv_draw_ctx_t *ctx, lv_point_t c, float scale, float rng, const trail_t *t, const blip_t *b,
                       lv_color_t col)
{
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = col;
    ld.width = 1;
    const float r2 = rng * rng;
    const int start = (t->head - t->n + TRAIL_POINTS) % TRAIL_POINTS;
    int px = 0, py = 0;
    bool prev_in = false;
    for (int k = 0; k <= t->n; k++) {
        float x = b->x_nm, y = b->y_nm;
        if (k < t->n) {
            int i = (start + k) % TRAIL_POINTS;
            x = t->x[i] / 100.0f;
            y = t->y[i] / 100.0f;
        }
        const bool in = x * x + y * y <= r2;
        const int sx = (int)lroundf(c.x + x * scale);
        const int sy = (int)lroundf(c.y - y * scale);
        if (k > 0 && in && prev_in) {
            ld.opa = (lv_opa_t)(LV_OPA_20 + (LV_OPA_70 - LV_OPA_20) * k / t->n);
            draw_seg(ctx, &ld, px, py, sx, sy);
        }
        px = sx;
        py = sy;
        prev_in = in;
    }
}

/* Line along the track to the predicted position in leader_s seconds, cut at the scope edge. */
static void draw_leader(lv_draw_ctx_t *ctx, int ox, int oy, float scale, const plane_t *p, const blip_t *b,
                        lv_color_t col)
{
    if (!s_cfg.leader_s || !(p->has & PF_GS) || !(p->has & PF_TRACK) || (p->has & PF_GROUND)) {
        return;
    }
    float len = p->gs_kt * s_cfg.leader_s / 3600.0f * scale;
    /* distance along (ux, uy) from the blip to the outer ring */
    const float dx = b->x - CX;
    const float dy = b->y - CY;
    const float du = dx * b->ux + dy * b->uy;
    const float disc = du * du - (dx * dx + dy * dy - (float)(R_OUT * R_OUT));
    if (disc > 0) {
        len = fminf(len, -du + sqrtf(disc));
    }
    if (len < 3.0f) {
        return;
    }
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = col;
    ld.width = 1;
    ld.opa = LV_OPA_60;
    const int x = ox + b->x;
    const int y = oy + b->y;
    draw_seg(ctx, &ld, x, y, (int)lroundf(x + b->ux * len), (int)lroundf(y + b->uy * len));
}

static void draw_blip(lv_draw_ctx_t *ctx, int ox, int oy, const plane_t *p, const blip_t *b, bool selected)
{
    lv_color_t col = plane_color(p);
    float size = (p->has & PF_GROUND) ? 4.0f : 7.0f;
    if (selected) {
        if (!s_cfg.alt_colors) {
            col = COL_AMBER; /* with altitude colours the ring alone marks the selection */
        }
        size = 9.0f;
    }

    const int x = ox + b->x;
    const int y = oy + b->y;

    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = col;
    rd.bg_opa = LV_OPA_COVER;
    rd.border_width = 0;

    if (p->has & PF_TRACK) {
        const float rx = -b->uy;
        const float ry = b->ux;
        lv_point_t pts[3] = {
            {(lv_coord_t)lroundf(x + b->ux * size), (lv_coord_t)lroundf(y + b->uy * size)},
            {(lv_coord_t)lroundf(x - b->ux * size * 0.6f + rx * size * 0.6f),
             (lv_coord_t)lroundf(y - b->uy * size * 0.6f + ry * size * 0.6f)},
            {(lv_coord_t)lroundf(x - b->ux * size * 0.6f - rx * size * 0.6f),
             (lv_coord_t)lroundf(y - b->uy * size * 0.6f - ry * size * 0.6f)},
        };
        lv_draw_polygon(ctx, &rd, pts, 3);
    } else {
        rd.radius = LV_RADIUS_CIRCLE;
        lv_area_t a = {x - 2, y - 2, x + 2, y + 2};
        lv_draw_rect(ctx, &rd, &a);
    }

    if (selected) {
        lv_draw_arc_dsc_t ad;
        lv_draw_arc_dsc_init(&ad);
        ad.color = COL_AMBER;
        ad.width = 2;
        ad.opa = LV_OPA_COVER;
        lv_point_t c = {x, y};
        lv_draw_arc(ctx, &ad, &c, 13, 0, 360);
    }
}

static void draw_text(lv_draw_ctx_t *ctx, const lv_font_t *font, lv_color_t col, int x, int y, int w, const char *txt)
{
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = font;
    ld.color = col;
    lv_area_t a = {x, y, x + w, y + font->line_height};
    lv_draw_label(ctx, &ld, &a, txt, NULL);
}

/* Label next to an aircraft: callsign, then type and altitude on a second line, as configured. */
static void draw_label(lv_draw_ctx_t *ctx, int x, int y, const plane_t *p)
{
    const uint8_t f = s_cfg.label_fields;
    const lv_font_t *font = &lv_font_montserrat_10;
    if (f & LABEL_CALLSIGN) {
        draw_text(ctx, font, COL_TEXT, x, y, 70, plane_name(p));
        y += font->line_height;
    }
    char line[24] = "";
    char alt[12];
    if ((f & LABEL_TYPE) && p->type[0]) {
        strlcpy(line, p->type, sizeof(line));
    }
    if ((f & LABEL_ALT) && (p->has & PF_ALT)) {
        fmt_alt_ft(alt, sizeof(alt), p->alt_baro_ft);
        if (line[0]) {
            strlcat(line, " ", sizeof(line));
        }
        strlcat(line, alt, sizeof(line));
    }
    if (line[0]) {
        draw_text(ctx, font, COL_LABEL2, x, y, 70, line);
    }
}

static void radar_draw_cb(lv_event_t *e)
{
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_obj_t *obj = lv_event_get_target(e);
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    const int ox = coords.x1;
    const int oy = coords.y1;
    lv_point_t c = {ox + CX, oy + CY};

    /* scope background */
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = COL_BG;
    rd.bg_opa = LV_OPA_COVER;
    rd.radius = LV_RADIUS_CIRCLE;
    rd.border_width = 0;
    lv_area_t disc = {c.x - R_OUT, c.y - R_OUT, c.x + R_OUT, c.y + R_OUT};
    lv_draw_rect(ctx, &rd, &disc);

    /* sweep trail (pie slices fading out behind the beam) */
    lv_draw_arc_dsc_t ad;
    lv_draw_arc_dsc_init(&ad);
    ad.color = COL_GREEN;
    ad.width = R_OUT;
    for (int k = 0; s_cfg.sweep && k < TRAIL_SLICES; k++) {
        float a_end = s_sweep_deg - 90.0f - k * TRAIL_STEP_DEG;
        float a_start = a_end - TRAIL_STEP_DEG;
        int e_deg = ((int)lroundf(a_end) % 360 + 360) % 360;
        int s_deg = ((int)lroundf(a_start) % 360 + 360) % 360;
        ad.opa = (lv_opa_t)(LV_OPA_40 - k * (LV_OPA_40 / (TRAIL_SLICES + 1)));
        lv_draw_arc(ctx, &ad, &c, R_OUT, s_deg, e_deg);
    }

    /* range rings and crosshair */
    lv_draw_arc_dsc_init(&ad);
    ad.width = 1;
    ad.opa = LV_OPA_COVER;
    ad.color = COL_RING;
    for (int k = 1; k < 4; k++) {
        lv_draw_arc(ctx, &ad, &c, R_OUT * k / 4, 0, 360);
    }
    ad.color = (s_alert >= 0 && s_blink) ? COL_RED : COL_RING_OUT;
    ad.width = 2;
    lv_draw_arc(ctx, &ad, &c, R_OUT, 0, 360);

    const float rng = range_nm();
    const float scale = R_OUT / rng;
    if (s_cfg.alert) {
        const int r = (int)lroundf(s_cfg.alert_dist_nm10 / 10.0f * scale);
        if (r >= 3 && r < R_OUT) {
            ad.color = COL_RED;
            ad.width = 1;
            ad.opa = LV_OPA_50;
            lv_draw_arc(ctx, &ad, &c, r, 0, 360);
            ad.opa = LV_OPA_COVER;
        }
    }

    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = COL_RING;
    ld.width = 1;
    ld.opa = LV_OPA_COVER;
    lv_point_t p1 = {c.x - R_OUT, c.y};
    lv_point_t p2 = {c.x + R_OUT, c.y};
    lv_draw_line(ctx, &ld, &p1, &p2);
    p1 = (lv_point_t){c.x, c.y - R_OUT};
    p2 = (lv_point_t){c.x, c.y + R_OUT};
    lv_draw_line(ctx, &ld, &p1, &p2);

    /* sweep beam */
    if (s_cfg.sweep) {
        ld.color = COL_GREEN;
        ld.width = 2;
        float th = s_sweep_deg * DEG2RAD;
        p1 = c;
        p2 = (lv_point_t){(lv_coord_t)lroundf(c.x + sinf(th) * R_OUT), (lv_coord_t)lroundf(c.y - cosf(th) * R_OUT)};
        lv_draw_line(ctx, &ld, &p1, &p2);
    }

    /* reference points: small diamond and name */
    ld.color = COL_REF;
    ld.width = 1;
    for (int i = 0; i < CFG_REF_POINTS; i++) {
        if (!s_cfg.ref[i].name[0] || s_ref_x[i] * s_ref_x[i] + s_ref_y[i] * s_ref_y[i] > rng * rng) {
            continue;
        }
        const int x = (int)lroundf(c.x + s_ref_x[i] * scale);
        const int y = (int)lroundf(c.y - s_ref_y[i] * scale);
        draw_seg(ctx, &ld, x, y - 3, x + 3, y);
        draw_seg(ctx, &ld, x + 3, y, x, y + 3);
        draw_seg(ctx, &ld, x, y + 3, x - 3, y);
        draw_seg(ctx, &ld, x - 3, y, x, y - 3);
        draw_text(ctx, &lv_font_montserrat_10, COL_REF, x + 5, y - 6, 50, s_cfg.ref[i].name);
    }

    /* trails and leader lines below all symbols */
    for (int k = 0; k < s_nvis; k++) {
        const int i = s_vis[k];
        const plane_t *p = &s_snap.planes[i];
        const lv_color_t col = plane_color(p);
        if (s_cfg.trails && s_trail_of[i] >= 0) {
            draw_trail(ctx, c, scale, rng, &s_trails[s_trail_of[i]], &s_blip[i], col);
        }
        draw_leader(ctx, ox, oy, scale, p, &s_blip[i], col);
    }

    /* aircraft: selected one last so it ends up on top */
    int sel = (s_mode == MODE_SELECT) ? find_vis_by_hex() : -1;
    bool labels = s_nvis <= LABELS_MAX_PLANES;
    for (int k = 0; k < s_nvis; k++) {
        if (k == sel) {
            continue;
        }
        const plane_t *p = &s_snap.planes[s_vis[k]];
        const blip_t *b = &s_blip[s_vis[k]];
        draw_blip(ctx, ox, oy, p, b, false);
        if (labels && !(p->has & PF_GROUND)) {
            draw_label(ctx, ox + b->x + 8, oy + b->y - 6, p);
        }
    }
    if (sel >= 0) {
        const plane_t *p = &s_snap.planes[s_vis[sel]];
        const blip_t *b = &s_blip[s_vis[sel]];
        draw_blip(ctx, ox, oy, p, b, true);
    }

    /* own position */
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = lv_color_white();
    rd.bg_opa = LV_OPA_COVER;
    rd.radius = LV_RADIUS_CIRCLE;
    lv_area_t me = {c.x - 2, c.y - 2, c.x + 2, c.y + 2};
    lv_draw_rect(ctx, &rd, &me);
}

/* ------------------------------------------------------------------------- */
/* Construction / public API                                                 */
/* ------------------------------------------------------------------------- */

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t col, int x, int y, int w)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(l, w);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, "");
    return l;
}

/* Text on the outer ring at height ring_y, in a black pill that cuts a gap into the ring. With a 15 px line,
 * 1 px vertical padding and the glyphs sitting above the baseline, top = ring_y - 9 centres them on it. The
 * width follows the text and stays centred. */
static lv_obj_t *make_ring_tag(lv_obj_t *parent, int ring_y, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(l, COL_RING_OUT, 0);
    lv_obj_set_style_bg_color(l, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(l, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(l, 6, 0);
    lv_obj_set_style_pad_ver(l, 1, 0);
    lv_label_set_text(l, txt);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, ring_y - 9);
    return l;
}

static lv_obj_t *make_panel(lv_obj_t *parent, int x, int y, int w, int h, int radius, lv_opa_t opa)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_set_style_radius(o, radius, 0);
    return o;
}

void radar_ui_init(lv_disp_t *disp)
{
    lvgl_port_lock(0);

    s_disp = disp;
    apply_config();

    lv_obj_t *scr = lv_disp_get_scr_act(disp);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_radar = lv_obj_create(scr);
    lv_obj_remove_style_all(s_radar);
    lv_obj_set_pos(s_radar, 0, 0);
    lv_obj_set_size(s_radar, BOARD_LCD_W, BOARD_LCD_H);
    lv_obj_clear_flag(s_radar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_radar, radar_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    s_top = make_label(scr, &lv_font_montserrat_12, COL_GREEN, 50, 24, 140);
    /* north marker and range sit on the outer ring (y = CY -/+ R_OUT) */
    make_ring_tag(scr, CY - R_OUT, "N");
    s_bottom = make_ring_tag(scr, CY + R_OUT, "");

    /* selected aircraft card: bottom part of the round screen */
    /* The large corner radius keeps the bottom corners inside the round display (R=120):
     * the farthest corner point is ~113 px from the centre. */
    s_card = make_panel(scr, 30, 118, 180, 90, 30, LV_OPA_80);
    lv_obj_set_style_border_width(s_card, 1, 0);
    lv_obj_set_style_border_color(s_card, COL_AMBER, 0);
    lv_obj_set_style_border_opa(s_card, LV_OPA_60, 0);
    s_card_lbl = make_label(s_card, &lv_font_montserrat_12, COL_TEXT, 3, 5, 172);
    lv_label_set_recolor(s_card_lbl, true);
    lv_obj_set_style_text_line_space(s_card_lbl, 2, 0);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);

    /* full details page */
    s_info = make_panel(scr, 0, 0, BOARD_LCD_W, BOARD_LCD_H, 0, LV_OPA_90);
    s_info_lbl = make_label(s_info, &lv_font_montserrat_12, COL_TEXT, 20, 38, 200);
    lv_label_set_recolor(s_info_lbl, true);
    lv_obj_set_style_text_line_space(s_info_lbl, 2, 0);
    lv_obj_add_flag(s_info, LV_OBJ_FLAG_HIDDEN);

    /* full-screen message (boot / setup) */
    s_msg = make_panel(scr, 0, 0, BOARD_LCD_W, BOARD_LCD_H, 0, LV_OPA_COVER);
    s_msg_lbl = make_label(s_msg, &lv_font_montserrat_14, COL_GREEN, 25, 60, 190);
    lv_label_set_long_mode(s_msg_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);

    s_last_input_us = esp_timer_get_time();
    lv_timer_create(tick_cb, TICK_MS, NULL);

    lvgl_port_unlock();
}

bool radar_ui_input(input_evt_t evt)
{
    bool consumed = true;
    if (!lvgl_port_lock(1000)) {
        return true;
    }
    if (!s_msg_visible) {
        s_last_input_us = esp_timer_get_time();
        switch (evt) {
        case INPUT_CW:
        case INPUT_CCW: {
            int dir = (evt == INPUT_CW) ? 1 : -1;
            if (s_mode == MODE_RADAR) {
                set_zoom(s_zoom - dir); /* clockwise = zoom in */
            } else {
                cycle_selection(dir);
            }
            break;
        }
        case INPUT_PRESS:
            if (s_mode == MODE_RADAR) {
                if (s_nvis > 0) {
                    s_mode = MODE_SELECT;
                    s_sel_hex[0] = 0;
                    select_vis(0);
                }
            } else {
                s_mode = MODE_RADAR;
                s_page = 0;
            }
            break;
        case INPUT_LONG_PRESS:
            if (s_mode == MODE_SELECT) {
                s_page ^= 1;
            } else {
                consumed = false;
            }
            break;
        }
        s_last_overlay_us = 0; /* redraw overlays on the next tick */
    }
    if (!s_msg_visible) {
        tick_cb(NULL); /* apply the change now instead of waiting for the next timer period */
    }
    lvgl_port_unlock();
    return consumed;
}

bool radar_ui_alert_active(void)
{
    return s_alert_active;
}

void radar_ui_show_message(const char *msg)
{
    if (!lvgl_port_lock(500)) {
        return;
    }
    if (msg) {
        lv_label_set_text(s_msg_lbl, msg);
        lv_obj_clear_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
        s_msg_visible = true;
    } else {
        lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
        s_msg_visible = false;
    }
    lvgl_port_unlock();
}
