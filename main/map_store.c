#include "map_store.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "map";

#define MAP_PART_SUBTYPE 0x40
#define SECTOR 4096

_Static_assert(sizeof(map_hdr_t) == 36, "map header layout");
_Static_assert(sizeof(map_poly_t) == 4, "map polyline layout");

static const esp_partition_t *s_part;
static SemaphoreHandle_t s_lock;
static const void *s_mapped;
static esp_partition_mmap_handle_t s_mmap;
static bool s_valid;
static volatile uint32_t s_gen;

/* upload state (holds s_lock from begin to end/abort) */
static size_t s_total, s_pos, s_erased;
static uint8_t s_hdr[sizeof(map_hdr_t)];
static uint32_t s_crc;

/* Walks the polyline records: they must exactly fill the payload and add up to the header counts. */
static bool payload_ok(const map_hdr_t *h, const uint8_t *payload)
{
    size_t off = 0;
    uint32_t points = 0;
    for (uint32_t i = 0; i < h->polylines; i++) {
        if (off + sizeof(map_poly_t) > h->payload_len) {
            return false;
        }
        const map_poly_t *pl = (const map_poly_t *)(payload + off);
        off += sizeof(map_poly_t) + (size_t)pl->n * 4;
        points += pl->n;
        if (off > h->payload_len || pl->layer > 7) {
            return false;
        }
    }
    return off == h->payload_len && points == h->points;
}

static bool header_ok(const map_hdr_t *h)
{
    return h->magic == MAP_MAGIC && h->format == MAP_FORMAT && h->payload_len <= s_part->size - sizeof(*h);
}

/* Map the partition and check the stored data. Called with s_lock held (or before anyone else runs). */
static void attach(void)
{
    s_valid = false;
    if (esp_partition_mmap(s_part, 0, s_part->size, ESP_PARTITION_MMAP_DATA, &s_mapped, &s_mmap) != ESP_OK) {
        s_mapped = NULL;
        ESP_LOGE(TAG, "mmap failed");
        return;
    }
    const map_hdr_t *h = s_mapped;
    if (!header_ok(h)) {
        ESP_LOGI(TAG, "no map stored");
        return;
    }
    const uint8_t *payload = (const uint8_t *)(h + 1);
    if (esp_rom_crc32_le(0, payload, h->payload_len) != h->crc || !payload_ok(h, payload)) {
        ESP_LOGW(TAG, "stored map is corrupt");
        return;
    }
    s_valid = true;
    ESP_LOGI(TAG, "map for %.3f, %.3f: %lu lines, %lu points", h->lat, h->lon, (unsigned long)h->polylines,
             (unsigned long)h->points);
}

static void detach(void)
{
    if (s_mapped) {
        esp_partition_munmap(s_mmap);
        s_mapped = NULL;
    }
    s_valid = false;
}

esp_err_t map_store_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "lock");
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, MAP_PART_SUBTYPE, "map");
    if (!s_part) {
        ESP_LOGW(TAG, "no map partition (old partition table?)");
        return ESP_ERR_NOT_FOUND;
    }
    attach();
    return ESP_OK;
}

size_t map_store_capacity(void)
{
    return s_part ? s_part->size : 0;
}

const map_hdr_t *map_store_lock(void)
{
    if (!s_lock || xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return NULL;
    }
    if (!s_valid) {
        xSemaphoreGive(s_lock);
        return NULL;
    }
    return s_mapped;
}

void map_store_unlock(void)
{
    xSemaphoreGive(s_lock);
}

uint32_t map_store_generation(void)
{
    return s_gen;
}

esp_err_t map_store_begin(size_t total_len)
{
    ESP_RETURN_ON_FALSE(s_part, ESP_ERR_NOT_FOUND, TAG, "no partition");
    ESP_RETURN_ON_FALSE(total_len > sizeof(map_hdr_t) && total_len <= s_part->size, ESP_ERR_INVALID_SIZE, TAG,
                        "size");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    detach(); /* no reads from flash that is about to change */
    s_total = total_len;
    s_pos = 0;
    s_crc = 0;
    s_erased = 0;
    esp_err_t err = esp_partition_erase_range(s_part, 0, SECTOR); /* the old header goes first */
    if (err == ESP_OK) {
        s_erased = SECTOR;
    } else {
        map_store_abort();
    }
    return err;
}

esp_err_t map_store_write(const void *data, size_t len)
{
    const uint8_t *d = data;
    if (s_pos + len > s_total) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* the header is kept in RAM and written last, so a partial upload never looks valid */
    while (len && s_pos < sizeof(s_hdr)) {
        s_hdr[s_pos++] = *d++;
        len--;
    }
    if (!len) {
        return ESP_OK;
    }
    while (s_erased < s_pos + len) { /* erase as we go: no long stall up front */
        ESP_RETURN_ON_ERROR(esp_partition_erase_range(s_part, s_erased, SECTOR), TAG, "erase");
        s_erased += SECTOR;
    }
    ESP_RETURN_ON_ERROR(esp_partition_write(s_part, s_pos, d, len), TAG, "write");
    s_crc = esp_rom_crc32_le(s_crc, d, len);
    s_pos += len;
    return ESP_OK;
}

esp_err_t map_store_end(void)
{
    esp_err_t err = ESP_ERR_INVALID_SIZE;
    map_hdr_t h;
    memcpy(&h, s_hdr, sizeof(h));
    if (s_pos == s_total) {
        err = ESP_ERR_INVALID_CRC;
        if (header_ok(&h) && h.payload_len == s_total - sizeof(h) && h.crc == s_crc) {
            err = esp_partition_write(s_part, 0, s_hdr, sizeof(s_hdr));
        }
    }
    attach(); /* also re-checks the record structure */
    if (err == ESP_OK && !s_valid) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err != ESP_OK) {
        detach();
        esp_partition_erase_range(s_part, 0, SECTOR);
        attach();
    }
    s_gen++;
    xSemaphoreGive(s_lock);
    return err;
}

void map_store_abort(void)
{
    esp_partition_erase_range(s_part, 0, SECTOR);
    attach();
    s_gen++;
    xSemaphoreGive(s_lock);
}

esp_err_t map_store_clear(void)
{
    ESP_RETURN_ON_FALSE(s_part, ESP_ERR_NOT_FOUND, TAG, "no partition");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    detach();
    esp_err_t err = esp_partition_erase_range(s_part, 0, SECTOR);
    attach();
    s_gen++;
    xSemaphoreGive(s_lock);
    return err;
}
