/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <inttypes.h>
#include "glue_time.h"
#include "glue_log.h"
#include "glue_mem.h"
#include "esp_daylight.h"
#include "esp_schedule_internal.h"

static const char *TAG = "esp_schedule";

#define SECONDS_TILL_2020 ((2020 - 1970) * 365 * 24 * 3600)
#define MINUTES_IN_DAY (60 * 24)

static bool init_done = false;

// Forward declarations for static functions
static void esp_schedule_common_timer_cb(void *priv_data);
static ESP_SCHEDULE_RETURN_TYPE esp_schedule_start_timer(esp_schedule_t *schedule);

/*
 * Unified date-based next occurrence calculation.
 * Returns true and sets *next_time to the next valid time that matches all provided constraints.
 * - now: current time
 * - minutes_since_midnight: target minutes in day [0, 24*60)
 * - days_of_week_mask: bitmask Monday=bit0 .. Sunday=bit6; 0 => any day
 * - day_of_month: 1..31; 0 => any day
 * - months_of_year_mask: bitmask January=bit0 .. December=bit11; 0 => any month
 * - year: 4-digit year (e.g., 2025); 0 => any year
 * - validity: optional window [start,end]; if provided, the returned time will be within this window
 */
bool esp_schedule_get_next_date_time(time_t now,
                                     uint16_t minutes_since_midnight,
                                     uint8_t days_of_week_mask,
                                     uint8_t day_of_month,
                                     uint16_t months_of_year_mask,
                                     uint16_t year,
                                     const esp_schedule_validity_t *validity,
                                     time_t *next_time)
{
    if (next_time == NULL) {
        return false;
    }
    /* If the validity window opens in the future, start the search there rather
     * than walking day-by-day from now. The month-attempt cap (~25 months) would
     * otherwise be exhausted before reaching a far-future start_time, causing the
     * schedule to silently never fire. We evaluate the first candidate day
     * unconditionally (force_include_first) and let the exact-instant
     * ">= start_time" validity check below decide whether it qualifies. That
     * avoids seeding need_next_occurrence from a wall-clock-of-day comparison,
     * which could skip the first valid day across a DST transition at the
     * window boundary. */
    bool force_include_first = false;
    if (validity != NULL && validity->start_time > now) {
        now = validity->start_time;
        force_include_first = true;
    }
    struct tm current_tm = {0};
    localtime_r(&now, &current_tm);
    struct tm candidate_tm = current_tm;

    uint32_t current_seconds_since_midnight = (uint32_t)(current_tm.tm_hour * 3600 + current_tm.tm_min * 60 + current_tm.tm_sec);
    uint32_t target_seconds_since_midnight = (uint32_t)minutes_since_midnight * 60U;

    bool need_next_occurrence = (current_seconds_since_midnight >= target_seconds_since_midnight);
    if (force_include_first) {
        /* Do not skip the window-open day; the ">= start_time" check gates it. */
        need_next_occurrence = false;
    }

    if (year != 0) {
        int target_year = (int)year - 1900;
        if (current_tm.tm_year > target_year) {
            *next_time = 0;
            return false;
        } else if (current_tm.tm_year < target_year) {
            candidate_tm.tm_year = target_year;
            candidate_tm.tm_mon = 0;
            candidate_tm.tm_mday = 1;
            need_next_occurrence = false;
        }
    }

    candidate_tm.tm_isdst = -1;
    time_t candidate_time = mktime(&candidate_tm);
    localtime_r(&candidate_time, &candidate_tm);

    for (int month_attempts = 0; month_attempts < 25; month_attempts++) {
        bool month_valid = true;
        if (months_of_year_mask != 0) {
            uint16_t month_bit = (uint16_t)(1U << candidate_tm.tm_mon);
            month_valid = (month_bit & months_of_year_mask) != 0;
        }

        if (!month_valid) {
            do {
                candidate_tm.tm_mon++;
                if (candidate_tm.tm_mon >= 12) {
                    candidate_tm.tm_mon = 0;
                    candidate_tm.tm_year++;
                }
                if (year != 0 && candidate_tm.tm_year > ((int)year - 1900)) {
                    *next_time = 0;
                    return false;
                }
                uint16_t month_bit = (uint16_t)(1U << candidate_tm.tm_mon);
                month_valid = (month_bit & months_of_year_mask) != 0;
            } while (!month_valid);

            candidate_tm.tm_mday = 1;
            candidate_tm.tm_isdst = -1;
            candidate_time = mktime(&candidate_tm);
            localtime_r(&candidate_time, &candidate_tm);
            need_next_occurrence = false;
        }

        int days_in_month = 31; /* bounded by normalization */
        for (int day_attempts = 0; day_attempts < days_in_month; day_attempts++) {
            bool day_matches = true;
            if (days_of_week_mask != 0 || day_of_month != 0) {
                day_matches = false;
                if (days_of_week_mask != 0) {
                    uint8_t day_of_week_index = (uint8_t)((candidate_tm.tm_wday + 6) % 7); /* Sunday=0 -> Monday=0 */
                    uint8_t day_bit = (uint8_t)(1U << day_of_week_index);
                    if ((day_bit & days_of_week_mask) != 0) {
                        day_matches = true;
                    }
                }
                if (!day_matches && day_of_month != 0) {
                    if (candidate_tm.tm_mday == day_of_month) {
                        day_matches = true;
                    }
                }
            }

            if (day_matches) {
                if (month_attempts == 0 && day_attempts == 0 && need_next_occurrence) {
                    /* skip today due to time passed */
                } else {
                    candidate_tm.tm_hour = (int)(minutes_since_midnight / 60U);
                    candidate_tm.tm_min = (int)(minutes_since_midnight % 60U);
                    candidate_tm.tm_sec = 0;
                    /* Let mktime resolve DST for the target local time. It uses
                     * tm_isdst to pick the correct epoch, so no manual +/-3600
                     * correction is needed (and applying one double-corrects). */
                    candidate_tm.tm_isdst = -1;
                    time_t result_time = mktime(&candidate_tm);

                    if (year != 0) {
                        struct tm check_tm;
                        localtime_r(&result_time, &check_tm);
                        if (check_tm.tm_year != ((int)year - 1900)) {
                            *next_time = 0;
                            return false;
                        }
                    }

                    if (validity && validity->end_time != 0 && result_time > validity->end_time) {
                        *next_time = 0;
                        return false;
                    }
                    if (!validity || validity->start_time == 0 || result_time >= validity->start_time) {
                        *next_time = result_time;
                        return true;
                    }
                    /* else fall through to search next valid day */
                }
            }

            /* Advance to the next calendar day. Incrementing tm_mday and
             * re-normalizing via mktime is DST-safe; adding a fixed 86400
             * seconds skips or repeats a day across DST transitions. */
            int prev_mon = candidate_tm.tm_mon;
            candidate_tm.tm_mday++;
            candidate_tm.tm_isdst = -1;
            candidate_time = mktime(&candidate_tm);
            localtime_r(&candidate_time, &candidate_tm);
            need_next_occurrence = false;
            if (candidate_tm.tm_mon != prev_mon) {
                break;
            }
            if (year != 0 && candidate_tm.tm_year > ((int)year - 1900)) {
                *next_time = 0;
                return false;
            }
        }
    }

    *next_time = 0;
    return false;
}

