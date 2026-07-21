/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include "esp_err.h"
#include "esp_netif.h"
#include "nvs.h"
#include "esp_log.h"
#include "unity.h"
#include "esp_schedule_internal.h"
#if CONFIG_ESP_SCHEDULE_ENABLE_DAYLIGHT
#include "esp_daylight.h"
#endif

static const char *TAG = "test_app";

static void print_time(const char *label, time_t t)
{
    struct tm tm_local;
    localtime_r(&t, &tm_local);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %z[%Z]", &tm_local);
    ESP_LOGI(TAG, "%s: %s (%ld)", label, buf, (long)t);
}

static time_t make_time_utc(int year, int mon, int mday, int hour, int min, int sec)
{
    struct tm tmv = {0};
    tmv.tm_year = year - 1900;
    tmv.tm_mon = mon - 1;
    tmv.tm_mday = mday;
    tmv.tm_hour = hour;
    tmv.tm_min = min;
    tmv.tm_sec = sec;
    tmv.tm_isdst = -1; /* let mktime resolve DST for the active timezone */
    return mktime(&tmv);
}

static void assert_time_eq(const char *name, time_t got, time_t want)
{
    if (got != want) {
        print_time("got ", got);
        print_time("want", want);
    }
    TEST_ASSERT_TRUE_MESSAGE(got == want, name);
}

static void __match_trigger(esp_schedule_trigger_t *got, esp_schedule_trigger_t *want)
{
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(got->type, want->type, "Trigger types should match");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(got->hours, want->hours, "Trigger hours should match");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(got->minutes, want->minutes, "Trigger minutes should match");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(got->day.repeat_days, want->day.repeat_days, "Trigger days should match");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(got->date.day, want->date.day, "Trigger date should match");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(got->date.repeat_months, want->date.repeat_months, "Trigger months should match");
#if CONFIG_ESP_SCHEDULE_ENABLE_DAYLIGHT
    TEST_ASSERT_EQUAL_DOUBLE_MESSAGE(got->solar.latitude, want->solar.latitude, "Trigger latitude should match");
    TEST_ASSERT_EQUAL_DOUBLE_MESSAGE(got->solar.longitude, want->solar.longitude, "Trigger longitude should match");
    TEST_ASSERT_EQUAL_INT32_MESSAGE(got->solar.offset_minutes, want->solar.offset_minutes, "Trigger offset minutes should match");
#endif
}

