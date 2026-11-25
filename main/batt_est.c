#include "batt_est.h"

/* The minute each 5 % was reached in a full discharge on this board
 * (2026-11-25: recording, screen off, 5 h 57 min from 100 % to the
 * power-off at 3 %), from 0 % up to 100 %. 0 % isn't reached: the 3 %
 * power-off at minute 357 stands in below 5 %. */
static const short MINUTE_AT[21] = {
    357, 351, 334, 320, 311, 305, 299, 293, 287, 280, 271,   /* 0..50 % */
    257, 233, 199, 170, 149, 126, 91, 43, 17, 0,             /* 55..100 % */
};
#define OFF_PCT 3
#define OFF_MINUTE 357

/* How far into that discharge a percent is, in its minutes. */
static double used(int pct)
{
    if (pct <= OFF_PCT) {
        return OFF_MINUTE;
    }
    if (pct >= 100) {
        return 0;
    }
    if (pct < 5) {
        return OFF_MINUTE + (MINUTE_AT[1] - OFF_MINUTE) * (pct - OFF_PCT) / (5.0 - OFF_PCT);
    }
    int i = pct / 5;
    return MINUTE_AT[i] + (MINUTE_AT[i + 1] - MINUTE_AT[i]) * (pct % 5) / 5.0;
}

double batt_hours_left(int start_pct, int pct, double hours_since)
{
    double gone = used(pct) - used(start_pct);
    if (gone <= 0 || hours_since <= 0) {
        return -1;
    }
    return hours_since * (OFF_MINUTE - used(pct)) / gone;
}