#if CONFIG_ESP_SCHEDULE_ENABLE_DAYLIGHT
/**
 * @brief Calculate solar time for a given time in UTC
 * @param is_sunrise: true if sunrise, false if sunset
 * @param time_utc: time in UTC
 * @param latitude: latitude
 * @param longitude: longitude
 * @param offset_minutes: offset in minutes
 * @return solar time in UTC, 0 if calculation failed
 */
time_t esp_schedule_calc_solar_time_for_time_utc(bool is_sunrise, time_t time_utc, double latitude, double longitude, int offset_minutes)
{
    struct tm time_tm;
    localtime_r(&time_utc, &time_tm);
    time_t sunrise_utc, sunset_utc;
    int year = time_tm.tm_year + 1900;
    int month = time_tm.tm_mon + 1;
    int day = time_tm.tm_mday;

    bool calc_ok = esp_daylight_calc_sunrise_sunset_utc(year, month, day, latitude, longitude, &sunrise_utc, &sunset_utc);
    if (!calc_ok) {
        ESP_SCHEDULE_LOGW(TAG, "Failed to calculate %s for date %04d-%02d-%02d at latitude %.5f, longitude %.5f (likely polar night/day condition)",
                          is_sunrise ? "sunrise" : "sunset",
                          year, month, day, latitude, longitude
                         );
        return 0;
    }
    time_t solar_time = is_sunrise ? sunrise_utc : sunset_utc;
    return esp_daylight_apply_offset(solar_time, offset_minutes);
}

time_t esp_schedule_get_next_valid_solar_time(time_t now, const esp_schedule_trigger_t *trigger, const esp_schedule_validity_t *validity, const char *schedule_name)
{
    time_t day_end = 0;
    bool is_sunrise = trigger->type == ESP_SCHEDULE_TYPE_SUNRISE;

    // Find first candidate day (use 23:59 so day selection logic is "date-only")
    if (!esp_schedule_get_next_date_time(now, MINUTES_IN_DAY - 1, trigger->day.repeat_days, trigger->date.day, trigger->date.repeat_months, trigger->date.year, validity, &day_end)) {
        return 0;
    }

    // try for 370 days (max possible days in a year)
    for (int attempts = 0; attempts < 370; attempts++) {
        time_t solar_time = esp_schedule_calc_solar_time_for_time_utc(is_sunrise, day_end, trigger->solar.latitude, trigger->solar.longitude, trigger->solar.offset_minutes);
        if ((solar_time == 0) ||
                (validity && validity->start_time && solar_time < validity->start_time) ||
                (solar_time <= now)) {
            // No solar event on this day (polar conditions) -> advance to next valid day
            // Outside validity window or not in the future -> advance to next valid day
        } else if (validity && validity->end_time && solar_time > validity->end_time) {
            // Past validity window -> return 0
            return 0;
        } else {
            return solar_time;
        }

        // Advance anchor to next day
        if (!esp_schedule_get_next_date_time(day_end + 1, MINUTES_IN_DAY - 1, trigger->day.repeat_days, trigger->date.day, trigger->date.repeat_months, trigger->date.year, validity, &day_end)) {
            return 0;
        }
    }
    return 0;
}
#endif /* CONFIG_ESP_SCHEDULE_ENABLE_DAYLIGHT */