static void nvs_erase_schd(void)
{
    nvs_handle_t h;
    if (nvs_open_from_partition("nvs", "schd", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

// --- Date permutations ---
TEST_CASE("date permutations", "[esp_schedule]")
{
    time_t now = make_time_utc(2025, 1, 16, 12, 0, 0); // Thu
    esp_schedule_validity_t validity = { .start_time = 0, .end_time = now + 365 * 24 * 3600 };

    // 17th at 00:24
    time_t next_ts = 0;
    bool ok = esp_schedule_get_next_date_time(now, /*00:24*/24, /*days_of_week*/0, /*day_of_month*/17, /*months*/0, /*year*/0, &validity, &next_ts);
    TEST_ASSERT_TRUE_MESSAGE(ok, "date: 17th 00:24");
    assert_time_eq("date: 17th 00:24", next_ts, make_time_utc(2025, 1, 17, 0, 24, 0));

    // Specific month mask (Jan, Mar) on 20th at 08:00 => Jan 20 since we're in Jan
    next_ts = 0; ok = esp_schedule_get_next_date_time(now, 8 * 60, 0, 20, (1u << 0) | (1u << 2), 0, &validity, &next_ts);
    TEST_ASSERT_TRUE_MESSAGE(ok, "date: month mask Jan/Mar day=20 08:00");
    assert_time_eq("date: month mask Jan/Mar day=20 08:00", next_ts, make_time_utc(2025, 1, 20, 8, 0, 0));

    // Specific year constraint (2026) day 5 at 09:15
    next_ts = 0; ok = esp_schedule_get_next_date_time(now, 9 * 60 + 15, 0, 5, 0, 2026, &validity, &next_ts);
    TEST_ASSERT_TRUE_MESSAGE(ok, "date: year=2026 day=5 09:15");
    assert_time_eq("date: year=2026 day=5 09:15", next_ts, make_time_utc(2026, 1, 5, 9, 15, 0));
}

// --- More date permutations (day-31 across months, year rollover) ---
TEST_CASE("date permutations more", "[esp_schedule]")
{
    // Day=31 with months mask including a 30-day month (Apr) and 31-day month (May).
    // Regression: a day-31 schedule must NOT fire on a normalized Mar/May-1 after a
    // short month; it must land on May 31.
    time_t now = make_time_utc(2025, 4, 29, 10, 0, 0);
    esp_schedule_validity_t validity = { .start_time = 0, .end_time = now + 400 * 24 * 3600 };

    time_t next_ts = 0; bool ok = esp_schedule_get_next_date_time(now, 6 * 60, 0, 31, (1u << 3) | (1u << 4), 0, &validity, &next_ts); // Apr(3), May(4)
    TEST_ASSERT_TRUE_MESSAGE(ok, "date: 31st across months");
    assert_time_eq("date: 31st across months -> May 31 06:00", next_ts, make_time_utc(2025, 5, 31, 6, 0, 0));

    // Month rollover year: months {Nov, Dec, Jan}, day=1 at 00:00 from Dec 31
    now = make_time_utc(2025, 12, 31, 23, 30, 0);
    next_ts = 0; ok = esp_schedule_get_next_date_time(now, 0, 0, 1, (1u << 10) | (1u << 11) | (1u << 0), 0, &validity, &next_ts);
    TEST_ASSERT_TRUE_MESSAGE(ok, "date: Nov/Dec/Jan day=1 at year boundary");
    assert_time_eq("date: Nov/Dec/Jan day=1 -> Jan 1 00:00", next_ts, make_time_utc(2026, 1, 1, 0, 0, 0));
}

// --- Feb 29 in non-leap years must skip to the next leap Feb 29 ---
TEST_CASE("date feb29 non leap", "[esp_schedule]")
{
    // From Mar 2025 (non-leap), a Feb-29 schedule must NOT fire on Mar 1 2026;
    // it must land on Feb 29 2028 (next leap year).
    time_t now = make_time_utc(2025, 3, 1, 12, 0, 0);
    esp_schedule_validity_t validity = { .start_time = 0, .end_time = now + 1500L * 24 * 3600 };

    time_t next_ts = 0;
    bool ok = esp_schedule_get_next_date_time(now, 9 * 60, 0, 29, ESP_SCHEDULE_MONTH_FEBRUARY, 0, &validity, &next_ts);
    TEST_ASSERT_TRUE_MESSAGE(ok, "date: Feb 29 skips non-leap years");
    assert_time_eq("date: Feb 29 -> 2028-02-29 09:00", next_ts, make_time_utc(2028, 2, 29, 9, 0, 0));
}

// --- Day of week ---
TEST_CASE("day of week", "[esp_schedule]")
{
    time_t now = make_time_utc(2025, 1, 16, 7, 45, 0); // Thu 07:45
    esp_schedule_validity_t validity = { .start_time = 0, .end_time = now + 30 * 24 * 3600 };

    uint8_t days_of_week = (1 << 0) | (1 << 1); // Mon/Tue
    time_t next_ts = 0; bool ok = esp_schedule_get_next_date_time(now, 8 * 60 + 30, days_of_week, 0, 0, 0, &validity, &next_ts);
    TEST_ASSERT_TRUE_MESSAGE(ok, "dow: Mon/Tue 08:30");
    assert_time_eq("dow: Mon/Tue 08:30", next_ts, make_time_utc(2025, 1, 20, 8, 30, 0));
}

// --- Hybrid OR (weekday OR date) ---
TEST_CASE("hybrid dow or date", "[esp_schedule]")
{
    time_t now = make_time_utc(2025, 1, 16, 7, 45, 0); // Thu 07:45
    esp_schedule_validity_t validity = { .start_time = 0, .end_time = now + 40 * 24 * 3600 };

    uint8_t days_of_week = (1 << 0) | (1 << 1);
    time_t a = 0, b = 0; bool ok_a, ok_b;
    ok_a = esp_schedule_get_next_date_time(now, 9 * 60, days_of_week, 0, 0, 0, &validity, &a);
    ok_b = esp_schedule_get_next_date_time(now, 30, 0, 17, 0, 0, &validity, &b);
    TEST_ASSERT_TRUE_MESSAGE(ok_a && ok_b, "hybrid: Mon/Tue 09:00 OR 17th 00:30");

    time_t chosen = (a < b) ? a : b;
    assert_time_eq("hybrid: Mon/Tue 09:00 OR 17th 00:30", chosen, make_time_utc(2025, 1, 17, 0, 30, 0));
}

// --- Knife edge: now equals target -> should select next occurrence ---
TEST_CASE("knife edge now equals target", "[esp_schedule]")
{
    time_t now = make_time_utc(2025, 1, 16, 8, 0, 0); // Thu 08:00
    esp_schedule_validity_t validity = { .start_time = 0, .end_time = now + 10 * 24 * 3600 };

    uint8_t days_of_week = (1 << 3); // Thursday
    time_t next_ts = 0; bool ok = esp_schedule_get_next_date_time(now, 8 * 60, days_of_week, 0, 0, 0, &validity, &next_ts);
    TEST_ASSERT_TRUE_MESSAGE(ok, "knife-edge: now != target (Thu 08:00)");

    // should be the next Thursday at 08:00
    assert_time_eq("knife-edge: now != target (Thu 08:00)", next_ts, make_time_utc(2025, 1, 23, 8, 0, 0));
}

// --- Validity window ---
TEST_CASE("validity respected", "[esp_schedule]")
{
    time_t now = make_time_utc(2025, 1, 16, 23, 50, 0);
    esp_schedule_validity_t validity = { .start_time = now + 20 * 60, .end_time = now + 2 * 24 * 3600 };

    time_t next_ts = 0; bool ok = esp_schedule_get_next_date_time(now, 10, 0, 0, 0, 0, &validity, &next_ts);
    TEST_ASSERT_TRUE_MESSAGE(ok, "validity: start boundary honored");
    assert_time_eq("validity: start boundary honored", next_ts, validity.start_time);
}

// --- Sequences for same trigger type ---
TEST_CASE("sequence dow mon wed", "[esp_schedule]")
{
    // Sequence Mon/Wed 09:00 from Monday 08:50 -> Mon 09:00, Wed 09:00, next Mon 09:00
    time_t now = make_time_utc(2025, 1, 13, 8, 50, 0); // Monday
    esp_schedule_validity_t validity = { .start_time = 0, .end_time = now + 30 * 24 * 3600 };
    uint8_t days_of_week = (1 << 0) | (1 << 2); // Mon, Wed

    time_t t1 = 0; bool ok = esp_schedule_get_next_date_time(now, 9 * 60, days_of_week, 0, 0, 0, &validity, &t1);
    TEST_ASSERT_TRUE_MESSAGE(ok, "seq dow: first");
    assert_time_eq("seq dow: first", t1, make_time_utc(2025, 1, 13, 9, 0, 0));

    time_t t2 = 0; ok = esp_schedule_get_next_date_time(t1, 9 * 60, days_of_week, 0, 0, 0, &validity, &t2);
    TEST_ASSERT_TRUE_MESSAGE(ok, "seq dow: second");
    assert_time_eq("seq dow: second", t2, make_time_utc(2025, 1, 15, 9, 0, 0));

    time_t t3 = 0; ok = esp_schedule_get_next_date_time(t2, 9 * 60, days_of_week, 0, 0, 0, &validity, &t3);
    TEST_ASSERT_TRUE_MESSAGE(ok, "seq dow: third");
    assert_time_eq("seq dow: third", t3, make_time_utc(2025, 1, 20, 9, 0, 0));
}

TEST_CASE("sequence date months mask", "[esp_schedule]")
{
    // Day=15 at 07:00 for months {Jan, Mar, Apr}
    time_t now = make_time_utc(2025, 1, 10, 7, 0, 0);
    esp_schedule_validity_t validity = { .start_time = 0, .end_time = now + 370 * 24 * 3600 };
    uint16_t months = (1u << 0) | (1u << 2) | (1u << 3); // Jan, Mar, Apr

    time_t t1 = 0; bool ok = esp_schedule_get_next_date_time(now, 7 * 60, 0, 15, months, 0, &validity, &t1);
    TEST_ASSERT_TRUE_MESSAGE(ok, "seq date: first");
    assert_time_eq("seq date: first", t1, make_time_utc(2025, 1, 15, 7, 0, 0));

    time_t t2 = 0; ok = esp_schedule_get_next_date_time(t1, 7 * 60, 0, 15, months, 0, &validity, &t2);
    TEST_ASSERT_TRUE_MESSAGE(ok, "seq date: second");
    assert_time_eq("seq date: second", t2, make_time_utc(2025, 3, 15, 7, 0, 0));

    time_t t3 = 0; ok = esp_schedule_get_next_date_time(t2, 7 * 60, 0, 15, months, 0, &validity, &t3);
    TEST_ASSERT_TRUE_MESSAGE(ok, "seq date: third");
    assert_time_eq("seq date: third", t3, make_time_utc(2025, 4, 15, 7, 0, 0));
}

TEST_CASE("sequence validity cutoff", "[esp_schedule]")
{
    // Validity end should stop sequences
    time_t now = make_time_utc(2025, 1, 13, 8, 50, 0);
    esp_schedule_validity_t validity = { .start_time = 0, .end_time = make_time_utc(2025, 1, 16, 0, 0, 0) };
    uint8_t days_of_week = (1 << 0) | (1 << 2); // Mon, Wed

    time_t t1 = 0; bool ok = esp_schedule_get_next_date_time(now, 9 * 60, days_of_week, 0, 0, 0, &validity, &t1);
    TEST_ASSERT_TRUE_MESSAGE(ok, "seq cutoff: first");
    assert_time_eq("seq cutoff: first", t1, make_time_utc(2025, 1, 13, 9, 0, 0));

    time_t t2 = 0; ok = esp_schedule_get_next_date_time(t1, 9 * 60, days_of_week, 0, 0, 0, &validity, &t2);
    TEST_ASSERT_TRUE_MESSAGE(ok, "seq cutoff: second");
    assert_time_eq("seq cutoff: second", t2, make_time_utc(2025, 1, 15, 9, 0, 0));

    time_t t3 = 0; ok = esp_schedule_get_next_date_time(t2, 9 * 60, days_of_week, 0, 0, 0, &validity, &t3);
    TEST_ASSERT_FALSE_MESSAGE(ok, "seq cutoff: third should fail due to validity end");
    TEST_ASSERT_TRUE(t3 == 0);
}

#if CONFIG_ESP_SCHEDULE_ENABLE_DAYLIGHT
TEST_CASE("solar with dow", "[esp_schedule]")
{
    double lat = 37.7749, lon = -122.4194; // San Francisco, CA
    time_t now = make_time_utc(2025, 1, 12, 6, 0, 0);
    esp_schedule_validity_t validity = { .start_time = 0, .end_time = now + 15 * 24 * 3600 };

    esp_schedule_trigger_t tr = (esp_schedule_trigger_t) {
        0
    };
    tr.type = ESP_SCHEDULE_TYPE_SUNRISE;
    tr.day.repeat_days = ESP_SCHEDULE_DAY_MONDAY | ESP_SCHEDULE_DAY_TUESDAY | ESP_SCHEDULE_DAY_WEDNESDAY | ESP_SCHEDULE_DAY_THURSDAY | ESP_SCHEDULE_DAY_FRIDAY;
    tr.solar.latitude = lat; tr.solar.longitude = lon; tr.solar.offset_minutes = 0;

    /* Get first expected sunrise of the triggered day */
    time_t last_solar = now;
    for (int day = 13; day <= 17; day++) {
        time_t sunrise = 0, sunset = 0;
        bool ok = esp_daylight_calc_sunrise_sunset_utc(2025, 1, day, lat, lon, &sunrise, &sunset);
        TEST_ASSERT_TRUE_MESSAGE(ok, "sunrise/sunset calculation");
        TEST_ASSERT_NOT_EQUAL(0, sunrise);
        TEST_ASSERT_NOT_EQUAL(0, sunset);
        last_solar = esp_schedule_get_next_valid_solar_time(last_solar, &tr, &validity, "solar_dow");
        char buf[128];
        snprintf(buf, sizeof(buf), "solar: day %d: failed to get next valid solar time", day);
        TEST_ASSERT_TRUE_MESSAGE(last_solar != 0, buf);
        snprintf(buf, sizeof(buf), "solar: day %d: %" PRIu32 " != %" PRIu32, day, (uint32_t)last_solar, (uint32_t)sunrise);
        TEST_ASSERT_TRUE_MESSAGE(last_solar == sunrise, buf);
    }
}

TEST_CASE("solar with date mask", "[esp_schedule]")
{
    double lat = 52.5200, lon = 13.4050; // Berlin, Germany
    // Use midday to avoid edge near-sunset timing
    time_t now = make_time_utc(2025, 6, 15, 12, 0, 0);
    esp_schedule_validity_t validity = { .start_time = 0, .end_time = now + 90 * 24 * 3600 };

    esp_schedule_trigger_t tr = (esp_schedule_trigger_t) {
        0
    };
    tr.type = ESP_SCHEDULE_TYPE_SUNSET;
    tr.date.day = 15;
    tr.date.repeat_months = ESP_SCHEDULE_MONTH_JUNE | ESP_SCHEDULE_MONTH_JULY | ESP_SCHEDULE_MONTH_AUGUST;
    tr.solar.latitude = lat; tr.solar.longitude = lon; tr.solar.offset_minutes = -15;

    time_t last_solar = now;
    for (int month = 6; month <= 8; month++) {
        time_t sunrise = 0, sunset = 0;
        bool ok = esp_daylight_calc_sunrise_sunset_utc(2025, month, 15, lat, lon, &sunrise, &sunset);
        TEST_ASSERT_TRUE_MESSAGE(ok, "sunrise/sunset calculation");
        TEST_ASSERT_NOT_EQUAL(0, sunset);
        last_solar = esp_schedule_get_next_valid_solar_time(last_solar, &tr, &validity, "solar_date_mask");
        char buf[128];
        snprintf(buf, sizeof(buf), "solar: month %d: failed to get next valid solar time", month);
        TEST_ASSERT_TRUE_MESSAGE(last_solar != 0, buf);
        time_t expected = sunset - 15 * 60;
        snprintf(buf, sizeof(buf), "solar: month %d: %" PRIu32 " != %" PRIu32, month, (uint32_t)last_solar, (uint32_t)expected);
        TEST_ASSERT_TRUE_MESSAGE(last_solar == expected, buf);
    }
}

TEST_CASE("solar sequence monotonic", "[esp_schedule]")
{
    int year = 2025, month = 1, day = 12; // Jan 12, 2025: Sunday
    double lat = 37.7749, lon = -122.4194; // San Francisco, CA
    time_t now = make_time_utc(year, month, day, 0, 0, 0);
    esp_schedule_validity_t validity = { .start_time = 0, .end_time = now + 10 * 24 * 3600 };

    esp_schedule_trigger_t tr = (esp_schedule_trigger_t) {
        0
    };
    tr.type = ESP_SCHEDULE_TYPE_SUNRISE;
    tr.day.repeat_days = ESP_SCHEDULE_DAY_MONDAY | ESP_SCHEDULE_DAY_WEDNESDAY | ESP_SCHEDULE_DAY_FRIDAY; // Mon, Wed, Fri
    tr.solar.latitude = lat; tr.solar.longitude = lon; tr.solar.offset_minutes = 0;

    /* Get first expected sunrise of the triggered day */
    time_t sunrise = 0, sunset = 0;
    bool ok = esp_daylight_calc_sunrise_sunset_utc(year, month, day + 1, /*next day*/ lat, lon, &sunrise, &sunset);
    TEST_ASSERT_TRUE_MESSAGE(ok, "sunrise/sunset calculation");
    TEST_ASSERT_NOT_EQUAL(0, sunrise);
    TEST_ASSERT_NOT_EQUAL(0, sunset);

    time_t s1 = esp_schedule_get_next_valid_solar_time(now, &tr, &validity, "solar_seq");
    TEST_ASSERT_TRUE_MESSAGE(s1 != 0, "solar seq first");

    char buf[128];
    snprintf(buf, sizeof(buf), "solar seq first: %" PRIu32 " != %" PRIu32, (uint32_t)s1, (uint32_t)sunrise);
    TEST_ASSERT_TRUE_MESSAGE(s1 == sunrise, buf);

    /* Get next expected sunrise on next triggered day */
    ok = esp_daylight_calc_sunrise_sunset_utc(year, month, day + 3, /*next triggered day*/ lat, lon, &sunrise, &sunset);
    TEST_ASSERT_TRUE_MESSAGE(ok, "sunrise/sunset calculation");
    TEST_ASSERT_NOT_EQUAL(0, sunrise);
    TEST_ASSERT_NOT_EQUAL(0, sunset);

    time_t s2 = esp_schedule_get_next_valid_solar_time(s1, &tr, &validity, "solar_seq");
    TEST_ASSERT_TRUE_MESSAGE(s2 != 0, "solar seq second");
    snprintf(buf, sizeof(buf), "solar seq second: %" PRIu32 " != %" PRIu32, (uint32_t)s2, (uint32_t)sunrise);
    TEST_ASSERT_TRUE_MESSAGE(s2 == sunrise, buf);
    TEST_ASSERT_TRUE_MESSAGE(s2 > s1, "solar seq monotonic");
}
#endif

/* --- esp_schedule_init must not dereference a NULL schedule_count --- */
TEST_CASE("init null schedule_count", "[esp_schedule]")
{
    /* esp_schedule_init() runs timesync (SNTP), which needs the TCP/IP stack up.
     * On a device this is already initialized; bring it up here for the test. */
    esp_netif_init();

    /* NVS-off path used to write *schedule_count unconditionally. */
    esp_schedule_handle_t *h = esp_schedule_init(false, NULL, NULL);
    TEST_ASSERT_NULL_MESSAGE(h, "init(false, NULL, NULL) should return NULL and not crash");

    /* NVS-on path with NULL count is rejected. */
    h = esp_schedule_init(true, NULL, NULL);
    TEST_ASSERT_NULL_MESSAGE(h, "init(true, NULL, NULL) should return NULL and not crash");
}

/* --- one-shot triggers must be detected as fired-and-done --- */
TEST_CASE("one-shot fired and done", "[esp_schedule]")
{
    time_t now = make_time_utc(2025, 6, 15, 12, 0, 0);
    esp_schedule_trigger_t tr;

    /* Not computed yet -> never "done". */
    memset(&tr, 0, sizeof(tr));
    tr.type = ESP_SCHEDULE_TYPE_DAYS_OF_WEEK;
    tr.day.repeat_days = ESP_SCHEDULE_DAY_ONCE;
    tr.next_scheduled_time_utc = 0;
    TEST_ASSERT_FALSE_MESSAGE(esp_schedule_trigger_fired_and_done(&tr, now), "uncomputed -> not done");

    /* DAY_ONCE fired in the past -> done (must not re-arm daily). */
    tr.next_scheduled_time_utc = now - 10;
    TEST_ASSERT_TRUE_MESSAGE(esp_schedule_trigger_fired_and_done(&tr, now), "DAY_ONCE fired -> done");

    /* Still in the future -> not done. */
    tr.next_scheduled_time_utc = now + 10;
    TEST_ASSERT_FALSE_MESSAGE(esp_schedule_trigger_fired_and_done(&tr, now), "future -> not done");

    /* Repeating weekday -> never done, even after firing. */
    tr.day.repeat_days = ESP_SCHEDULE_DAY_MONDAY;
    tr.next_scheduled_time_utc = now - 10;
    TEST_ASSERT_FALSE_MESSAGE(esp_schedule_trigger_fired_and_done(&tr, now), "repeating weekday -> not done");

    /* RELATIVE fires exactly once. */
    memset(&tr, 0, sizeof(tr));
    tr.type = ESP_SCHEDULE_TYPE_RELATIVE;
    tr.next_scheduled_time_utc = now - 1;
    TEST_ASSERT_TRUE_MESSAGE(esp_schedule_trigger_fired_and_done(&tr, now), "RELATIVE fired -> done");

    /* DATE without a year and without repeat_every_year -> one-shot. */
    memset(&tr, 0, sizeof(tr));
    tr.type = ESP_SCHEDULE_TYPE_DATE;
    tr.date.day = 15;
    tr.date.year = 0;
    tr.date.repeat_every_year = false;
    tr.next_scheduled_time_utc = now - 1;
    TEST_ASSERT_TRUE_MESSAGE(esp_schedule_trigger_fired_and_done(&tr, now), "DATE yearless one-shot -> done");

    /* DATE repeat_every_year -> recurs. */
    tr.date.repeat_every_year = true;
    TEST_ASSERT_FALSE_MESSAGE(esp_schedule_trigger_fired_and_done(&tr, now), "DATE repeat_every_year -> not done");

    /* DATE bound to a specific year -> engine bounds it, not "done" here. */
    tr.date.repeat_every_year = false;
    tr.date.year = 2025;
    TEST_ASSERT_FALSE_MESSAGE(esp_schedule_trigger_fired_and_done(&tr, now), "DATE year-bounded handled by engine");
}

/* --- repeat_every_year / year bounding in the date engine --- */
TEST_CASE("date year bounding", "[esp_schedule]")
{
    time_t now = make_time_utc(2026, 3, 1, 12, 0, 0);
    esp_schedule_validity_t validity = { .start_time = 0, .end_time = now + 800L * 24 * 3600 };
    time_t next_ts = 0;

    /* A year in the past yields no match (bounds a non-repeating date). */
    bool ok = esp_schedule_get_next_date_time(now, 9 * 60, 0, 10, 0, 2025, &validity, &next_ts);
    TEST_ASSERT_FALSE_MESSAGE(ok, "year=2025 in the past -> no match");

    /* year=0 (repeat_every_year semantics) recurs to the next occurrence. */
    next_ts = 0;
    ok = esp_schedule_get_next_date_time(now, 9 * 60, 0, 10, 0, 0, &validity, &next_ts);
    TEST_ASSERT_TRUE_MESSAGE(ok, "year=0 -> matches next occurrence");
    assert_time_eq("year=0 day=10 09:00", next_ts, make_time_utc(2026, 3, 10, 9, 0, 0));
}

/* --- DST must not be double-corrected --- */
#define NY_TZ "EST5EDT,M3.2.0,M11.1.0" /* America/New_York */

/* TZ guard so tests restore the previous value even if a Unity assertion
 * longjmps past the test's own tz_pop(). */
static bool s_tz_active = false;
static bool s_tz_had = false;
static char s_tz_saved[64];

static void tz_push(const char *tz)
{
    const char *cur = getenv("TZ");
    s_tz_had = (cur != NULL);
    if (cur) {
        strlcpy(s_tz_saved, cur, sizeof(s_tz_saved));
    }
    setenv("TZ", tz, 1);
    tzset();
    s_tz_active = true;
}

static void tz_pop(void)
{
    if (!s_tz_active) {
        return;
    }
    if (s_tz_had) {
        setenv("TZ", s_tz_saved, 1);
    } else {
        unsetenv("TZ");
    }
    tzset();
    s_tz_active = false;
}

TEST_CASE("dst daily across spring forward", "[esp_schedule]")
{
    tz_push(NY_TZ);

    /* Spring forward 2025: Sun Mar 9. Daily 12:00 from Sat Mar 8 13:00. */
    time_t now = make_time_utc(2025, 3, 8, 13, 0, 0);
    esp_schedule_validity_t validity = { .start_time = 0, .end_time = now + 10L * 24 * 3600 };
    time_t next_ts = 0;
    bool ok = esp_schedule_get_next_date_time(now, 12 * 60, ESP_SCHEDULE_DAY_EVERYDAY, 0, 0, 0, &validity, &next_ts);
    TEST_ASSERT_TRUE(ok);
    assert_time_eq("spring-forward daily 12:00", next_ts, make_time_utc(2025, 3, 9, 12, 0, 0));

    tz_pop();
}

TEST_CASE("dst daily across fall back", "[esp_schedule]")
{
    tz_push(NY_TZ);

    /* Fall back 2025: Sun Nov 2. Daily 12:00 from Sat Nov 1 13:00. */
    time_t now = make_time_utc(2025, 11, 1, 13, 0, 0);
    esp_schedule_validity_t validity = { .start_time = 0, .end_time = now + 10L * 24 * 3600 };
    time_t next_ts = 0;
    bool ok = esp_schedule_get_next_date_time(now, 12 * 60, ESP_SCHEDULE_DAY_EVERYDAY, 0, 0, 0, &validity, &next_ts);
    TEST_ASSERT_TRUE(ok);
    assert_time_eq("fall-back daily 12:00", next_ts, make_time_utc(2025, 11, 2, 12, 0, 0));

    tz_pop();
}

TEST_CASE("dst skipped local time", "[esp_schedule]")
{
    tz_push(NY_TZ);

    /* 02:30 on spring-forward day does not exist (clocks jump 02:00->03:00).
     * mktime(tm_isdst=-1) must resolve it to a real future instant, not drift. */
    time_t now = make_time_utc(2025, 3, 9, 1, 0, 0);
    esp_schedule_validity_t validity = { .start_time = 0, .end_time = now + 2L * 24 * 3600 };
    time_t next_ts = 0;
    bool ok = esp_schedule_get_next_date_time(now, 2 * 60 + 30, ESP_SCHEDULE_DAY_EVERYDAY, 0, 0, 0, &validity, &next_ts);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE_MESSAGE(next_ts > now, "skipped local time resolves to a future instant");
    TEST_ASSERT_TRUE_MESSAGE(next_ts < now + 4 * 3600, "resolved within a few hours, no day/hour drift");

    tz_pop();
}

/* --- advancing days must not skip a day across spring forward --- */
TEST_CASE("dst next dow near midnight", "[esp_schedule]")
{
    tz_push(NY_TZ);

    /* Sat Mar 8 2025 23:30, next Sunday 23:45 -> Sun Mar 9 (NOT Mar 16).
     * The old code advanced by 86400s and skipped the 23h DST day. */
    time_t now = make_time_utc(2025, 3, 8, 23, 30, 0);
    esp_schedule_validity_t validity = { .start_time = 0, .end_time = now + 30L * 24 * 3600 };
    time_t next_ts = 0;
    bool ok = esp_schedule_get_next_date_time(now, 23 * 60 + 45, ESP_SCHEDULE_DAY_SUNDAY, 0, 0, 0, &validity, &next_ts);
    TEST_ASSERT_TRUE(ok);
    assert_time_eq("next Sunday 23:45 across spring-forward", next_ts, make_time_utc(2025, 3, 9, 23, 45, 0));

    tz_pop();
}

/* --- RainMaker DST compliance ---
 * https://legacy.rainmaker.espressif.com/docs/scheduling/#managing-daylight-saving-time-dst
 * Schedules fire on LOCAL wall-clock time:
 *  - Spring forward: a 02:00-02:59 local time does not exist; the schedule is
 *    delayed by 1hr and fires at 03:00-03:59 on the switch day.
 *  - Fall back: a 01:00-01:59 local time occurs twice; the schedule fires only
 *    once (the first occurrence, before the switch), not again after it. */
TEST_CASE("dst rainmaker spring forward delayed one hour", "[esp_schedule]")
{
    tz_push(NY_TZ);

    /* Daily 02:30. On spring-forward day (Sun Mar 9 2025) 02:30 does not exist,
     * so it must fire at 03:30. From Sat Mar 8 12:00 the next occurrence is Mar 9. */
    time_t now = make_time_utc(2025, 3, 8, 12, 0, 0);
    esp_schedule_validity_t validity = { .start_time = 0, .end_time = now + 10L * 24 * 3600 };
    time_t next_ts = 0;
    bool ok = esp_schedule_get_next_date_time(now, 2 * 60 + 30, ESP_SCHEDULE_DAY_EVERYDAY, 0, 0, 0, &validity, &next_ts);
    TEST_ASSERT_TRUE(ok);
    /* 02:30 Mar 9 resolves to the same instant as 03:30 EDT. */
    assert_time_eq("spring 02:30 -> 03:30", next_ts, make_time_utc(2025, 3, 9, 3, 30, 0));
    struct tm lt;
    localtime_r(&next_ts, &lt);
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, lt.tm_hour, "must fire in the 03:00-03:59 window (delayed 1hr)");
    TEST_ASSERT_EQUAL_INT_MESSAGE(9, lt.tm_mday, "must fire on the switch day, not skip to the next");

    tz_pop();
}

TEST_CASE("dst rainmaker fall back fires once", "[esp_schedule]")
{
    tz_push(NY_TZ);

    /* Daily 01:30. On fall-back day (Sun Nov 2 2025) 01:30 occurs twice. It must
     * fire once at the first (EDT) occurrence and then advance to the next day,
     * NOT fire again at the repeated 01:30 EST. */
    time_t now = make_time_utc(2025, 11, 1, 12, 0, 0);
    esp_schedule_validity_t validity = { .start_time = 0, .end_time = now + 10L * 24 * 3600 };

    time_t t1 = 0;
    bool ok = esp_schedule_get_next_date_time(now, 1 * 60 + 30, ESP_SCHEDULE_DAY_EVERYDAY, 0, 0, 0, &validity, &t1);
    TEST_ASSERT_TRUE(ok);
    assert_time_eq("fall 01:30 first occurrence (EDT)", t1, make_time_utc(2025, 11, 2, 1, 30, 0));

    /* Next occurrence must be the next day, not the repeated 01:30 EST. */
    time_t t2 = 0;
    ok = esp_schedule_get_next_date_time(t1, 1 * 60 + 30, ESP_SCHEDULE_DAY_EVERYDAY, 0, 0, 0, &validity, &t2);
    TEST_ASSERT_TRUE(ok);
    assert_time_eq("fall next occurrence is next day", t2, make_time_utc(2025, 11, 3, 1, 30, 0));
    TEST_ASSERT_TRUE_MESSAGE((t2 - t1) >= 24 * 3600, "advanced a full day, not the repeated DST hour");

    tz_pop();
}

/* Unity runs setUp()/tearDown() before/after every TEST_CASE. tearDown()
 * restores TZ even if a DST test aborted via a failed assertion before its own
 * tz_pop(), so a leaked TZ cannot make later timezone-independent tests fail. */
void setUp(void)
{
    /* Start each test from a clean NVS namespace so count-based assertions do
     * not depend on leftover state from a prior test (or a prior run's flash). */
    nvs_erase_schd();
}

void tearDown(void)
{
    tz_pop();
}

/* =========================================== */
/* Multi-trigger and NVS v2 persistence tests  */
/* =========================================== */

TEST_CASE("nvs basic operations", "[esp_schedule]")
{
    esp_schedule_config_t config = {0};
    strcpy(config.name, "test_schedule");
    config.triggers.count = 1;
    config.triggers.list = (esp_schedule_trigger_t *)calloc(1, sizeof(esp_schedule_trigger_t));
    config.triggers.list[0].type = ESP_SCHEDULE_TYPE_DAYS_OF_WEEK;
    config.triggers.list[0].hours = 8;
    config.triggers.list[0].minutes = 0;
    config.triggers.list[0].day.repeat_days = ESP_SCHEDULE_DAY_MONDAY;
    config.validity.start_time = 0;
    config.validity.end_time = 2147483647;

    esp_schedule_handle_t handle = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(ESP_OK, (int)esp_schedule_create(&config, &handle), "Failed to create schedule");
    TEST_ASSERT_NOT_NULL_MESSAGE(handle, "Failed to create schedule");

    uint8_t count = 0;
    esp_schedule_handle_t *handles = esp_schedule_nvs_get_all(&count);
    TEST_ASSERT_NOT_NULL_MESSAGE(handles, "Failed to get schedules from NVS");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, count, "Should have 1 schedule in NVS");

    esp_schedule_config_t rc = {0};
    esp_schedule_get(handles[0], &rc);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(config.name, rc.name, "Schedule names should match");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(config.triggers.count, rc.triggers.count, "Trigger counts should match");
    __match_trigger(&config.triggers.list[0], &rc.triggers.list[0]);
    esp_schedule_config_free_internals(&rc);

    esp_schedule_delete(handle);
    for (int i = 0; i < count; i++) {
        esp_schedule_delete(handles[i]);
    }
    free(handles);
    free(config.triggers.list);

    handles = esp_schedule_nvs_get_all(&count);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, count, "Should have 0 schedules after removal");
    if (handles) {
        free(handles);
    }
}

