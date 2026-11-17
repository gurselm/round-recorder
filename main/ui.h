#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* The round screen: the recorder's state, Save, and the saved clips. */
esp_err_t ui_start(void);

/* From the buttons task. */
bool ui_awake(void);
uint32_t ui_idle_ms(void);   /* since the last touch or wake */
void ui_wake(void);
void ui_sleep(void);
void ui_sleep_if_idle(uint32_t idle_ms);   /* awake and untouched that long: sleep */
void ui_show_off(void);      /* "Off" before the power goes */
bool ui_take_power_off(void); /* true once after Power off in Settings */
bool ui_take_wake_request(void); /* true once after a tap on the dim ring */
void ui_quiet_save(void);     /* the next save is PWR's: no name prompt, it may be in a pocket */
void ui_nothing_new(void);     /* a save with nothing new since the last: says so, with the low pair */
void ui_battery_low(bool low); /* shown until it's false again */

/* For tools/snap.py: opens a screen (main, list, settings, dial (the mic
 * gain's), confirm, name, keyboard, manage or player, the last four on the
 * newest recording; NULL or "" for the one showing) and renders it: w * h
 * RGB565 pixels, little-endian, rows one after another, in PSRAM to free().
 * Wakes the screen. */
esp_err_t ui_snapshot(const char *screen, uint8_t **rgb565, uint32_t *w, uint32_t *h);

/* RR TAP: presses the screen at x, y for ms, through LVGL as a finger would. */
void ui_tap(int x, int y, int ms);