/*
 * Returns true if this is a one-shot trigger that has already fired, and must
 * therefore NOT be recomputed to a future occurrence. A trigger has "fired"
 * once its next_scheduled_time_utc is set (>0) and has passed (<=now).
 *
 * Repeating triggers (weekly day-of-week, yearly dates, repeating solar) return
 * false so they are recomputed and re-armed. Without this guard the date engine
 * treats an empty day/month mask as "any", so a DAY_ONCE schedule would re-fire
 * every day and a one-time DATE schedule every month.
 */
bool esp_schedule_trigger_fired_and_done(const esp_schedule_trigger_t *trigger, time_t now)
{
    if (!(trigger->next_scheduled_time_utc > 0 && trigger->next_scheduled_time_utc <= now)) {
        return false; /* not computed yet, or still in the future */
    }
    switch (trigger->type) {
    case ESP_SCHEDULE_TYPE_RELATIVE:
        return true; /* relative schedules fire exactly once */
    case ESP_SCHEDULE_TYPE_DAYS_OF_WEEK:
        return trigger->day.repeat_days == ESP_SCHEDULE_DAY_ONCE;
#if CONFIG_ESP_SCHEDULE_ENABLE_DAYLIGHT
    case ESP_SCHEDULE_TYPE_SUNRISE:
    case ESP_SCHEDULE_TYPE_SUNSET:
        /* Single-time solar schedule: no day-of-week and no date pattern. */
        return trigger->date.day == 0 && trigger->day.repeat_days == 0;
#endif
    case ESP_SCHEDULE_TYPE_DATE:
        /* Yearly-repeating dates recur. A date bound to a specific year is
         * bounded by the date engine itself (no match past that year), so it is
         * only genuinely one-shot when no year is set. */
        if (trigger->date.repeat_every_year) {
            return false;
        }
        return trigger->date.year == 0;
    default:
        return true;
    }
}

/*
 * Ensure trigger->next_scheduled_time_utc is set to the next occurrence.
 * If it's already set in the future, it is reused and not recomputed.
 * Returns true if a valid future time is present after this call, false otherwise.
 */
static bool esp_schedule_set_next_scheduled_time_utc(const char *schedule_name, esp_schedule_trigger_t *trigger, const esp_schedule_validity_t *validity)
{
    struct tm schedule_time;
    time_t now;

    /* Get current time */
    esp_schedule_get_time(&now);
    /* Always recompute the next occurrence for repeating date/day-of-week/solar
     * triggers instead of reusing a stored next_scheduled_time_utc. This keeps
     * the fire time correct after a timezone change (picked up on the next arm)
     * without needing an explicit recalculation API. RELATIVE triggers keep
     * their computed absolute target (handled below); one-shot triggers that
     * already fired are guarded next. */
    /* One-shot triggers that have already fired must not be recomputed to a
     * future occurrence (see esp_schedule_trigger_fired_and_done). */
    if (esp_schedule_trigger_fired_and_done(trigger, now)) {
        return false;
    }
    /* Handling ESP_SCHEDULE_TYPE_RELATIVE first since it doesn't require any
     * computation based on days, hours, minutes, etc.
     */
    if (trigger->type == ESP_SCHEDULE_TYPE_RELATIVE) {
        /* Compute only once from first encounter. If already set and passed, do not recompute. */
        if (trigger->next_scheduled_time_utc == 0) {
            time_t base = now;
            if (validity && validity->start_time && validity->start_time > now) {
                base = validity->start_time;
            }
            time_t target = base + (time_t)trigger->relative_seconds;
            localtime_r(&target, &schedule_time);
            trigger->next_scheduled_time_utc = mktime(&schedule_time);
        }
        if (validity) {
            if ((validity->start_time && trigger->next_scheduled_time_utc < validity->start_time) ||
                    (validity->end_time && trigger->next_scheduled_time_utc > validity->end_time)) {
                trigger->next_scheduled_time_utc = 0;
                return false;
            }
        }
        return (trigger->next_scheduled_time_utc > now);
    }

#if CONFIG_ESP_SCHEDULE_ENABLE_DAYLIGHT
    /* Handle solar-based schedules (sunrise/sunset) */
    if (trigger->type == ESP_SCHEDULE_TYPE_SUNRISE || trigger->type == ESP_SCHEDULE_TYPE_SUNSET) {
        time_t solar_time = esp_schedule_get_next_valid_solar_time(now, trigger, validity, schedule_name);
        if (solar_time == 0) {
            ESP_SCHEDULE_LOGW(TAG, "Solar schedule %s cannot be calculated (no sunrise/sunset at this location/date)", schedule_name);
            return false;
        }

        trigger->next_scheduled_time_utc = solar_time;
        return (trigger->next_scheduled_time_utc > now);
    }
#endif /* CONFIG_ESP_SCHEDULE_ENABLE_DAYLIGHT */

    /* Unified DATE and DAYS_OF_WEEK using date finder */
    time_t next_time = 0;
    bool ok = false;
    uint16_t minutes_since_midnight = (uint16_t)(trigger->hours * 60 + trigger->minutes);
    if (trigger->type == ESP_SCHEDULE_TYPE_DATE) {
        /* repeat_every_year: ignore the specific year so the month/day pattern
         * recurs every year. Otherwise honor the year, which bounds the schedule
         * to a single year (the engine returns no match once that year passes). */
        uint16_t match_year = trigger->date.repeat_every_year ? 0 : trigger->date.year;
        ok = esp_schedule_get_next_date_time(now, minutes_since_midnight, 0, trigger->date.day, trigger->date.repeat_months, match_year, validity, &next_time);
    } else if (trigger->type == ESP_SCHEDULE_TYPE_DAYS_OF_WEEK) {
        ok = esp_schedule_get_next_date_time(now, minutes_since_midnight, trigger->day.repeat_days, 0, 0, 0, validity, &next_time);
    }
    if (!ok || next_time == 0) {
        return false;
    }
    trigger->next_scheduled_time_utc = next_time;
    return (trigger->next_scheduled_time_utc > now);
}