TEST_CASE("nvs multiple schedules", "[esp_schedule]")
{
    const char *names[3] = {"schedule1", "schedule2", "schedule3"};
    esp_schedule_config_t configs[3] = {0};

    for (int i = 0; i < 3; i++) {
        strcpy(configs[i].name, names[i]);
        configs[i].triggers.count = 1;
        configs[i].triggers.list = (esp_schedule_trigger_t *)calloc(1, sizeof(esp_schedule_trigger_t));
        configs[i].triggers.list[0].type = ESP_SCHEDULE_TYPE_DAYS_OF_WEEK;
        configs[i].triggers.list[0].hours = 8 + i;
        configs[i].triggers.list[0].minutes = i * 15;
        configs[i].triggers.list[0].day.repeat_days = ESP_SCHEDULE_DAY_MONDAY;
        configs[i].validity.end_time = 2147483647;
        esp_schedule_handle_t handle = NULL;
        TEST_ASSERT_EQUAL_INT_MESSAGE(ESP_OK, (int)esp_schedule_create(&configs[i], &handle), "Failed to create schedule");
    }

    uint8_t retrieved_count = 0;
    esp_schedule_handle_t *handles = esp_schedule_nvs_get_all(&retrieved_count);
    TEST_ASSERT_NOT_NULL_MESSAGE(handles, "Failed to get all schedules");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(3, retrieved_count, "Should retrieve 3 schedules");

    bool found_schedules[3] = {false, false, false};
    for (int i = 0; i < retrieved_count; i++) {
        esp_schedule_config_t rc = {0};
        esp_schedule_get(handles[i], &rc);
        for (int j = 0; j < 3; j++) {
            if (strcmp(rc.name, names[j]) == 0) {
                TEST_ASSERT_FALSE_MESSAGE(found_schedules[j], "Duplicate schedule found");
                found_schedules[j] = true;
                __match_trigger(&configs[j].triggers.list[0], &rc.triggers.list[0]);
                break;
            }
        }
        esp_schedule_config_free_internals(&rc);
        esp_schedule_delete(handles[i]);
    }
    free(handles);

    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_TRUE_MESSAGE(found_schedules[i], "Expected schedule not found");
        free(configs[i].triggers.list);
    }

    handles = esp_schedule_nvs_get_all(&retrieved_count);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, retrieved_count, "Should have 0 schedules after removal");
    if (handles) {
        free(handles);
    }
}

