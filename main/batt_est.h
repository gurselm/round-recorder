#pragma once

/* Hours left on battery, from how long it took to fall from start_pct to pct.
 * The power chip's percent doesn't fall evenly (the top half lasts about
 * three times as long as the bottom half), so the percents are first turned
 * into how much of a discharge they stand for, from one measured on this
 * board; the load still comes from the time it took. Negative while there's
 * nothing to go on (no fall yet). */
double batt_hours_left(int start_pct, int pct, double hours_since);
