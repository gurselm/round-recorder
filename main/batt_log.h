#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "pmu.h"

/*
 * A log of the battery through a discharge, to measure how long it lasts:
 * from coming off USB, an entry a minute (its voltage and percent, and
 * whether the screen was on or a recording going), until USB is back or the
 * board powers itself off. It's kept in flash past the last recording slot,
 * so the power-off at low battery doesn't lose it; tools/pull.py --battery
 * reads it (RR BATT). Coming off USB again starts a new log. Turned on away
 * from USB with a log that didn't end on USB, it carries that log on.
 */

enum {
    BATT_USB = 1,          /* USB came back: the log ends */
    BATT_CHARGING = 2,
    BATT_SCREEN = 4,       /* the screen was on during the minute */
    BATT_RECORDING = 8,    /* a recording was going on during the minute */
    BATT_OFF = 16,         /* the board powered off */
    BATT_BOOT = 32,        /* turned on again since the entry before */
    BATT_STANDBY = 64,     /* in standby during the minute: asleep between checks for sound */
};

/* The flash past the last recording slot (recorder_spare()); does nothing
 * if there's under 64 KB of it. */
esp_err_t batt_log_init(void);

/* Every few seconds, with the power chip's reading. */
void batt_log_sample(const pmu_power_t *p, bool screen_on, bool recording, bool standby);

/* Right before powering off. */
void batt_log_power_off(void);

/* For RR BATT: the log's entry i, oldest first; 0 if there is one. */
int batt_log_get(uint32_t i, uint32_t *minute, uint32_t *mv, uint32_t *pct, uint32_t *flags);