TEST_CASE("nvs schedule with multiple triggers", "[esp_schedule]")
{
    esp_schedule_config_t config = {0};
    strcpy(config.name, "multi_trigger");
    config.triggers.count = 3;
    config.triggers.list = (esp_schedule_trigger_t *)calloc(3, sizeof(esp_schedule_trigger_t));
    config.triggers.list[0].type = ESP_SCHEDULE_TYPE_DAYS_OF_WEEK;
    config.triggers.list[0].hours = 8;
    config.triggers.list[0].minutes = 0;
    config.triggers.list[0].day.repeat_days = ESP_SCHEDULE_DAY_MONDAY;
    config.triggers.list[1].type = ESP_SCHEDULE_TYPE_DAYS_OF_WEEK;
    config.triggers.list[1].hours = 14;
    config.triggers.list[1].minutes = 30;
    config.triggers.list[1].day.repeat_days = ESP_SCHEDULE_DAY_WEDNESDAY;
    config.triggers.list[2].type = ESP_SCHEDULE_TYPE_DAYS_OF_WEEK;
    config.triggers.list[2].hours = 18;
    config.triggers.list[2].minutes = 45;
    config.triggers.list[2].day.repeat_days = ESP_SCHEDULE_DAY_FRIDAY;
    config.validity.end_time = 2147483647;

    esp_schedule_handle_t handle = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(ESP_OK, (int)esp_schedule_create(&config, &handle), "Failed to create schedule");

    uint8_t count = 0;
    esp_schedule_handle_t *handles = esp_schedule_nvs_get_all(&count);
    TEST_ASSERT_NOT_NULL_MESSAGE(handles, "Failed to get schedules from NVS");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, count, "Should have 1 schedule in NVS");

    esp_schedule_config_t rc = {0};
    esp_schedule_get(handles[0], &rc);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(3, rc.triggers.count, "Should have 3 triggers");
    for (int i = 0; i < 3; i++) {
        __match_trigger(&config.triggers.list[i], &rc.triggers.list[i]);
    }
    esp_schedule_config_free_internals(&rc);

    esp_schedule_delete(handle);
    for (int i = 0; i < count; i++) {
        esp_schedule_delete(handles[i]);
    }
    free(handles);
    free(config.triggers.list);
}

