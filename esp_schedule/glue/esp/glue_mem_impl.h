/**
 * @file glue_mem_impl.h
 * @brief Implementation of the memory allocation glue layer for ESP-IDF.
 */

#pragma once

/** Use esp_rmaker_utils.h for memory allocation. */
#include <esp_rmaker_utils.h>
#define ESP_SCHEDULE_MALLOC(size) MEM_ALLOC_EXTRAM(size)
#define ESP_SCHEDULE_CALLOC(num, size) MEM_CALLOC_EXTRAM(num, size)
#define ESP_SCHEDULE_REALLOC(ptr, size) MEM_REALLOC_EXTRAM(ptr, size)
#define ESP_SCHEDULE_FREE(ptr) free(ptr)