/*
 * Calculate the next time diff across all triggers and set schedule->next_scheduled_time_utc
 * to the chosen (nearest future) occurrence. Returns 0 (and clears the chosen timestamp) if no
 * trigger has a valid future occurrence.
 */
static uint32_t esp_schedule_get_next_schedule_time_diff_multi(esp_schedule_t *schedule)
{
    if (schedule == NULL) {
        return 0;
    }

    const char *schedule_name = schedule->name;

    /* If no trigger list provided, nothing to schedule */
    if (schedule->triggers.list == NULL || schedule->triggers.count == 0) {
        schedule->next_scheduled_time_utc = 0;
        return 0;
    }

    uint32_t best_diff = 0;
    bool best_set = false;
    time_t best_utc = 0;
    time_t now = 0;
    esp_schedule_get_time(&now);

    for (uint8_t i = 0; i < schedule->triggers.count; i++) {
        esp_schedule_trigger_t *tr = &schedule->triggers.list[i];
        /* Compute or reuse next_scheduled_time_utc */
        bool ok = esp_schedule_set_next_scheduled_time_utc(schedule_name, tr, &schedule->validity);
        if (!ok) {
            continue;
        }
        /* Select nearest future ts */
        if (tr->next_scheduled_time_utc > now) {
            uint32_t diff = (uint32_t)difftime(tr->next_scheduled_time_utc, now);
            if (!best_set || diff < best_diff) {
                best_set = true;
                best_diff = diff;
                best_utc = tr->next_scheduled_time_utc;
            }
        }
    }

    if (!best_set) {
        /* No valid trigger */
        schedule->next_scheduled_time_utc = 0;
        return 0;
    }

    /* Store the chosen timestamp */
    schedule->next_scheduled_time_utc = best_utc;

    /* Print chosen schedule time once */
    {
        char time_str[64];
        struct tm schedule_time;
        memset(&schedule_time, 0, sizeof(schedule_time));
        localtime_r(&best_utc, &schedule_time);
        memset(time_str, 0, sizeof(time_str));
        strftime(time_str, sizeof(time_str), "%c %z[%Z]", &schedule_time);
        ESP_SCHEDULE_LOGI(TAG, "Schedule %s will be active on: %s. DST: %s", schedule_name, time_str, schedule_time.tm_isdst ? "Yes" : "No");
    }
    return best_diff;
}

static void esp_schedule_stop_timer(esp_schedule_t *schedule)
{
    if (schedule->timer) {
        esp_schedule_timer_stop(schedule->timer);
    }
}

/*
 * Ensure the schedule's timer exists and (re)arm it for the given number of
 * seconds. Creates the FreeRTOS software timer lazily on the first arm. The
 * period is computed in 64-bit and clamped so that a large seconds value cannot
 * overflow the 32-bit tick math ((seconds * 1000) used to overflow for diffs
 * beyond ~49 days). If the requested delay exceeds what a single TickType_t
 * period can represent, it is clamped; esp_schedule_common_timer_cb re-arms for
 * the remaining time when it detects an early expiry.
 * Returns ESP_SCHEDULE_RET_NO_MEM if the timer could not be created.
 */
static ESP_SCHEDULE_RETURN_TYPE esp_schedule_arm_timer(esp_schedule_t *schedule, uint32_t seconds)
{
    if (!esp_schedule_timer_start(&schedule->timer, seconds, esp_schedule_common_timer_cb, (void *)schedule)) {
        return ESP_SCHEDULE_RET_NO_MEM;
    }
    return ESP_SCHEDULE_RET_OK;
}

static void esp_schedule_delete_timer(esp_schedule_t *schedule)
{
    esp_schedule_timer_cancel(&schedule->timer);
}

static ESP_SCHEDULE_RETURN_TYPE esp_schedule_start_timer(esp_schedule_t *schedule)
{
    time_t current_time = 0;
    esp_schedule_get_time(&current_time);
    if (current_time < SECONDS_TILL_2020) {
        ESP_SCHEDULE_LOGE(TAG, "Time is not updated");
        /* Time is no longer valid (e.g. RTC lost). Stop any already-armed timer
         * and clear the chosen time so we don't keep firing on a stale diff. It
         * will be recomputed once time is synced and the schedule re-enabled.
         * This is a transient condition, not an arming failure. */
        esp_schedule_stop_timer(schedule);
        schedule->next_scheduled_time_utc = 0;
        return ESP_SCHEDULE_RET_OK;
    }

    schedule->next_scheduled_time_diff = esp_schedule_get_next_schedule_time_diff_multi(schedule);

    /* Check if schedule calculation failed (returns 0) */
    if (schedule->next_scheduled_time_diff == 0) {
        ESP_SCHEDULE_LOGW(TAG, "Schedule %s calculation failed or returned invalid time. Skipping timer creation.", schedule->name);
        /* Reset timestamp to indicate schedule is not active. No future
         * occurrence is not an error, so report success. */
        schedule->next_scheduled_time_utc = 0;
        return ESP_SCHEDULE_RET_OK;
    }

    ESP_SCHEDULE_LOGI(TAG, "Starting a timer for %"PRIu32" seconds for schedule %s", schedule->next_scheduled_time_diff, schedule->name);

    if (schedule->timestamp_cb) {
        schedule->timestamp_cb((esp_schedule_handle_t)schedule, schedule->next_scheduled_time_utc, schedule->priv_data);
    }

    ESP_SCHEDULE_RETURN_TYPE ret = esp_schedule_arm_timer(schedule, schedule->next_scheduled_time_diff);
    if (ret != ESP_SCHEDULE_RET_OK) {
        ESP_SCHEDULE_LOGE(TAG, "Failed to arm timer for schedule %s", schedule->name);
        schedule->next_scheduled_time_utc = 0;
    }
    return ret;
}