TEST_CASE("nvs delete all", "[esp_schedule]")
{
    esp_schedule_config_t configs[3] = {0};
    esp_schedule_handle_t handles[3] = {NULL, NULL, NULL};
    const char *names[3] = {"delete_test1", "delete_test2", "delete_test3"};

    for (int i = 0; i < 3; i++) {
        strcpy(configs[i].name, names[i]);
        configs[i].triggers.count = 1;
        configs[i].triggers.list = (esp_schedule_trigger_t *)calloc(1, sizeof(esp_schedule_trigger_t));
        configs[i].triggers.list[0].type = ESP_SCHEDULE_TYPE_DAYS_OF_WEEK;
        configs[i].triggers.list[0].hours = 9 + i;
        configs[i].triggers.list[0].day.repeat_days = ESP_SCHEDULE_DAY_MONDAY;
        configs[i].validity.end_time = 2147483647;
        TEST_ASSERT_EQUAL_INT_MESSAGE(ESP_OK, (int)esp_schedule_create(&configs[i], &handles[i]), "Failed to create schedule");
    }

    uint8_t count_before = 0;
    esp_schedule_handle_t *all_before = esp_schedule_nvs_get_all(&count_before);
    TEST_ASSERT_NOT_NULL_MESSAGE(all_before, "Failed to get schedules from NVS");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(3, count_before, "Should have 3 schedules before delete_all");
    free(all_before);

    TEST_ASSERT_EQUAL_INT_MESSAGE(ESP_OK, (int)esp_schedule_delete_all(handles, 3), "delete_all should succeed");

    uint8_t count_after = 0;
    esp_schedule_handle_t *all_after = esp_schedule_nvs_get_all(&count_after);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, count_after, "Should have 0 schedules after delete_all");
    if (all_after) {
        free(all_after);
    }
    for (int i = 0; i < 3; i++) {
        free(configs[i].triggers.list);
    }
}

