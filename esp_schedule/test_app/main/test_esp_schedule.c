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
}

void tearDown(void)
{
    tz_pop();
}