static void esp_schedule_common_timer_cb(void *priv_data)
{
    esp_schedule_t *schedule = (esp_schedule_t *)priv_data;

    /* Guard against a premature timer expiry: if the scheduled instant has not
     * actually arrived (e.g. tick truncation on a very long delay that had to be
     * clamped), re-arm for the remaining time instead of firing early. */
    time_t now = 0;
    esp_schedule_get_time(&now);
    if (schedule->next_scheduled_time_utc > now) {
        ESP_SCHEDULE_LOGW(TAG, "Schedule %s fired early; rescheduling for the remaining time", schedule->name);
        esp_schedule_start_timer(schedule);
        return;
    }

    /* Re-check the validity window at fire time. The occurrence was computed to
     * be within [start,end], but callback dispatch can be delayed past end_time
     * (tick truncation, timer-queue latency, system load). Suppress a trigger
     * that is now outside the window; the re-arm below will find no further
     * valid occurrence and leave the schedule disarmed. */
    if (schedule->validity.end_time != 0 && now > schedule->validity.end_time) {
        ESP_SCHEDULE_LOGW(TAG, "Schedule %s expired before dispatch; suppressing out-of-window trigger", schedule->name);
        esp_schedule_start_timer(schedule);
        return;
    }

    ESP_SCHEDULE_LOGI(TAG, "Schedule %s triggered", schedule->name);
    if (schedule->trigger_cb) {
        schedule->trigger_cb((esp_schedule_handle_t)schedule, schedule->priv_data);
    }

    esp_schedule_start_timer(schedule);
}

ESP_SCHEDULE_RETURN_TYPE esp_schedule_get(esp_schedule_handle_t handle, esp_schedule_config_t *schedule_config)
{
    if (schedule_config == NULL) {
        return ESP_SCHEDULE_RET_INVALID_ARG;
    }
    if (handle == NULL) {
        return ESP_SCHEDULE_RET_INVALID_ARG;
    }
    esp_schedule_t *schedule = (esp_schedule_t *)handle;

    /* Copy trigger list. Handle the empty case explicitly: allocating 0 bytes is
     * implementation-defined (may return NULL) and would be a spurious error. */
    if (schedule->triggers.count == 0 || schedule->triggers.list == NULL) {
        schedule_config->triggers.list = NULL;
        schedule_config->triggers.count = 0;
    } else {
        size_t trigger_list_size = schedule->triggers.count * sizeof(esp_schedule_trigger_t);
        schedule_config->triggers.list = (esp_schedule_trigger_t *)ESP_SCHEDULE_MALLOC(trigger_list_size);
        if (schedule_config->triggers.list == NULL) {
            return ESP_SCHEDULE_RET_NO_MEM;
        }
        memcpy(schedule_config->triggers.list, schedule->triggers.list, trigger_list_size);
        schedule_config->triggers.count = schedule->triggers.count;
    }

    strlcpy(schedule_config->name, schedule->name, sizeof(schedule_config->name));
    schedule_config->trigger_cb = schedule->trigger_cb;
    schedule_config->timestamp_cb = schedule->timestamp_cb;
    schedule_config->priv_data = schedule->priv_data;
    schedule_config->validity = schedule->validity;
    return ESP_SCHEDULE_RET_OK;
}

void esp_schedule_config_free_internals(esp_schedule_config_t *schedule_config)
{
    if (schedule_config == NULL) {
        return;
    }
    if (schedule_config->triggers.list) {
        ESP_SCHEDULE_FREE(schedule_config->triggers.list);
        schedule_config->triggers.list = NULL;
        schedule_config->triggers.count = 0;
    }
}

ESP_SCHEDULE_RETURN_TYPE esp_schedule_enable(esp_schedule_handle_t handle)
{
    if (handle == NULL) {
        return ESP_SCHEDULE_RET_INVALID_ARG;
    }
    esp_schedule_t *schedule = (esp_schedule_t *)handle;
    return esp_schedule_start_timer(schedule);
}

ESP_SCHEDULE_RETURN_TYPE esp_schedule_disable(esp_schedule_handle_t handle)
{
    if (handle == NULL) {
        return ESP_SCHEDULE_RET_INVALID_ARG;
    }
    esp_schedule_t *schedule = (esp_schedule_t *)handle;
    esp_schedule_stop_timer(schedule);
    /* Disabling a schedule resets the cached next-fire times so they are fully
     * recomputed on the next enable. Both the schedule-level chosen time AND the
     * per-trigger times must be cleared: the re-arm path reads the per-trigger
     * next_scheduled_time_utc and, if left stale (already passed), would treat a
     * RELATIVE or one-shot trigger as already fired and never re-arm it. */
    schedule->next_scheduled_time_utc = 0;
    for (uint8_t i = 0; i < schedule->triggers.count; i++) {
        schedule->triggers.list[i].next_scheduled_time_utc = 0;
    }
    return ESP_SCHEDULE_RET_OK;
}