TEST_CASE("nvs blob has no live pointer", "[esp_schedule]")
{
    esp_schedule_config_t config = {0};
    strcpy(config.name, "ptrtest");
    config.triggers.count = 2;
    config.triggers.list = (esp_schedule_trigger_t *)calloc(2, sizeof(esp_schedule_trigger_t));
    config.triggers.list[0].type = ESP_SCHEDULE_TYPE_DAYS_OF_WEEK;
    config.triggers.list[0].hours = 8;
    config.triggers.list[0].day.repeat_days = ESP_SCHEDULE_DAY_MONDAY;
    config.triggers.list[1].type = ESP_SCHEDULE_TYPE_DAYS_OF_WEEK;
    config.triggers.list[1].hours = 9;
    config.triggers.list[1].day.repeat_days = ESP_SCHEDULE_DAY_TUESDAY;
    config.validity.end_time = 2147483647;

    esp_schedule_handle_t handle = NULL;
    TEST_ASSERT_EQUAL_INT(ESP_OK, (int)esp_schedule_create(&config, &handle));

    nvs_handle_t h;
    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_open_from_partition("nvs", "schd", NVS_READONLY, &h));
    size_t blob_size = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_get_blob(h, "ptrtest", NULL, &blob_size));
    size_t expected = sizeof(esp_schedule_persistent_t) + 2 * sizeof(esp_schedule_trigger_t);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(expected, blob_size, "blob size must be header + triggers only (no live pointer)");

    uint8_t *buf = malloc(blob_size);
    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_get_blob(h, "ptrtest", buf, &blob_size));
    esp_schedule_persistent_t *p = (esp_schedule_persistent_t *)buf;
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(ESP_SCHEDULE_NVS_FORMAT_VERSION, p->version, "version byte");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, p->trigger_count, "trigger count persisted");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(sizeof(esp_schedule_trigger_t), p->trigger_size, "trigger size persisted");
    free(buf);
    nvs_close(h);

    esp_schedule_delete(handle);
    free(config.triggers.list);
}

