/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "glue_timer.h"
#include "esp_schedule.h"

/* NVS support */
#if defined(CONFIG_ESP_SCHEDULE_ENABLE_NVS) && CONFIG_ESP_SCHEDULE_ENABLE_NVS
#define ESP_SCHEDULE_NVS_ENABLED 1
#else
#define ESP_SCHEDULE_NVS_ENABLED 0
#endif

typedef struct esp_schedule {
    char name[MAX_SCHEDULE_NAME_LEN + 1];
    /* List of triggers associated with this schedule. We deep-copy from config. */
    esp_schedule_trigger_list_t triggers;
    uint32_t next_scheduled_time_diff;
    time_t next_scheduled_time_utc;
    esp_schedule_timer_handle_t timer;
    esp_schedule_trigger_cb_t trigger_cb;
    esp_schedule_timestamp_cb_t timestamp_cb;
    void *priv_data;
    esp_schedule_validity_t validity;
} esp_schedule_t;

#if ESP_SCHEDULE_NVS_ENABLED
/* On-disk format version for the persisted schedule blob. Bump whenever the
 * layout of esp_schedule_persistent_t or esp_schedule_trigger_t changes in a
 * way that is not backward compatible, so stale blobs are rejected (or
 * migrated) on read. */
#define ESP_SCHEDULE_NVS_FORMAT_VERSION 2

/* Persistent header for NVS storage. The trigger list is stored immediately
 * after this header, followed by any private data. Runtime-only and
 * runtime-derived fields (live pointers, timer handle, callbacks, and the
 * next-fire countdown which is recomputed on every arm) are intentionally
 * excluded: persisting them is meaningless across reboots and makes the format
 * depend on pointer width and padding. */
typedef struct esp_schedule_persistent {
    uint8_t version;        /* ESP_SCHEDULE_NVS_FORMAT_VERSION */
    uint8_t trigger_count;  /* number of triggers stored after this header */
    uint16_t trigger_size;  /* sizeof(esp_schedule_trigger_t) when written */
    char name[MAX_SCHEDULE_NAME_LEN + 1];
    time_t next_scheduled_time_utc;
    esp_schedule_validity_t validity;
} esp_schedule_persistent_t;
#endif /* ESP_SCHEDULE_NVS_ENABLED */

#ifdef __cplusplus
extern "C" {
#endif

#if ESP_SCHEDULE_NVS_ENABLED
ESP_SCHEDULE_RETURN_TYPE esp_schedule_nvs_add(esp_schedule_t *schedule);
ESP_SCHEDULE_RETURN_TYPE esp_schedule_nvs_remove(esp_schedule_t *schedule);
ESP_SCHEDULE_RETURN_TYPE esp_schedule_nvs_remove_all(void);
esp_schedule_handle_t *esp_schedule_nvs_get_all(uint8_t *schedule_count);
bool esp_schedule_nvs_is_enabled(void);
ESP_SCHEDULE_RETURN_TYPE esp_schedule_nvs_init(char *nvs_partition, esp_schedule_priv_data_callbacks_t *priv_data_callbacks);

/* Free private data that the library loaded from NVS via the on_load callback
 * but that never reached the application (e.g. an expired schedule deleted
 * during init). Invokes the registered on_free callback if any; a no-op
 * otherwise. Must not be used on application-owned private data. */
void esp_schedule_nvs_free_loaded_priv_data(void *priv_data);
#endif /* ESP_SCHEDULE_NVS_ENABLED */

/* Returns true if a one-shot trigger has already fired and must not be
 * recomputed to a future occurrence. Exposed for unit testing. */
bool esp_schedule_trigger_fired_and_done(const esp_schedule_trigger_t *trigger, time_t now);

/* Unified date-based next occurrence calculation. Returns true and sets
 * *next_time to the next valid time matching all provided constraints.
 * Shared across implementation files and exposed for unit testing. */
bool esp_schedule_get_next_date_time(
    time_t now,
    uint16_t minutes_since_midnight,
    uint8_t days_of_week_mask,
    uint8_t day_of_month,
    uint16_t months_of_year_mask,
    uint16_t year,
    const esp_schedule_validity_t *validity,
    time_t *next_time
);

#if CONFIG_ESP_SCHEDULE_ENABLE_DAYLIGHT
time_t esp_schedule_calc_solar_time_for_time_utc(
    bool is_sunrise,
    time_t time_utc,
    double latitude,
    double longitude,
    int offset_minutes
);

time_t esp_schedule_get_next_valid_solar_time(
    time_t now,
    const esp_schedule_trigger_t *trigger,
    const esp_schedule_validity_t *validity,
    const char *schedule_name
);
#endif /* CONFIG_ESP_SCHEDULE_ENABLE_DAYLIGHT */

#ifdef __cplusplus
}
#endif