static ESP_SCHEDULE_RETURN_TYPE esp_schedule_set(esp_schedule_t *schedule, esp_schedule_config_t *schedule_config)
{
    /* Build the new trigger list first, without touching the schedule, so a
     * later failure (allocation or persistence) can leave the schedule exactly
     * as it was. */
    esp_schedule_trigger_t *new_list = NULL;
    uint8_t new_count = 0;
    if (schedule_config->triggers.count > 0 && schedule_config->triggers.list != NULL) {
        size_t bytes = (size_t)schedule_config->triggers.count * sizeof(esp_schedule_trigger_t);
        new_list = (esp_schedule_trigger_t *)ESP_SCHEDULE_CALLOC(1, bytes);
        if (new_list == NULL) {
            return ESP_SCHEDULE_RET_NO_MEM;
        }
        memcpy(new_list, schedule_config->triggers.list, bytes);
        new_count = schedule_config->triggers.count;
    }

    /* Stop and tear down any running timer BEFORE mutating the trigger list.
     * esp_schedule_delete_timer() barriers against an in-flight timer callback
     * (which runs in the FreeRTOS timer daemon task and re-reads the trigger
     * list), so the swap below and the free of the old list cannot race it. */
    if (schedule->timer != NULL) {
        esp_schedule_delete_timer(schedule);
    }

    /* Snapshot the current state so we can roll back if persistence fails. The
     * old trigger list is kept (not freed) until we know the change stuck. */
    esp_schedule_trigger_list_t old_triggers = schedule->triggers;
    esp_schedule_validity_t old_validity = schedule->validity;
    esp_schedule_trigger_cb_t old_trigger_cb = schedule->trigger_cb;
    esp_schedule_timestamp_cb_t old_timestamp_cb = schedule->timestamp_cb;
    void *old_priv_data = schedule->priv_data;
    time_t old_next = schedule->next_scheduled_time_utc;

    /* Apply the new state. */
    schedule->triggers.list = new_list;
    schedule->triggers.count = new_count;
    schedule->next_scheduled_time_utc = 0; /* chosen during start */
    schedule->trigger_cb = schedule_config->trigger_cb;
    schedule->timestamp_cb = schedule_config->timestamp_cb;
    schedule->priv_data = schedule_config->priv_data;
    schedule->validity = schedule_config->validity;

    /* Calculate trigger timestamps once */
    for (uint8_t i = 0; i < schedule->triggers.count; i++) {
        esp_schedule_trigger_t *tr = &schedule->triggers.list[i];
        esp_schedule_set_next_scheduled_time_utc(schedule->name, tr, &schedule_config->validity);
    }

#if ESP_SCHEDULE_NVS_ENABLED
    ESP_SCHEDULE_RETURN_TYPE nvs_ret = esp_schedule_nvs_add(schedule);
    /* INVALID_STATE just means NVS is not enabled at runtime - that is not a
     * failure to persist. Any other error is a genuine persistence failure: roll
     * back so the RAM state stays consistent with what is (still) in flash. */
    if (nvs_ret != ESP_SCHEDULE_RET_OK && nvs_ret != ESP_SCHEDULE_RET_INVALID_STATE) {
        if (new_list) {
            ESP_SCHEDULE_FREE(new_list);
        }
        schedule->triggers = old_triggers;
        schedule->validity = old_validity;
        schedule->trigger_cb = old_trigger_cb;
        schedule->timestamp_cb = old_timestamp_cb;
        schedule->priv_data = old_priv_data;
        schedule->next_scheduled_time_utc = old_next;
        return nvs_ret;
    }
#else
    (void)old_validity;
    (void)old_trigger_cb;
    (void)old_timestamp_cb;
    (void)old_priv_data;
    (void)old_next;
#endif

    /* Success: the change stuck, so the old trigger list can be released. */
    if (old_triggers.list) {
        ESP_SCHEDULE_FREE(old_triggers.list);
    }
    return ESP_SCHEDULE_RET_OK;
}

ESP_SCHEDULE_RETURN_TYPE esp_schedule_edit(esp_schedule_handle_t handle, esp_schedule_config_t *schedule_config)
{
    if (handle == NULL || schedule_config == NULL) {
        return ESP_SCHEDULE_RET_INVALID_ARG;
    }
    esp_schedule_t *schedule = (esp_schedule_t *)handle;
    if (strncmp(schedule->name, schedule_config->name, sizeof(schedule->name)) != 0) {
        ESP_SCHEDULE_LOGE(TAG, "Schedule name mismatch. Expected: %s, Passed: %s", schedule->name, schedule_config->name);
        return ESP_SCHEDULE_RET_FAIL;
    }

    /* Reset chosen trigger timestamp; it will be recomputed */
    schedule->next_scheduled_time_utc = 0;
    ESP_SCHEDULE_RETURN_TYPE ret = esp_schedule_set(schedule, schedule_config);
    if (ret != ESP_SCHEDULE_RET_OK) {
        ESP_SCHEDULE_LOGE(TAG, "Failed to edit schedule %s (error %d)", schedule->name, (int)ret);
        return ret;
    }
    ESP_SCHEDULE_LOGD(TAG, "Schedule %s edited", schedule->name);
    return ESP_SCHEDULE_RET_OK;
}