TEST_CASE("nvs rejects invalid blobs", "[esp_schedule]")
{
    nvs_handle_t h;

    /* (a) Blob shorter than the persistent header. */
    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_open_from_partition("nvs", "schd", NVS_READWRITE, &h));
    uint8_t tiny[4] = {0};
    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_set_blob(h, "shorty", tiny, sizeof(tiny)));
    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_set_u8(h, "schd_count", 1));
    nvs_commit(h);
    nvs_close(h);
    uint8_t count = 0;
    esp_schedule_handle_t *handles = esp_schedule_nvs_get_all(&count);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, count, "short blob must be rejected");
    free(handles);

    /* (b) Correct header size but wrong version byte. */
    nvs_erase_schd();
    esp_schedule_persistent_t p = {0};
    p.version = ESP_SCHEDULE_NVS_FORMAT_VERSION + 99;
    p.trigger_count = 0;
    p.trigger_size = sizeof(esp_schedule_trigger_t);
    strcpy(p.name, "badver");
    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_open_from_partition("nvs", "schd", NVS_READWRITE, &h));
    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_set_blob(h, "badver", &p, sizeof(p)));
    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_set_u8(h, "schd_count", 1));
    nvs_commit(h);
    nvs_close(h);
    count = 0;
    handles = esp_schedule_nvs_get_all(&count);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, count, "bad-version blob must be rejected");
    free(handles);

    /* (c) Header claims more triggers than the blob holds. */
    nvs_erase_schd();
    memset(&p, 0, sizeof(p));
    p.version = ESP_SCHEDULE_NVS_FORMAT_VERSION;
    p.trigger_count = 5;
    p.trigger_size = sizeof(esp_schedule_trigger_t);
    strcpy(p.name, "trunc");
    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_open_from_partition("nvs", "schd", NVS_READWRITE, &h));
    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_set_blob(h, "trunc", &p, sizeof(p)));
    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_set_u8(h, "schd_count", 1));
    nvs_commit(h);
    nvs_close(h);
    count = 0;
    handles = esp_schedule_nvs_get_all(&count);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, count, "truncated-triggers blob must be rejected");
    free(handles);
}

TEST_CASE("nvs count less than blobs", "[esp_schedule]")
{
    esp_schedule_config_t config = {0};
    esp_schedule_handle_t handle = NULL;
    for (int i = 0; i < 2; i++) {
        memset(&config, 0, sizeof(config));
        snprintf(config.name, sizeof(config.name), "ov%d", i);
        config.triggers.count = 1;
        config.triggers.list = (esp_schedule_trigger_t *)calloc(1, sizeof(esp_schedule_trigger_t));
        config.triggers.list[0].type = ESP_SCHEDULE_TYPE_DAYS_OF_WEEK;
        config.triggers.list[0].day.repeat_days = ESP_SCHEDULE_DAY_MONDAY;
        config.validity.end_time = 2147483647;
        TEST_ASSERT_EQUAL_INT(ESP_OK, (int)esp_schedule_create(&config, &handle));
        free(config.triggers.list);
    }

    /* Corrupt the count key to under-report (simulates power loss). */
    nvs_handle_t h;
    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_open_from_partition("nvs", "schd", NVS_READWRITE, &h));
    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_set_u8(h, "schd_count", 1));
    nvs_commit(h);
    nvs_close(h);

    uint8_t count = 0;
    esp_schedule_handle_t *handles = esp_schedule_nvs_get_all(&count);
    TEST_ASSERT_TRUE_MESSAGE(count <= 1, "must clamp to the count-sized array");
    if (handles) {
        for (int i = 0; i < count; i++) {
            if (handles[i]) {
                esp_schedule_delete(handles[i]);
            }
        }
        free(handles);
    }
}

TEST_CASE("edit preserves new triggers", "[esp_schedule]")
{
    esp_schedule_config_t config = {0};
    strcpy(config.name, "edittest");
    config.triggers.count = 2;
    config.triggers.list = (esp_schedule_trigger_t *)calloc(2, sizeof(esp_schedule_trigger_t));
    for (int i = 0; i < 2; i++) {
        config.triggers.list[i].type = ESP_SCHEDULE_TYPE_DAYS_OF_WEEK;
        config.triggers.list[i].hours = 8 + i;
        config.triggers.list[i].day.repeat_days = ESP_SCHEDULE_DAY_MONDAY;
    }
    config.validity.end_time = 2147483647;

    esp_schedule_handle_t handle = NULL;
    TEST_ASSERT_EQUAL_INT(ESP_OK, (int)esp_schedule_create(&config, &handle));
    free(config.triggers.list);

    esp_schedule_config_t edit = {0};
    strcpy(edit.name, "edittest");
    edit.triggers.count = 3;
    edit.triggers.list = (esp_schedule_trigger_t *)calloc(3, sizeof(esp_schedule_trigger_t));
    for (int i = 0; i < 3; i++) {
        edit.triggers.list[i].type = ESP_SCHEDULE_TYPE_DAYS_OF_WEEK;
        edit.triggers.list[i].hours = 14 + i;
        edit.triggers.list[i].day.repeat_days = ESP_SCHEDULE_DAY_FRIDAY;
    }
    edit.validity.end_time = 2147483647;
    TEST_ASSERT_EQUAL_INT(ESP_OK, (int)esp_schedule_edit(handle, &edit));

    esp_schedule_config_t got = {0};
    TEST_ASSERT_EQUAL_INT(ESP_OK, (int)esp_schedule_get(handle, &got));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(3, got.triggers.count, "edit must install the 3 new triggers");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(14, got.triggers.list[0].hours, "trigger[0] hours");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(16, got.triggers.list[2].hours, "trigger[2] hours");
    esp_schedule_config_free_internals(&got);

    esp_schedule_delete(handle);
    free(edit.triggers.list);
}

TEST_CASE("nvs remove count no underflow", "[esp_schedule]")
{
    esp_schedule_config_t config = {0};
    strcpy(config.name, "f2test");
    config.triggers.count = 1;
    config.triggers.list = (esp_schedule_trigger_t *)calloc(1, sizeof(esp_schedule_trigger_t));
    config.triggers.list[0].type = ESP_SCHEDULE_TYPE_DAYS_OF_WEEK;
    config.triggers.list[0].day.repeat_days = ESP_SCHEDULE_DAY_MONDAY;
    config.validity.end_time = 2147483647;
    esp_schedule_handle_t handle = NULL;
    TEST_ASSERT_EQUAL_INT(ESP_OK, (int)esp_schedule_create(&config, &handle));
    free(config.triggers.list);

    /* Desync: force the count key to 0 while the blob still exists. */
    nvs_handle_t h;
    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_open_from_partition("nvs", "schd", NVS_READWRITE, &h));
    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_set_u8(h, "schd_count", 0));
    nvs_commit(h);
    nvs_close(h);

    esp_schedule_delete(handle);

    uint8_t c = 0xEE;
    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_open_from_partition("nvs", "schd", NVS_READONLY, &h));
    nvs_get_u8(h, "schd_count", &c);
    nvs_close(h);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, c, "count must stay 0, not underflow to 255");
}

TEST_CASE("get empty trigger list", "[esp_schedule]")
{
    esp_schedule_config_t config = {0};
    strcpy(config.name, "f3test");
    config.triggers.count = 1;
    config.triggers.list = (esp_schedule_trigger_t *)calloc(1, sizeof(esp_schedule_trigger_t));
    config.triggers.list[0].type = ESP_SCHEDULE_TYPE_DAYS_OF_WEEK;
    config.triggers.list[0].day.repeat_days = ESP_SCHEDULE_DAY_MONDAY;
    config.validity.end_time = 2147483647;
    esp_schedule_handle_t handle = NULL;
    TEST_ASSERT_EQUAL_INT(ESP_OK, (int)esp_schedule_create(&config, &handle));
    free(config.triggers.list);

    esp_schedule_config_t empty = {0};
    strcpy(empty.name, "f3test");
    empty.triggers.count = 0;
    empty.triggers.list = NULL;
    empty.validity.end_time = 2147483647;
    TEST_ASSERT_EQUAL_INT(ESP_OK, (int)esp_schedule_edit(handle, &empty));

    esp_schedule_config_t got = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ESP_OK, (int)esp_schedule_get(handle, &got), "get on empty list must succeed");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, got.triggers.count, "count must be 0");
    TEST_ASSERT_NULL_MESSAGE(got.triggers.list, "list must be NULL");
    esp_schedule_config_free_internals(&got);

    esp_schedule_delete(handle);
}

