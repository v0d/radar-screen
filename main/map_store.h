#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Map background data, built by the settings page from Natural Earth and stored in the "map" partition.
 * All values little-endian. Points are offsets from the centre the map was built for, in 1/50 nm
 * (x east, y north), using the same flat-earth projection as the aircraft positions. */
#define MAP_MAGIC 0x50414D52u /* "RMAP" */
#define MAP_FORMAT 1
#define MAP_UNITS_PER_NM 50

typedef struct {
    uint32_t magic;
    uint16_t format;
    uint16_t reserved;
    float lat;
    float lon;
    uint32_t polylines;
    uint32_t points;
    uint32_t payload_len; /* bytes after the header */
    uint32_t crc;         /* CRC-32 (zlib) of the payload */
    uint32_t reserved2;
} map_hdr_t;

/* Payload: polylines one after another, each a map_poly_t followed by n (x, y) int16 pairs. */
typedef struct {
    uint8_t layer; /* bit index of MAP_L_* */
    uint8_t reserved;
    uint16_t n;
} map_poly_t;

esp_err_t map_store_init(void);

/* Capacity of the map partition in bytes (0 without one). */
size_t map_store_capacity(void);

/* Readers: the header (payload follows it) stays valid until map_store_unlock(). Returns NULL without
 * valid data or while an upload is in progress. Never blocks. */
const map_hdr_t *map_store_lock(void);
void map_store_unlock(void);

/* Changes with every completed upload or clear. */
uint32_t map_store_generation(void);

/* Upload: begin, write the whole file sequentially, end. end() validates and only then activates the data;
 * on any error the previous map is gone and none is active. */
esp_err_t map_store_begin(size_t total_len);
esp_err_t map_store_write(const void *data, size_t len);
esp_err_t map_store_end(void);
void map_store_abort(void);

esp_err_t map_store_clear(void);