static void esp_schedule_free_schedule(esp_schedule_t *schedule)
{
    if (schedule == NULL) {
        return;
    }
    if (schedule->timer) {
        esp_schedule_stop_timer(schedule);
        esp_schedule_delete_timer(schedule);
    }
    if (schedule->triggers.list) {
        ESP_SCHEDULE_FREE(schedule->triggers.list);
        schedule->triggers.list = NULL;
        schedule->triggers.count = 0;
    }
    ESP_SCHEDULE_FREE(schedule);
}

static void esp_schedule_free_all_schedules(esp_schedule_handle_t *handle_list, uint8_t schedule_count)
{
    for (uint8_t i = 0; i < schedule_count; i++) {
        esp_schedule_free_schedule(handle_list[i]);
    }
}

ESP_SCHEDULE_RETURN_TYPE esp_schedule_delete(esp_schedule_handle_t handle)
{
    if (handle == NULL) {
        return ESP_SCHEDULE_RET_INVALID_ARG;
    }
    esp_schedule_t *schedule = (esp_schedule_t *)handle;
    ESP_SCHEDULE_LOGI(TAG, "Deleting schedule %s", schedule->name);
#if ESP_SCHEDULE_NVS_ENABLED
    esp_schedule_nvs_remove(schedule);
#endif
    esp_schedule_free_schedule(schedule);
    return ESP_SCHEDULE_RET_OK;
}

ESP_SCHEDULE_RETURN_TYPE esp_schedule_delete_all(esp_schedule_handle_t *handle_list, uint8_t schedule_count)
{
#if ESP_SCHEDULE_NVS_ENABLED
    esp_schedule_nvs_remove_all();
#endif
    esp_schedule_free_all_schedules(handle_list, schedule_count);
    return ESP_SCHEDULE_RET_OK;
}

#if ESP_SCHEDULE_NVS_ENABLED
ESP_SCHEDULE_RETURN_TYPE esp_schedule_unload(esp_schedule_handle_t handle)
{
    if (handle == NULL) {
        return ESP_SCHEDULE_RET_INVALID_ARG;
    }
    esp_schedule_t *schedule = (esp_schedule_t *)handle;
    ESP_SCHEDULE_LOGI(TAG, "Freeing schedule %s from memory", schedule->name);
    esp_schedule_free_schedule(schedule);
    return ESP_SCHEDULE_RET_OK;
}

ESP_SCHEDULE_RETURN_TYPE esp_schedule_unload_all(esp_schedule_handle_t *handle_list, uint8_t schedule_count)
{
    esp_schedule_free_all_schedules(handle_list, schedule_count);
    return ESP_SCHEDULE_RET_OK;
}
#endif /* ESP_SCHEDULE_NVS_ENABLED */

ESP_SCHEDULE_RETURN_TYPE esp_schedule_create(const esp_schedule_config_t *schedule_config, esp_schedule_handle_t *handle_out)
{
    if (schedule_config == NULL || handle_out == NULL) {
        return ESP_SCHEDULE_RET_INVALID_ARG;
    }
    if (strlen(schedule_config->name) <= 0) {
        ESP_SCHEDULE_LOGE(TAG, "Set schedule failed. Please enter a unique valid name for the schedule.");
        return ESP_SCHEDULE_RET_INVALID_ARG;
    }

    /* Validate at least one trigger present */
    if (schedule_config->triggers.count == 0 || schedule_config->triggers.list == NULL) {
        ESP_SCHEDULE_LOGE(TAG, "Schedule must have at least one trigger.");
        return ESP_SCHEDULE_RET_INVALID_ARG;
    }

    /* Reject invalid trigger types up front. An INVALID (e.g. zero-initialized)
     * trigger can never compute a next occurrence, so it would otherwise be
     * accepted as a valid handle that silently never fires (and be persisted as
     * a permanent no-op that reloads on every boot). */
    for (uint8_t i = 0; i < schedule_config->triggers.count; i++) {
        if (schedule_config->triggers.list[i].type == ESP_SCHEDULE_TYPE_INVALID) {
            ESP_SCHEDULE_LOGE(TAG, "Trigger %u of schedule %s has an invalid type.", i, schedule_config->name);
            return ESP_SCHEDULE_RET_INVALID_ARG;
        }
    }

    esp_schedule_t *schedule = (esp_schedule_t *)ESP_SCHEDULE_CALLOC(1, sizeof(esp_schedule_t));
    if (schedule == NULL) {
        ESP_SCHEDULE_LOGE(TAG, "Could not allocate handle");
        return ESP_SCHEDULE_RET_NO_MEM;
    }
    strlcpy(schedule->name, schedule_config->name, sizeof(schedule->name));

    ESP_SCHEDULE_RETURN_TYPE ret = esp_schedule_set(schedule, (esp_schedule_config_t *)schedule_config);
    if (ret != ESP_SCHEDULE_RET_OK) {
        /* esp_schedule_set may have allocated the trigger list before failing
         * (e.g. persistence error), so free the whole schedule, not just the handle. */
        esp_schedule_free_schedule(schedule);
        return ret;
    }

    *handle_out = (esp_schedule_handle_t)schedule;
    ESP_SCHEDULE_LOGD(TAG, "Schedule %s created", schedule->name);
    return ESP_SCHEDULE_RET_OK;
}