TEST_CASE("nvs add at max count no orphan", "[esp_schedule]")
{
    /* Force the count key to the maximum. */
    nvs_handle_t h;
    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_open_from_partition("nvs", "schd", NVS_READWRITE, &h));
    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_set_u8(h, "schd_count", UINT8_MAX));
    nvs_commit(h);
    nvs_close(h);

    esp_schedule_config_t config = {0};
    strcpy(config.name, "orphan_test");
    config.triggers.count = 1;
    config.triggers.list = (esp_schedule_trigger_t *)calloc(1, sizeof(esp_schedule_trigger_t));
    config.triggers.list[0].type = ESP_SCHEDULE_TYPE_DAYS_OF_WEEK;
    config.triggers.list[0].day.repeat_days = ESP_SCHEDULE_DAY_MONDAY;
    config.validity.end_time = 2147483647;

    esp_schedule_handle_t handle = NULL;
    esp_err_t ret = esp_schedule_create(&config, &handle);
    free(config.triggers.list);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ESP_ERR_NO_MEM, (int)ret, "create must fail when count is at max");
    TEST_ASSERT_NULL_MESSAGE(handle, "no handle on failed create");

    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_open_from_partition("nvs", "schd", NVS_READONLY, &h));
    size_t sz = 0;
    esp_err_t e = nvs_get_blob(h, "orphan_test", NULL, &sz);
    nvs_close(h);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ESP_ERR_NVS_NOT_FOUND, (int)e, "rejected add must not leave an orphan blob");
}

/* --- pre-2.0 (v1) blobs must be migrated on load --- */
/* Replica of the frozen v1 on-flash layout (a raw v1 esp_schedule_t). Must match
 * esp_schedule_migrate_from_v1()'s expected size exactly. */
typedef struct {
    char name[MAX_SCHEDULE_NAME_LEN + 1];
    esp_schedule_trigger_t trigger;
    uint32_t next_scheduled_time_diff;
    void *timer;
    void *trigger_cb;
    void *timestamp_cb;
    void *priv_data;
    esp_schedule_validity_t validity;
} test_legacy_v1_t;

TEST_CASE("nvs migrate from v1", "[esp_schedule]")
{
    /* Hand-write a v1 blob (raw struct, no version byte). */
    test_legacy_v1_t v1 = {0};
    strcpy(v1.name, "legacy1");
    v1.trigger.type = ESP_SCHEDULE_TYPE_DAYS_OF_WEEK;
    v1.trigger.hours = 7;
    v1.trigger.minutes = 15;
    v1.trigger.day.repeat_days = ESP_SCHEDULE_DAY_TUESDAY;
    v1.validity.start_time = 0;
    v1.validity.end_time = 2147483647;

    nvs_handle_t h;
    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_open_from_partition("nvs", "schd", NVS_READWRITE, &h));
    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_set_blob(h, "legacy1", &v1, sizeof(v1)));
    TEST_ASSERT_EQUAL_INT(ESP_OK, nvs_set_u8(h, "schd_count", 1));
    nvs_commit(h);
    nvs_close(h);

    uint8_t count = 0;
    esp_schedule_handle_t *handles = esp_schedule_nvs_get_all(&count);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, count, "v1 blob should migrate to one schedule");
    TEST_ASSERT_NOT_NULL(handles);

    esp_schedule_config_t got = {0};
    TEST_ASSERT_EQUAL_INT(ESP_OK, (int)esp_schedule_get(handles[0], &got));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("legacy1", got.name, "migrated name");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, got.triggers.count, "migrated to a single-trigger list");
    __match_trigger(&v1.trigger, &got.triggers.list[0]);
    esp_schedule_config_free_internals(&got);

    for (int i = 0; i < count; i++) {
        esp_schedule_delete(handles[i]);
    }
    free(handles);
}

/* --- far-future validity.start_time must still resolve --- */
TEST_CASE("date far future validity start", "[esp_schedule]")
{
    time_t now = make_time_utc(2025, 1, 1, 0, 0, 0);
    time_t start = now + 3L * 365 * 24 * 3600; /* ~3 years out */
    esp_schedule_validity_t validity = { .start_time = start, .end_time = start + 30L * 24 * 3600 };

    time_t next_ts = 0;
    bool ok = esp_schedule_get_next_date_time(now, 8 * 60, ESP_SCHEDULE_DAY_EVERYDAY, 0, 0, 0, &validity, &next_ts);
    TEST_ASSERT_TRUE_MESSAGE(ok, "far-future start must still find an occurrence");
    TEST_ASSERT_TRUE_MESSAGE(next_ts >= start, "occurrence must be within the validity window");
    TEST_ASSERT_TRUE_MESSAGE(next_ts < start + 24 * 3600, "occurrence must be the first day of the window");
}

TEST_CASE("date validity start exact boundary", "[esp_schedule]")
{
    time_t now = make_time_utc(2025, 1, 1, 0, 0, 0);
    time_t start = make_time_utc(2027, 6, 15, 8, 0, 0); /* ~29 months out, on an 08:00 slot */
    esp_schedule_validity_t validity = { .start_time = start, .end_time = start + 30L * 24 * 3600 };

    time_t next_ts = 0;
    bool ok = esp_schedule_get_next_date_time(now, 8 * 60, ESP_SCHEDULE_DAY_EVERYDAY, 0, 0, 0, &validity, &next_ts);
    TEST_ASSERT_TRUE_MESSAGE(ok, "far-future window-open occurrence must be found");
    assert_time_eq("window-open day occurrence returned (not skipped)", next_ts, start);
}

TEST_CASE("start_timer stops on time loss", "[esp_schedule]")
{
    struct timeval tv = { .tv_sec = make_time_utc(2025, 6, 15, 12, 0, 0), .tv_usec = 0 };
    settimeofday(&tv, NULL);

    esp_schedule_config_t config = {0};
    strcpy(config.name, "f5test");
    config.triggers.count = 1;
    config.triggers.list = (esp_schedule_trigger_t *)calloc(1, sizeof(esp_schedule_trigger_t));
    config.triggers.list[0].type = ESP_SCHEDULE_TYPE_DAYS_OF_WEEK;
    config.triggers.list[0].hours = 8;
    config.triggers.list[0].day.repeat_days = ESP_SCHEDULE_DAY_EVERYDAY;
    config.validity.end_time = 2147483647;
    esp_schedule_handle_t handle = NULL;
    TEST_ASSERT_EQUAL_INT(ESP_OK, (int)esp_schedule_create(&config, &handle));
    free(config.triggers.list);

    esp_schedule_enable(handle);
    esp_schedule_t *sched = (esp_schedule_t *)handle;
    TEST_ASSERT_TRUE_MESSAGE(sched->next_scheduled_time_utc > 0, "schedule should be armed with a future time");

    /* Simulate RTC loss: jump the clock before 2020. */
    struct timeval bad = { .tv_sec = make_time_utc(2010, 1, 1, 0, 0, 0), .tv_usec = 0 };
    settimeofday(&bad, NULL);

    esp_schedule_enable(handle);
    TEST_ASSERT_EQUAL_MESSAGE(0, sched->next_scheduled_time_utc, "stale time must be cleared when time is invalid");

    settimeofday(&tv, NULL);
    esp_schedule_delete(handle);
}
