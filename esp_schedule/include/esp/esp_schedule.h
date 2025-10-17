/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** This is the default include file for the esp_schedule component. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Use ESP_SCHEDULE_RETURN_TYPE as the return type for the esp_schedule component. */
#include <esp_err.h>
#define ESP_SCHEDULE_RETURN_TYPE esp_err_t
#define ESP_SCHEDULE_RET_OK ESP_OK
#define ESP_SCHEDULE_RET_FAIL ESP_FAIL
#define ESP_SCHEDULE_RET_NO_MEM ESP_ERR_NO_MEM
#define ESP_SCHEDULE_RET_INVALID_ARG ESP_ERR_INVALID_ARG
#define ESP_SCHEDULE_RET_INVALID_STATE ESP_ERR_INVALID_STATE

/** Use esp_log.h for logging. */
#include <esp_log.h>
#define ESP_SCHEDULE_LOGE(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#define ESP_SCHEDULE_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define ESP_SCHEDULE_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define ESP_SCHEDULE_LOGD(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)
#define ESP_SCHEDULE_LOGV(tag, fmt, ...) ESP_LOGV(tag, fmt, ##__VA_ARGS__)

#include "esp_schedule_untyped.h"

#ifdef __cplusplus
}
#endif
