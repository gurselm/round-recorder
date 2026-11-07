#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * The settings the screen can change, kept in NVS. Until one is changed it
 * has its Kconfig default. A change takes effect at once and is written to
 * flash by settings_commit() a second later, so dragging a slider doesn't
 * write flash at every step.
 */

/* Opens NVS and loads them; before audio_init(). */
esp_err_t settings_init(void);

int settings_mic_gain_db(void);    /* 0 to 36, in 3 dB steps */
int settings_volume(void);         /* 0 to 100 */
int settings_screen_off_s(void);
bool settings_sounds(void);        /* the start, stop and failure sounds */
int settings_utc_offset(void);     /* seconds, as pull.py last set the clock */
int settings_brightness(void);     /* the screen's, 10 to 100 % */
bool settings_aod(void);           /* with the screen off, the ring stays on, dim */
bool settings_tap_wake(void);      /* a tap on the dim ring wakes the screen */
bool settings_intro_seen(void);    /* the buttons' first-run screen, once */
bool settings_tour_seen(void);     /* the tour, shown the first time ? is tapped */
uint32_t settings_boot(void);      /* this boot's number, counted across restarts */

/* These also apply the gain or volume to the codecs. */
void settings_set_mic_gain_db(int db);
void settings_set_volume(int volume);
void settings_set_screen_off_s(int s);
void settings_set_sounds(bool on);
void settings_set_utc_offset(int s);
void settings_set_brightness(int pct);   /* ui.c sends it to the panel */
void settings_set_aod(bool on);
void settings_set_tap_wake(bool on);
void settings_set_intro_seen(void);
void settings_set_tour_seen(void);

/* Writes changed settings once they've been left alone a second. From a
 * task with an internal-RAM stack, as it writes flash. */
void settings_commit(void);