ESP_SCHEDULE_RETURN_TYPE esp_schedule_set_trigger_callback(esp_schedule_handle_t handle, esp_schedule_trigger_cb_t trigger_cb)
{
    if (handle == NULL) {
        return ESP_SCHEDULE_RET_INVALID_ARG;
    }
    esp_schedule_t *schedule = (esp_schedule_t *)handle;
    schedule->trigger_cb = trigger_cb;
    return ESP_SCHEDULE_RET_OK;
}

ESP_SCHEDULE_RETURN_TYPE esp_schedule_set_timestamp_callback(esp_schedule_handle_t handle, esp_schedule_timestamp_cb_t timestamp_cb)
{
    if (handle == NULL) {
        return ESP_SCHEDULE_RET_INVALID_ARG;
    }
    esp_schedule_t *schedule = (esp_schedule_t *)handle;
    schedule->timestamp_cb = timestamp_cb;
    return ESP_SCHEDULE_RET_OK;
}

ESP_SCHEDULE_RETURN_TYPE esp_schedule_init_default(void)
{
    esp_schedule_timesync_init();
    init_done = true;
    return ESP_SCHEDULE_RET_OK;
}

#if ESP_SCHEDULE_NVS_ENABLED
/* Returns true only if all triggers of the schedule are expired (no future occurrence). */
static bool esp_schedule_is_expired(esp_schedule_t *schedule)
{
    for (uint8_t i = 0; i < schedule->triggers.count; i++) {
        if (esp_schedule_set_next_scheduled_time_utc(schedule->name, &schedule->triggers.list[i], &schedule->validity)) {
            return false; /* If any trigger has a future occurrence, the schedule is not expired */
        }
    }
    return true;
}

ESP_SCHEDULE_RETURN_TYPE esp_schedule_init_nvs(char *nvs_partition, esp_schedule_priv_data_callbacks_t *priv_data_callbacks, uint8_t *schedule_count, esp_schedule_handle_t **handles_out)
{
    if (schedule_count == NULL || handles_out == NULL) {
        return ESP_SCHEDULE_RET_INVALID_ARG;
    }

    esp_schedule_timesync_init();

    /* Initialize NVS */
    ESP_SCHEDULE_RETURN_TYPE ret = esp_schedule_nvs_init(nvs_partition, priv_data_callbacks);
    if (ret != ESP_SCHEDULE_RET_OK) {
        return ret;
    }

    /* Get handle list from NVS */
    *handles_out = esp_schedule_nvs_get_all(schedule_count);
    if (*handles_out == NULL) {
        ESP_SCHEDULE_LOGI(TAG, "No schedules found in NVS");
        *schedule_count = 0;
    } else {
        ESP_SCHEDULE_LOGI(TAG, "Schedules found in NVS: %"PRIu8, *schedule_count);
        /* Start/Delete the schedules. Iterate downwards so a swap-removal of an
         * expired schedule only moves an already-processed entry into the slot. */
        esp_schedule_t *schedule = NULL;
        for (int handle_count = *schedule_count - 1; handle_count >= 0; handle_count--) {
            schedule = (esp_schedule_t *) * (*handles_out + handle_count);
            schedule->trigger_cb = NULL;
            schedule->timestamp_cb = NULL;
            schedule->timer = NULL;
            /* Check for ONCE and expired schedules and delete them. */
            if (esp_schedule_is_expired(schedule)) {
                ESP_SCHEDULE_LOGI(TAG, "Schedule %s does not repeat and has already expired. Deleting it.", schedule->name);
                /* The app never receives this handle, so it cannot free the
                 * private data that on_load allocated for it. Release it via the
                 * on_free callback (no-op if none registered) before the schedule
                 * struct is freed, to avoid leaking it on every boot. */
                if (schedule->priv_data != NULL) {
                    esp_schedule_nvs_free_loaded_priv_data(schedule->priv_data);
                    schedule->priv_data = NULL;
                }
                esp_schedule_delete((esp_schedule_handle_t)schedule);
                (*handles_out)[handle_count] = (*handles_out)[*schedule_count - 1];
                (*handles_out)[*schedule_count - 1] = NULL;
                (*schedule_count)--;
                continue;
            }
            esp_schedule_start_timer(schedule);
        }
    }
    init_done = true;
    return ESP_SCHEDULE_RET_OK;
}
#endif /* ESP_SCHEDULE_NVS_ENABLED */

esp_schedule_handle_t *esp_schedule_init(bool enable_nvs, char *nvs_partition, uint8_t *schedule_count)
{
#if !ESP_SCHEDULE_NVS_ENABLED
    /* Force default (non-NVS) init when NVS support is compiled out. */
    enable_nvs = false;
#endif

    if (!enable_nvs) {
        esp_schedule_init_default();
        if (schedule_count) {
            *schedule_count = 0;
        }
        return NULL;
    }

#if ESP_SCHEDULE_NVS_ENABLED
    if (schedule_count == NULL) {
        ESP_SCHEDULE_LOGE(TAG, "schedule_count cannot be NULL when NVS is enabled");
        return NULL;
    }
    esp_schedule_handle_t *handle_list = NULL;
    esp_schedule_init_nvs(nvs_partition, NULL, schedule_count, &handle_list);
    return handle_list;
#else
    return NULL;
#endif
}
