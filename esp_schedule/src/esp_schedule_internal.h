/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "glue_timer.h"
#include <esp_schedule.h>

typedef struct esp_schedule {
    char name[MAX_SCHEDULE_NAME_LEN + 1];
    esp_schedule_trigger_t trigger;
    uint32_t next_scheduled_time_diff;
    esp_schedule_timer_handle_t timer;
    esp_schedule_trigger_cb_t trigger_cb;
    esp_schedule_timestamp_cb_t timestamp_cb;
    void *priv_data;
    esp_schedule_validity_t validity;
} esp_schedule_t;

ESP_SCHEDULE_RETURN_TYPE esp_schedule_nvs_add(esp_schedule_t *schedule);
ESP_SCHEDULE_RETURN_TYPE esp_schedule_nvs_remove(esp_schedule_t *schedule);
ESP_SCHEDULE_RETURN_TYPE esp_schedule_nvs_remove_all(void);
esp_schedule_handle_t *esp_schedule_nvs_get_all(uint8_t *schedule_count);
bool esp_schedule_nvs_is_enabled(void);
ESP_SCHEDULE_RETURN_TYPE esp_schedule_nvs_init(char *nvs_partition);
