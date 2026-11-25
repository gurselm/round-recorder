#include "ui.h"

#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "lvgl_private.h"   /* lv_hit_test_info_t, for the bands' touch areas */
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "batt_est.h"
#include "pmu.h"
#include "rec_ring.h"
#include "recorder.h"
#include "multitap.h"
#include "settings.h"

static const char *TAG = "ui";

#define DRAW_LINES 50
#define MAX_ROWS 64
#define NAME_PROMPT_MS 20000   /* the name prompt after a save gives up after this */
#define NAME_TYPED_MAX 24      /* characters typed on the screen; pull.py --name takes more */
#define RED lv_color_hex(0xE53935)
#define GREY lv_color_hex(0x424242)
#define GREEN lv_color_hex(0x2E7D32)
#define AMBER lv_color_hex(0xEF6C00)
#define DIM lv_color_hex(0x9E9E9E)
/* The main screen's bands: a lighter grey than before, with white words,
 * to stand out from the black (3:1 against it; the old dark grey was
 * 1.4:1). Pressed, darker. Amber and red lightened to read on them (4 and
 * 3:1); the paused Pause band is amber with dark words, as SAVE is. */
#define BAND_FILL lv_color_hex(0x5A5A5A)
#define BAND_PRESSED lv_color_hex(0x3A3A3A)
#define BAND_INK lv_color_white()
#define BAND_SUB lv_color_hex(0xD0D0D0)
#define BAND_AMBER lv_color_hex(0xFFB74D)
#define BAND_RED lv_color_hex(0xFF8A80)
#define CONTINUE_MS 10000      /* after a PWR save, PWR again keeps recording for this long (main.c) */

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_touch_handle_t s_tp;
static lv_indev_t *s_touch;
/* RR TAP: a pointer of its own that presses where it's told, as a finger
 * would, for checking flows from the laptop (tools/snap.py --tap). */
static lv_indev_t *s_fake;
static atomic_int s_fake_x, s_fake_y;
static atomic_llong s_fake_until;   /* us; pressed until then */
static atomic_bool s_awake = true;
static atomic_bool s_power_off;
static atomic_bool s_slept;   /* since the last refresh: drop a half-done name */

static const int OFF_STEPS[] = { 10, 15, 30, 60, 120, 300 };   /* screen-off choices, seconds */
#define OFF_N ((int)(sizeof(OFF_STEPS) / sizeof(OFF_STEPS[0])))

static lv_obj_t *s_main, *s_list_screen, *s_settings_screen, *s_ring;

/* The settings a dial edits (Settings' rows), and the dial's screen. */
enum { SET_GAIN, SET_VOLUME, SET_BRIGHT, SET_OFF, SET_DIALS, SET_SOUNDS = SET_DIALS, SET_AOD, SET_TAP, SET_ROWS };
static lv_obj_t *s_set_list, *s_set_sub[SET_ROWS], *s_storage_sub;
static lv_obj_t *s_c_title, *s_c_why, *s_c_icon, *s_c_caption;   /* the confirm screen's */
static void (*s_confirm_do)(void);
static lv_obj_t *s_edit, *s_e_arc, *s_e_title, *s_e_value, *s_e_level;
static int s_edit_kind;
static lv_obj_t *s_name_screen, *s_name_title, *s_kb_screen, *s_kb_ta;
static multitap_t s_tap;
static lv_obj_t *s_kb_keys[MULTITAP_KEYS], *s_kb_mode, *s_kb_hint;   /* key labels, by key */
static lv_obj_t *s_confirm;
static lv_obj_t *s_confirm_back;   /* where Cancel goes: the screen that asked */
static lv_obj_t *s_name_back;          /* the screen naming goes back to */
static uint32_t s_name_slot, s_name_seq;
static lv_obj_t *s_name_list, *s_name_keep, *s_name_keep_label, *s_name_cont;
static lv_obj_t *s_manage, *s_m_title, *s_m_keep, *s_m_keep_label, *s_m_name;
static uint32_t s_manage_first;
static lv_obj_t *s_manage_back;   /* the list, or the player it came from */
static lv_obj_t *s_undo;
static int64_t s_undo_until;

static const char *const PRESETS[] = { "Idea", "To-do", "Meeting", "Call", "Note", "Music", "Funny", "Remember" };

static lv_obj_t *s_held, *s_save, *s_save_label, *s_save_sub, *s_batt, *s_pwr_arc;
static atomic_bool s_quiet;        /* the last save came from PWR: no name prompt, in case it's in a pocket */
static atomic_bool s_battery_low;
/* With the screen "off" and the always-on ring set: the panel stays on, dim,
 * showing only the ring, so it's plain it's listening, or recording. */
static lv_obj_t *s_aod, *s_aod_ring, *s_aod_text, *s_aod_back;
static atomic_bool s_aod_on, s_wake_req;
/* Sleep and wake take turns: a wake from another task (a screenshot) mid-sleep
 * would leave it "awake" with the dim ring up, and the screen not updating. */
static SemaphoreHandle_t s_power_lock;
static bool s_touch_asleep;
static lv_obj_t *s_intro;
static lv_obj_t *s_list;
static bool s_list_opened;             /* fill_list() centres the first recording */
static uint32_t s_list_changes = UINT32_MAX;
static uint32_t s_list_going;          /* the recording going on when the list was filled */
static lv_obj_t *s_player, *s_p_arc, *s_p_title, *s_p_time, *s_p_play_label, *s_p_speed_label;
static char s_p_title_text[REC_NAME_MAX + 32];   /* what the title says when it isn't the volume */
static int64_t s_p_volume_until;                 /* us: the title shows the volume until then */
static uint32_t s_player_first, s_player_blocks;
static rec_recording_t *s_rows;        /* MAX_ROWS of them, PSRAM: the list's, on the LVGL task */

/* ---- the panel ---- */

/* The CO5300 needs even-aligned update windows. */
static void round_area(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

static void panel_cmd(uint8_t cmd, const void *param, size_t n)
{
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | (cmd << 8), param, n);
}

/* The CO5300's "write display brightness", 0 to 255, from Settings' percent. */
static void set_brightness(int pct)
{
    panel_cmd(0x51, (uint8_t[]){ (uint8_t)(pct * 255 / 100) }, 1);
}

static void fake_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    data->point.x = atomic_load(&s_fake_x);
    data->point.y = atomic_load(&s_fake_y);
    data->state = esp_timer_get_time() < atomic_load(&s_fake_until) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void ui_tap(int x, int y, int ms)
{
    atomic_store(&s_fake_x, x);
    atomic_store(&s_fake_y, y);
    atomic_store(&s_fake_until, esp_timer_get_time() + (int64_t)ms * 1000);
}

static esp_err_t display_start(void)
{
    esp_lv_adapter_config_t cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    cfg.task_core_id = 0;
    ESP_RETURN_ON_ERROR(esp_lv_adapter_init(&cfg), TAG, "lvgl adapter");
    esp_lcd_panel_handle_t panel;
    const bsp_display_config_t panel_cfg = {
        .max_transfer_sz = BSP_LCD_H_RES * DRAW_LINES * 2,
    };
    ESP_RETURN_ON_ERROR(bsp_display_new(&panel_cfg, &panel, &s_io), TAG, "panel");
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = panel,
        .panel_io = s_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = BSP_LCD_H_RES,
            .ver_res = BSP_LCD_V_RES,
            .buffer_height = DRAW_LINES,
            .use_psram = false,   /* SPI DMA straight from PSRAM underflows */
            .enable_ppa_accel = false,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    ESP_RETURN_ON_FALSE(disp, ESP_FAIL, TAG, "display");
    lv_display_add_event_cb(disp, round_area, LV_EVENT_INVALIDATE_AREA, NULL);

    const bsp_display_cfg_t touch_cfg = {
        .touch_flags = { .mirror_x = 1, .mirror_y = 1 },
    };
    ESP_RETURN_ON_ERROR(bsp_touch_new(&touch_cfg, &s_tp), TAG, "touch");
    const esp_lv_adapter_touch_config_t tp_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, s_tp);
    lv_indev_t *indev = esp_lv_adapter_register_touch(&tp_cfg);
    ESP_RETURN_ON_FALSE(indev, ESP_FAIL, TAG, "touch input");
    s_touch = indev;
    /* A deliberate tap on a small screen can last 400 ms, LVGL's hold, and
     * drift 10 px, its scroll: both then lose the tap. */
    lv_indev_set_long_press_time(indev, 650);
    lv_indev_set_scroll_limit(indev, 16);
    s_fake = lv_indev_create();
    lv_indev_set_type(s_fake, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_fake, fake_read);
    lv_indev_set_long_press_time(s_fake, 650);
    lv_indev_set_scroll_limit(s_fake, 16);
    ESP_RETURN_ON_ERROR(esp_lv_adapter_start(), TAG, "lvgl start");
    set_brightness(settings_brightness());
    return ESP_OK;
}

/*
 * Off: LVGL stops, the CST9217 goes to deep sleep (0xD105, where only its
 * reset wakes it) and the panel to SLPIN. Plain SLPIN rather than the
 * driver's sleep, whose deep standby pulses the reset line it shares with
 * touch.
 */
static void power_lock(void)
{
    if (!s_power_lock) {
        s_power_lock = xSemaphoreCreateRecursiveMutex();
    }
    xSemaphoreTakeRecursive(s_power_lock, portMAX_DELAY);
}

static void power_unlock(void)
{
    xSemaphoreGiveRecursive(s_power_lock);
}

static void sleep_locked(void);
static void wake_locked(void);

void ui_sleep(void)
{
    power_lock();
    sleep_locked();
    power_unlock();
}

void ui_wake(void)
{
    power_lock();
    wake_locked();
    power_unlock();
}

void ui_sleep_if_idle(uint32_t idle_ms)
{
    /* Asked and done under the lock: a wake in between (it takes a while to
     * reset touch) can't have its idle time read stale and be undone. */
    power_lock();
    if (atomic_load(&s_awake) && ui_idle_ms() > idle_ms) {
        sleep_locked();
    }
    power_unlock();
}

static void sleep_locked(void)
{
    if (!atomic_exchange(&s_awake, false)) {
        return;
    }
    ESP_LOGI(TAG, "screen off%s", settings_aod() ? ": dim ring" : "");
    atomic_store(&s_slept, true);
    /* Touch sleeps unless a tap is to wake the dim ring: in deep sleep only
     * its reset wakes it. */
    bool aod = settings_aod(), tap = aod && settings_tap_wake();
    if (!tap) {
        esp_err_t err = esp_lcd_panel_io_tx_param(s_tp->io, 0xD1, (uint8_t[]){ 0x05 }, 1);
        vTaskDelay(pdMS_TO_TICKS(20));
        /* Asleep, it doesn't answer at all: a touch controller still awake
         * would both take taps and use power. */
        bool answers = i2c_master_probe(bsp_i2c_get_handle(), 0x5A, 50) == ESP_OK;
        ESP_LOGI(TAG, "touch to deep sleep: %s, %s", esp_err_to_name(err), answers ? "still answers" : "quiet");
        s_touch_asleep = true;
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            lv_indev_enable(s_touch, false);   /* reading it asleep only fails */
            esp_lv_adapter_unlock();
        }
    }
    if (aod) {
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            s_aod_back = lv_screen_active();
            lv_screen_load(s_aod);
            esp_lv_adapter_unlock();
        }
        set_brightness(4);
        atomic_store(&s_aod_on, true);
        return;
    }
    esp_lv_adapter_pause(-1);
    panel_cmd(0x10, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static void touch_wake(void)
{
    if (s_touch_asleep) {
        gpio_set_level(BSP_LCD_TOUCH_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(BSP_LCD_TOUCH_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
        s_touch_asleep = false;
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            lv_indev_enable(s_touch, true);
            esp_lv_adapter_unlock();
        }
    }
}

static void wake_locked(void)
{
    ESP_LOGI(TAG, "wake (awake %d, ring %d)", atomic_load(&s_awake), atomic_load(&s_aod_on));
    if (atomic_exchange(&s_awake, true)) {
        if (esp_lv_adapter_lock(100) == ESP_OK) {
            lv_display_trigger_activity(NULL);
            esp_lv_adapter_unlock();
        }
        return;
    }
    if (atomic_exchange(&s_aod_on, false)) {
        touch_wake();
        set_brightness(settings_brightness());
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            lv_screen_load(s_aod_back);
            lv_display_trigger_activity(NULL);
            esp_lv_adapter_unlock();
        }
        return;
    }
    panel_cmd(0x11, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
    set_brightness(settings_brightness());
    touch_wake();
    esp_lv_adapter_resume();
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        lv_display_trigger_activity(NULL);
        lv_obj_invalidate(lv_screen_active());
        esp_lv_adapter_unlock();
    }
}

bool ui_take_wake_request(void)
{
    return atomic_exchange(&s_wake_req, false);
}

static atomic_llong s_nothing_until;   /* "Nothing new" on the screen until then */

void ui_nothing_new(void)
{
    atomic_store(&s_nothing_until, esp_timer_get_time() + 2500000);
    recorder_warn();
}

void ui_quiet_save(void)
{
    atomic_store(&s_quiet, true);
}

void ui_battery_low(bool low)
{
    atomic_store(&s_battery_low, low);
}

bool ui_awake(void)
{
    return atomic_load(&s_awake);
}

uint32_t ui_idle_ms(void)
{
    uint32_t ms = 0;
    if (esp_lv_adapter_lock(100) == ESP_OK) {
        ms = lv_display_get_inactive_time(NULL);
        esp_lv_adapter_unlock();
    }
    return ms;
}

void ui_show_off(void)
{
    ui_wake();
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        lv_obj_t *scr = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
        lv_obj_t *l = lv_label_create(scr);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_48, 0);
        lv_label_set_text(l, "Off");
        lv_obj_center(l);
        lv_screen_load(scr);
        esp_lv_adapter_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(600));
}

bool ui_take_power_off(void)
{
    return atomic_exchange(&s_power_off, false);
}

/* ---- screens (LVGL task, or with the adapter's lock) ---- */

/* "45 s", or "2:37" from a minute up. */
static void format_len(uint32_t s, char *out, size_t n)
{
    if (s < 60) {
        snprintf(out, n, "%" PRIu32 " s", s);
    } else {
        snprintf(out, n, "%" PRIu32 ":%02" PRIu32, s / 60, s % 60);
    }
}

/* When a recording was saved. Without the clock at the time, the board's own
 * count since boot still tells it for this boot's recordings: as a time once
 * the clock is set, else as how long ago. */
static void format_when(const rec_recording_t *r, char *out, size_t n)
{
    int64_t unix_time = r->unix_time, now = recorder_time();
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    bool this_boot = r->boot && r->boot == settings_boot() && r->uptime_s <= up;
    if (!unix_time && this_boot && now) {
        unix_time = now - (up - r->uptime_s);
    }
    if (!unix_time) {
        uint32_t min = this_boot ? (up - r->uptime_s) / 60 : 0;
        if (!this_boot) {
            snprintf(out, n, "Recording %" PRIu32, r->first);
        } else if (min < 1) {
            snprintf(out, n, "Just now");
        } else if (min < 60) {
            snprintf(out, n, "%" PRIu32 " min ago", min);
        } else {
            snprintf(out, n, "%" PRIu32 " h ago", min / 60);
        }
        return;
    }
    /* In local time, which the offsets make UTC's; without the clock, there's
     * no today to go by. */
    time_t t = (time_t)(unix_time + recorder_utc_offset());
    time_t today = (time_t)(now + recorder_utc_offset());
    long days = now ? (long)(today / 86400) - (long)(t / 86400) : -1;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, n, days == 0 ? "Today %H:%M" : days == 1 ? "Yesterday %H:%M" : "%b %d %H:%M", &tm);
}

/* ---- the player ---- */

static void open_player(const rec_recording_t *r)
{
    char when[32];
    format_when(r, when, sizeof(when));
    snprintf(s_p_title_text, sizeof(s_p_title_text), "%s", r->name[0] ? r->name : when);
    lv_label_set_text(s_p_title, s_p_title_text);
    s_p_volume_until = 0;
    s_player_first = r->first;
    s_player_blocks = r->blocks;
    lv_arc_set_value(s_p_arc, 0);
    recorder_play(r->first);
    lv_screen_load(s_player);
}

/* Quieter or louder by a tenth, kept as Settings keeps it; the title says
 * how loud for a moment. */
static void on_p_volume(lv_event_t *e)
{
    int by = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    int v = settings_volume() + by;
    settings_set_volume(v < 0 ? 0 : v > 100 ? 100 : v);
    lv_label_set_text_fmt(s_p_title, LV_SYMBOL_VOLUME_MAX "  %d", settings_volume());
    s_p_volume_until = esp_timer_get_time() + 1500000;
}

static void on_p_play(lv_event_t *e)
{
    rec_play_t st;
    recorder_play_status(&st);
    if (st.first != s_player_first) {
        recorder_play(s_player_first);   /* stopped (by a delete, say): from the start */
    } else {
        recorder_play_pause(!st.paused);
    }
}

static void on_p_skip(lv_event_t *e)
{
    int32_t by = (int32_t)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e)) * REC_BLOCKS_PER_S;
    rec_play_t st;
    recorder_play_status(&st);
    int32_t to = (int32_t)st.block + by;
    recorder_play_seek(to < 0 ? 0 : (uint32_t)to > st.blocks ? st.blocks : (uint32_t)to);
}

static void on_p_arc(lv_event_t *e)
{
    recorder_play_seek((uint32_t)((uint64_t)lv_arc_get_value(s_p_arc) * s_player_blocks / 1000));
}

static void show_speed(int pct)
{
    lv_label_set_text(s_p_speed_label, pct == 150 ? "1.5x" : pct == 200 ? "2x" : "1x");
}

static void on_p_speed(lv_event_t *e)
{
    rec_play_t st;
    recorder_play_status(&st);
    int next = st.speed == 100 ? 150 : st.speed == 150 ? 200 : 100;
    recorder_play_speed(next);
    show_speed(next);
}

static void on_p_back(lv_event_t *e)
{
    recorder_play_stop();
    lv_screen_load(s_list_screen);
}

static void open_manage(uint32_t first, lv_obj_t *back);

static void on_p_more(lv_event_t *e)
{
    recorder_play_pause(true);
    open_manage(s_player_first, s_player);
}

/* ---- the list of recordings ---- */

static bool find_row(uint32_t first, rec_recording_t *out)
{
    int n = recorder_recordings(s_rows, MAX_ROWS);
    for (int i = 0; i < n; i++) {
        if (s_rows[i].first == first) {
            *out = s_rows[i];
            return true;
        }
    }
    return false;
}

static void on_row_click(lv_event_t *e)
{
    rec_recording_t r;
    uint32_t first = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (first == recorder_recording()) {
        lv_screen_load(s_main);   /* still going: where its STOP is */
    } else if (find_row(first, &r)) {
        open_player(&r);
    }
}


static void on_row_hold(lv_event_t *e)
{
    uint32_t first = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (first != recorder_recording()) {   /* not while it's still being recorded */
        open_manage(first, s_list_screen);
    }
}

/* ---- lists, as a round watch has them ---- */

#define ROW_H 76
#define ROW_GAP 12             /* room for the middle row's zoom */
#define ROW_BG lv_color_hex(0x2A2A2A)
#define ROW_W 420              /* the most, in the middle: the circle sets the rest */
#define ROW_MIN_W 150
#define ROW_MARGIN 24          /* between a row's end and the rim */
#define ROW_ZOOM_H 6           /* how much taller the middle row's pill is, above and below */

/* Darker by opa, toward the black behind: a row's fade, with no layer. */
static lv_color_t fade_filter(const lv_color_filter_dsc_t *dsc, lv_color_t c, lv_opa_t opa)
{
    LV_UNUSED(dsc);
    return lv_color_darken(c, opa);
}

static const lv_color_filter_dsc_t s_fade_filter = { .filter_cb = fade_filter };

/* Rows fit the circle at their height, as on a round watch: full width in
 * the middle, narrowing toward the top and bottom with the circle, and
 * fading there. The one in the middle, the one you're on, is zoomed in.
 * Nothing here renders through a layer: each frame of a scroll is only a
 * redraw. */
static void on_round_scroll(lv_event_t *e)
{
    lv_obj_t *list = lv_event_get_current_target_obj(e);
    lv_area_t box;
    lv_obj_get_coords(list, &box);
    int32_t mid = (box.y1 + box.y2) / 2;
    const float r = BSP_LCD_H_RES / 2.0f;
    for (uint32_t i = 0; i < lv_obj_get_child_count(list); i++) {
        lv_obj_t *row = lv_obj_get_child(list, (int32_t)i);
        lv_area_t a;
        lv_obj_get_coords(row, &a);
        int32_t off = LV_ABS((a.y1 + a.y2) / 2 - mid);
        /* Its chord a quarter of the way out: the pill's round ends follow
         * the rim beyond that. */
        float edge = (float)(off + ROW_H / 4);
        int32_t w = edge < r ? (int32_t)(2.0f * sqrtf(r * r - edge * edge)) - 2 * ROW_MARGIN : 0;
        w = w > ROW_W ? ROW_W : w < ROW_MIN_W ? ROW_MIN_W : w;
        if (lv_obj_get_width(row) != w) {
            lv_obj_set_width(row, w);
        }
        /* The row in the middle, the one you're on, zoomed in: its pill
         * grows taller as it nears the middle (drawn bigger, the layout
         * untouched) and its title steps up a size. No transform scale:
         * that renders the row through a layer, which made each frame of
         * a scroll take 2.4 times as long. */
        int32_t step = ROW_H + ROW_GAP;
        int32_t grow = off < step ? ROW_ZOOM_H * (step - off) / step : 0;
        if (lv_obj_get_style_transform_height(row, 0) != grow) {
            lv_obj_set_style_transform_height(row, grow, 0);
        }
        lv_obj_t *title = lv_obj_get_child(row, 0);
        const lv_font_t *big = title ? lv_obj_get_user_data(title) : NULL;   /* round_row() picked it */
        if (big) {
            const lv_font_t *font = off < step / 3 ? big : (big == &lv_font_montserrat_34 ? &lv_font_montserrat_28
                                                                                           : &lv_font_montserrat_20);
            if (lv_obj_get_style_text_font(title, 0) != font) {
                lv_obj_set_style_text_font(title, font, 0);
                lv_obj_set_height(title, lv_font_get_line_height(font));
            }
        }
        /* Fading toward the edges by darkening its colours, which its
         * labels inherit: opacity blends every pixel, a third more per frame. */
        int32_t dim = off * 215 / (BSP_LCD_V_RES / 2);
        dim = dim > 215 ? 215 : dim;
        if (lv_obj_get_style_color_filter_opa(row, 0) != dim) {
            lv_obj_set_style_color_filter_opa(row, (lv_opa_t)dim, 0);
        }
    }
}

/* A column of rows down the middle of the screen, whichever is nearest the
 * middle snapping to it: the first and last can reach it too. */
static lv_obj_t *round_list(lv_obj_t *parent)
{
    lv_obj_t *l = lv_obj_create(parent);
    lv_obj_remove_style_all(l);
    lv_obj_set_size(l, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_center(l);
    lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(l, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(l, BSP_LCD_V_RES / 2 - ROW_H / 2, 0);
    lv_obj_set_style_pad_row(l, ROW_GAP, 0);
    lv_obj_set_scroll_dir(l, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(l, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(l, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(l, on_round_scroll, LV_EVENT_SCROLL, NULL);
    return l;
}

/* Centres row i: the list's padding puts row 0 there at scroll 0. */
static void round_list_show(lv_obj_t *list, int i)
{
    lv_obj_update_layout(list);
    lv_obj_scroll_to_y(list, i * (ROW_H + ROW_GAP), LV_ANIM_OFF);
    lv_obj_send_event(list, LV_EVENT_SCROLL, NULL);
}

/* A row: a title, and under it, smaller and grey, sub (if any), which *sub
 * gets to change later. NULL click: not a button, just words. */
static lv_obj_t *round_row(lv_obj_t *list, const char *title, const char *sub, lv_obj_t **sub_label,
                           lv_event_cb_t click, void *user)
{
    lv_obj_t *r = lv_obj_create(list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, ROW_W, ROW_H);
    lv_obj_set_style_color_filter_dsc(r, &s_fade_filter, 0);   /* the fade: see on_round_scroll() */
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (click) {
        lv_obj_set_style_radius(r, ROW_H / 2, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(r, ROW_BG, 0);   /* light enough to read as something to tap */
        lv_obj_set_style_bg_color(r, lv_color_hex(0x484848), LV_STATE_PRESSED);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, click, LV_EVENT_SHORT_CLICKED, user);
    }
    lv_obj_t *t = lv_label_create(r);
    lv_obj_set_width(t, LV_PCT(88));   /* narrowing with the row */
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    /* Large if it fits the row on one line, else a size down: never wrapped
     * out of the pill. */
    lv_point_t size;
    lv_text_get_size(&size, title, &lv_font_montserrat_28, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const lv_font_t *font = size.x <= 290 ? &lv_font_montserrat_28 : &lv_font_montserrat_20;
    lv_obj_set_style_text_font(t, font, 0);
    /* Zoomed in the middle, a size up, if it fits the middle row then. */
    const lv_font_t *big = font == &lv_font_montserrat_28 ? &lv_font_montserrat_34 : &lv_font_montserrat_28;
    lv_text_get_size(&size, title, big, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_obj_set_user_data(t, size.x <= 350 && *title ? (void *)big : NULL);
    lv_obj_set_height(t, lv_font_get_line_height(font));   /* one line: "..." if the row narrows past it */
    lv_label_set_text(t, title);
    if (sub) {
        lv_obj_t *s = lv_label_create(r);
        lv_obj_set_width(s, LV_PCT(88));
        lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(s, lv_color_hex(0x9E9E9E), 0);
        lv_obj_set_height(s, lv_font_get_line_height(LV_FONT_DEFAULT));
        lv_label_set_text(s, sub);
        if (sub_label) {
            *sub_label = s;
        }
    }
    return r;
}

static void on_back(lv_event_t *e);

/* Behind a recording's row, faintly, its loudness over its length: talk
 * looks different from a quiet room at a glance. */
static void on_row_draw(lv_event_t *e)
{
    const uint8_t *env = lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    if (!env) {
        return;
    }
    lv_area_t box;
    lv_obj_get_coords(lv_event_get_current_target_obj(e), &box);
    lv_draw_rect_dsc_t bar;
    lv_draw_rect_dsc_init(&bar);
    /* Drawn by hand, so the row's fade isn't applied for us. */
    bar.bg_color = lv_color_darken(lv_color_hex(0x3D5560),
                                   lv_obj_get_style_color_filter_opa(lv_event_get_current_target_obj(e), 0));
    bar.radius = 1;
    int32_t w = lv_area_get_width(&box) - 2 * ROW_H / 2, h = lv_area_get_height(&box) - 14;
    int32_t mid = (box.y1 + box.y2) / 2;
    for (int b = 0; b < REC_ENV_N; b++) {
        int32_t level = env[b] > 20 ? (env[b] - 20) * h / 68 : 0;   /* the quietest steps show as nothing */
        if (level < 2) {
            continue;
        }
        int32_t x = box.x1 + ROW_H / 2 + b * w / REC_ENV_N;
        lv_area_t a = { x, mid - level / 2, x + w / REC_ENV_N - 3, mid + level / 2 };
        lv_draw_rect(lv_event_get_layer(e), &bar, &a);
    }
}

static void on_row_delete(lv_event_t *e)
{
    lv_free(lv_obj_get_user_data(lv_event_get_current_target_obj(e)));
}

static int s_all_n, s_all_new, s_all_kept;   /* what Delete all would take: count_all() */
static void count_all(void);
static void on_delete_all(lv_event_t *e);
static void on_delete_downloaded(lv_event_t *e);

static void fill_list(void)
{
    rec_recording_t *recs = s_rows;
    int n = recorder_recordings(recs, MAX_ROWS);
    uint32_t going = recorder_recording();
    int32_t scrolled = lv_obj_get_scroll_y(s_list);
    lv_obj_clean(s_list);
    round_row(s_list, LV_SYMBOL_LEFT " Back", NULL, NULL, on_back, NULL);
    for (int i = 0; i < n; i++) {
        char when[32], len[16], title[REC_NAME_MAX + 8], sub[96];
        format_when(&recs[i], when, sizeof(when));
        format_len(recs[i].blocks / REC_BLOCKS_PER_S, len, sizeof(len));
        /* Named: the name, then when. Not: when, then how long. Protected
         * (never replaced by a save) carries the disk; downloaded, a tick. */
        snprintf(title, sizeof(title), "%s%s", recs[i].kept ? LV_SYMBOL_SAVE " " : "",
                 recs[i].name[0] ? recs[i].name : when);
        snprintf(sub, sizeof(sub), "%s%s%s%s%s", recs[i].name[0] ? when : "", recs[i].name[0] ? "   " : "", len,
                 recs[i].first == going ? "   recording" : "", recs[i].pulled ? "   " LV_SYMBOL_OK : "");
        void *first = (void *)(uintptr_t)recs[i].first;
        lv_obj_t *row = round_row(s_list, title, sub, NULL, on_row_click, first);
        lv_obj_add_event_cb(row, on_row_hold, LV_EVENT_LONG_PRESSED, first);
        uint8_t *env = lv_malloc(REC_ENV_N);
        if (env) {
            memcpy(env, recs[i].env, REC_ENV_N);
            lv_obj_set_user_data(row, env);
            lv_obj_add_event_cb(row, on_row_draw, LV_EVENT_DRAW_MAIN_END, NULL);
            lv_obj_add_event_cb(row, on_row_delete, LV_EVENT_DELETE, NULL);
        }
    }
    /* Last, out of the thumb's way: Delete downloaded, then Delete all, each
     * asking first. Delete downloaded whenever some are on the laptop; Delete
     * all only when some aren't, since otherwise it takes the same ones. */
    count_all();
    int pulled = s_all_n - s_all_new;
    if (pulled) {
        char sub[32];
        snprintf(sub, sizeof(sub), "%d on the laptop", pulled);
        round_row(s_list, LV_SYMBOL_TRASH " Delete downloaded", sub, NULL, on_delete_downloaded, NULL);
    }
    if (s_all_new) {
        lv_obj_t *all = round_row(s_list, LV_SYMBOL_TRASH " Delete all", s_all_kept ? "Protected ones stay" : NULL,
                                  NULL, on_delete_all, NULL);
        lv_obj_set_style_text_color(lv_obj_get_child(all, 0), RED, 0);
    }
    if (!n) {
        char sub[48];
        snprintf(sub, sizeof(sub), "SAVE keeps the last %d min", CONFIG_REC_SECONDS / 60);
        round_row(s_list, "No recordings", sub, NULL, NULL, NULL);
    }
    /* Opened: the newest recording in the middle, Back just above it. A
     * refill while it's open keeps the place. */
    if (s_list_opened) {
        s_list_opened = false;
        round_list_show(s_list, 1);
    } else {
        lv_obj_update_layout(s_list);
        lv_obj_scroll_to_y(s_list, scrolled, LV_ANIM_OFF);
        lv_obj_send_event(s_list, LV_EVENT_SCROLL, NULL);
    }
}

static void on_save(lv_event_t *e)
{
    if (recorder_paused()) {
        recorder_set_paused(false);   /* it says RESUME */
        return;
    }
    atomic_store(&s_quiet, false);   /* on the screen: the name prompt follows */
    if (!recorder_save() && recorder_seconds() == 0) {
        ui_nothing_new();
    }
}



static void on_open_list(lv_event_t *e)
{
    s_list_changes = UINT32_MAX;   /* refill on the next tick */
    s_list_opened = true;
    lv_screen_load(s_list_screen);
}

static void on_back(lv_event_t *e)
{
    lv_screen_load(s_main);
}

/* ---- Settings: a round list, and a dial for each number ---- */

/* What each dial's steps mean: value(step) and the step for the setting now. */
static int dial_value(int kind, int step)
{
    switch (kind) {
    case SET_GAIN: return step * 3;              /* dB, the ES7210's steps */
    case SET_VOLUME: return step * 5;
    case SET_BRIGHT: return (step + 1) * 10;     /* 10 to 100 % */
    default: return OFF_STEPS[step];
    }
}

static int dial_now(int kind)
{
    switch (kind) {
    case SET_GAIN: return settings_mic_gain_db() / 3;
    case SET_VOLUME: return (settings_volume() + 2) / 5;
    case SET_BRIGHT: return (settings_brightness() + 5) / 10 - 1;
    default: {
        int i = 0;
        while (i + 1 < OFF_N && OFF_STEPS[i] < settings_screen_off_s()) {
            i++;
        }
        return i;
    }
    }
}

static void format_setting(int kind, int value, char *out, size_t n)
{
    switch (kind) {
    case SET_GAIN: snprintf(out, n, "%d dB", value); break;
    case SET_VOLUME: snprintf(out, n, "%d", value); break;
    case SET_BRIGHT: snprintf(out, n, "%d%%", value); break;
    case SET_OFF: snprintf(out, n, value < 60 ? "%d s" : "%d min", value < 60 ? value : value / 60); break;
    case SET_SOUNDS: snprintf(out, n, "%s", settings_sounds() ? "On" : "Off"); break;
    case SET_AOD: snprintf(out, n, "%s", settings_aod() ? "On: dim ring, screen off" : "Off"); break;
    default:
        snprintf(out, n, "%s", !settings_aod() ? "Needs the always-on ring" : settings_tap_wake() ? "On" : "Off");
        break;
    }
}

static void show_settings(void)
{
    char v[40];
    for (int k = 0; k < SET_DIALS; k++) {
        format_setting(k, dial_value(k, dial_now(k)), v, sizeof(v));
        lv_label_set_text(s_set_sub[k], v);
    }
    for (int k = SET_SOUNDS; k < SET_ROWS; k++) {
        char t[40];
        format_setting(k, 0, t, sizeof(t));
        lv_label_set_text(s_set_sub[k], t);
    }
    /* Storage: what's used, and what's protected from being replaced. */
    int n = recorder_recordings(s_rows, MAX_ROWS), used = 0, kept = 0;
    for (int i = 0; i < n; i++) {
        used += (int)s_rows[i].parts;
        kept += s_rows[i].kept;
    }
    lv_label_set_text_fmt(s_storage_sub, "%d of %" PRIu32 " used, %d protected", used, recorder_slots(), kept);
}

static void on_dial(lv_event_t *e)
{
    int value = dial_value(s_edit_kind, lv_arc_get_value(s_e_arc));
    switch (s_edit_kind) {
    case SET_GAIN: settings_set_mic_gain_db(value); break;
    case SET_VOLUME: settings_set_volume(value); break;
    case SET_BRIGHT:
        settings_set_brightness(value);
        set_brightness(value);
        break;
    default: settings_set_screen_off_s(value); break;
    }
    char v[16];
    format_setting(s_edit_kind, value, v, sizeof(v));
    lv_label_set_text(s_e_value, v);
}

static void open_dial(int kind)
{
    static const char *const TITLES[] = { "Mic sensitivity", "Speaker volume", "Brightness", "Screen off after" };
    static const int STEPS[] = { 12, 20, 9, OFF_N - 1 };
    s_edit_kind = kind;
    lv_label_set_text(s_e_title, TITLES[kind]);
    lv_arc_set_range(s_e_arc, 0, STEPS[kind]);
    lv_arc_set_value(s_e_arc, dial_now(kind));
    if (kind == SET_GAIN) {
        lv_obj_remove_flag(s_e_level, LV_OBJ_FLAG_HIDDEN);   /* to hear the gain by eye */
    } else {
        lv_obj_add_flag(s_e_level, LV_OBJ_FLAG_HIDDEN);
    }
    char v[16];
    format_setting(kind, dial_value(kind, dial_now(kind)), v, sizeof(v));
    lv_label_set_text(s_e_value, v);
    lv_screen_load(s_edit);
}

static void on_dial_released(lv_event_t *e)
{
    if (s_edit_kind == SET_VOLUME) {
        recorder_beep();   /* how loud it is now */
    }
}

static void on_dial_done(lv_event_t *e)
{
    show_settings();
    lv_screen_load(s_settings_screen);
}

static void on_set_row(lv_event_t *e)
{
    int kind = (int)(intptr_t)lv_event_get_user_data(e);
    if (kind == SET_SOUNDS) {
        settings_set_sounds(!settings_sounds());
    } else if (kind == SET_AOD) {
        settings_set_aod(!settings_aod());
        ESP_LOGI(TAG, "always-on ring %s", settings_aod() ? "on" : "off");
    } else if (kind == SET_TAP) {
        if (settings_aod()) {
            settings_set_tap_wake(!settings_tap_wake());
        }
        ESP_LOGI(TAG, "tap to wake %s%s", settings_tap_wake() ? "on" : "off", settings_aod() ? "" : " (no ring)");
    } else {
        open_dial(kind);
        return;
    }
    show_settings();
}

static void on_open_settings(lv_event_t *e)
{
    show_settings();
    round_list_show(s_set_list, 1);   /* Mic gain in the middle, Back just above */
    lv_screen_load(s_settings_screen);
}

/* A question on a screen of its own, big enough to answer: what's asked, why,
 * and the one button that does it. */
static void open_confirm(const char *title, const char *why, const char *icon, const char *caption,
                         void (*act)(void))
{
    lv_label_set_text(s_c_title, title);
    lv_label_set_text(s_c_why, why);
    lv_label_set_text(s_c_icon, icon);
    lv_label_set_text(s_c_caption, caption);
    s_confirm_do = act;
    if (lv_screen_active() != s_confirm) {
        s_confirm_back = lv_screen_active();
    }
    lv_screen_load(s_confirm);
}

static void on_confirm_yes(lv_event_t *e)
{
    s_confirm_do();
}

static void on_confirm_no(lv_event_t *e)
{
    lv_screen_load(s_confirm_back ? s_confirm_back : s_settings_screen);
}

static void power_off_now(void)
{
    atomic_store(&s_power_off, true);   /* the buttons task powers off */
}

static void on_power_off(lv_event_t *e)
{
    open_confirm("Power off?", "Recording stops until\nPWR turns it back on.", LV_SYMBOL_POWER, "Power off",
                 power_off_now);
}

static void delete_all_now(void)
{
    ESP_LOGI(TAG, "deleted %d recordings", recorder_delete_all());
    s_list_opened = true;   /* from the top again */
    fill_list();
    lv_screen_load(s_list_screen);
}

static void delete_downloaded_now(void)
{
    ESP_LOGI(TAG, "deleted %d downloaded recordings", recorder_delete_downloaded());
    s_list_opened = true;   /* from the top again */
    fill_list();
    lv_screen_load(s_list_screen);
}

/* How many go, and how many of those the laptop hasn't got yet: those are
 * gone for good. */
static void count_all(void)
{
    int n = recorder_recordings(s_rows, MAX_ROWS);
    uint32_t going = recorder_recording();
    s_all_n = s_all_new = s_all_kept = 0;
    for (int i = 0; i < n; i++) {
        if (s_rows[i].kept) {
            s_all_kept++;
        } else if (s_rows[i].first != going) {
            s_all_n++;
            s_all_new += !s_rows[i].pulled;
        }
    }
}

static void on_delete_all(lv_event_t *e)
{
    static char title[40], why[96];
    count_all();   /* as it is now, not when the list was filled */
    if (!s_all_n) {
        return;
    }
    snprintf(title, sizeof(title), s_all_n == 1 ? "Delete 1 recording?" : "Delete %d recordings?", s_all_n);
    if (s_all_new == s_all_n) {
        snprintf(why, sizeof(why), "%s downloaded yet.\n%s", s_all_n == 1 ? "It isn't" : "None are",
                 s_all_kept ? "Protected ones stay." : "This can't be undone.");
    } else if (s_all_new) {
        snprintf(why, sizeof(why), "%d not downloaded yet.\n%s", s_all_new,
                 s_all_kept ? "Protected ones stay." : "This can't be undone.");
    } else {
        snprintf(why, sizeof(why), "%s on the laptop.\n%s", s_all_n == 1 ? "It's" : "All are",
                 s_all_kept ? "Protected ones stay." : "This can't be undone.");
    }
    open_confirm(title, why, LV_SYMBOL_TRASH, "Delete", delete_all_now);
}

static void on_delete_downloaded(lv_event_t *e)
{
    static char title[40];
    count_all();
    int pulled = s_all_n - s_all_new;
    if (!pulled) {
        return;
    }
    snprintf(title, sizeof(title), pulled == 1 ? "Delete 1 downloaded?" : "Delete %d downloaded?", pulled);
    open_confirm(title, pulled == 1 ? "It's on the laptop.\nProtected ones stay." :
                                      "They're on the laptop.\nProtected ones stay.",
                 LV_SYMBOL_TRASH, "Delete", delete_downloaded_now);
}

static void on_swipe(lv_event_t *e);

static lv_obj_t *screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_swipe, LV_EVENT_GESTURE, NULL);
    return scr;
}

static lv_obj_t *button(lv_obj_t *parent, int w, int h, int y, const lv_font_t *font, lv_obj_t **label,
                        lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_align(b, LV_ALIGN_CENTER, 0, y);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_ext_click_area(b, 4);   /* a fingertip lands a little wide */
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    *label = lv_label_create(b);
    lv_obj_set_style_text_font(*label, font, 0);
    lv_obj_center(*label);
    return b;
}

static lv_obj_t *round_button(lv_obj_t *parent, int size, int x, int y, const lv_font_t *font, const char *text,
                              lv_event_cb_t cb, lv_obj_t **label)
{
    lv_obj_t *l;
    lv_obj_t *b = button(parent, size, size, y, font, &l, cb);
    lv_obj_set_x(b, x);
    lv_label_set_text(l, text);
    if (label) {
        *label = l;
    }
    return b;
}

static lv_obj_t *label_at(lv_obj_t *parent, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, y);
    return l;
}

/* Where the i-th of n things goes on a ring of radius r, from the top
 * clockwise. */
static void on_ring(int i, int n, int r, int *x, int *y)
{
    float a = 2.0f * (float)M_PI * (float)i / (float)n - (float)M_PI / 2.0f;
    *x = (int)lroundf((float)r * cosf(a));
    *y = (int)lroundf((float)r * sinf(a));
}

/* The screens' ring of places, the same on each: six at 60 degrees round
 * the middle, i = 0 at the top, clockwise. A round button of size there,
 * its word (if any) outside it, above for the top ones, below for the rest. */
#define RING_R 162

static lv_obj_t *ring_button_at(lv_obj_t *parent, int i, int r, int size, const char *icon, lv_color_t color,
                                lv_event_cb_t cb, const char *word, lv_obj_t **caption)
{
    int x, y;
    on_ring(i, 6, r, &x, &y);
    lv_obj_t *b = round_button(parent, size, x, y, &lv_font_montserrat_28, icon, cb, NULL);
    lv_obj_set_style_bg_color(b, color, 0);
    if (word) {
        lv_obj_t *c = label_at(parent, y + (y < 0 ? -1 : 1) * (size / 2 + 16));
        lv_obj_set_x(c, x);
        lv_label_set_text(c, word);
        if (caption) {
            *caption = c;
        }
    }
    return b;
}

static lv_obj_t *ring_button(lv_obj_t *parent, int i, int size, const char *icon, lv_color_t color, lv_event_cb_t cb,
                             const char *word, lv_obj_t **caption)
{
    return ring_button_at(parent, i, RING_R, size, icon, color, cb, word, caption);
}

/* ---- keeping, naming and deleting a recording ---- */

/* The naming list's Keep row: green, and saying so, once kept. */
static void show_keep(lv_obj_t *row, lv_obj_t *sub, bool kept)
{
    lv_label_set_text(sub, kept ? "On: never replaced" : "Off");
    lv_obj_set_style_bg_color(row, kept ? GREEN : lv_color_hex(0x1E1E1E), 0);
}

static bool clip_kept(uint32_t slot, uint32_t seq)
{
    rec_clip_t clip;
    return !recorder_clip(slot, &clip) && clip.seq == seq && clip.kept;
}

static void open_naming(uint32_t slot, uint32_t seq, const char *title, lv_obj_t *back)
{
    s_name_slot = slot;
    s_name_seq = seq;
    s_name_back = back;
    lv_label_set_text(s_name_title, title);
    show_keep(s_name_keep, s_name_keep_label, clip_kept(slot, seq));
    /* Right after a save: carrying it on is the first thing offered. */
    bool cont = recorder_can_continue() && back != s_list_screen;
    if (cont) {
        lv_obj_remove_flag(s_name_cont, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_name_cont, LV_OBJ_FLAG_HIDDEN);
    }
    round_list_show(s_name_list, 1);   /* that, or Protect, in the middle, the title above */
    lv_screen_load(s_name_screen);
}

static void on_name_keep(lv_event_t *e)
{
    bool kept = !clip_kept(s_name_slot, s_name_seq);
    if (!recorder_set_kept(s_name_slot, s_name_seq, kept)) {
        show_keep(s_name_keep, s_name_keep_label, kept);
    }
}

static void show_manage(void)
{
    rec_recording_t r;
    if (!find_row(s_manage_first, &r)) {
        lv_screen_load(s_list_screen);   /* gone meanwhile */
        return;
    }
    char when[32], len[16];
    format_when(&r, when, sizeof(when));
    format_len(r.blocks / REC_BLOCKS_PER_S, len, sizeof(len));
    lv_label_set_text_fmt(s_m_title, r.name[0] ? "%s\n%s\n%s" : "%s%s\n%s", r.name, when, len);   /* a line each */
    lv_label_set_text(s_m_keep_label, r.kept ? "Protected" : "Protect");
    lv_obj_set_style_bg_color(s_m_keep, r.kept ? GREEN : GREY, 0);
    /* Its name and keep flag are on its first part, so without it, neither. */
    if (r.whole) {
        lv_obj_remove_state(s_m_keep, LV_STATE_DISABLED);
        lv_obj_remove_state(s_m_name, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_m_keep, LV_STATE_DISABLED);
        lv_obj_add_state(s_m_name, LV_STATE_DISABLED);
    }
}

static void open_manage(uint32_t first, lv_obj_t *back)
{
    s_manage_first = first;
    s_manage_back = back;
    show_manage();
    lv_screen_load(s_manage);
}

static void on_m_keep(lv_event_t *e)
{
    rec_recording_t r;
    if (find_row(s_manage_first, &r)) {
        recorder_protect_recording(r.first, !r.kept);
    }
    show_manage();
}

static void on_m_name(lv_event_t *e)
{
    rec_recording_t r;
    if (find_row(s_manage_first, &r) && r.whole) {
        open_naming(r.slots[0], r.first, "Name this recording", s_list_screen);
    }
}

static void show_undo(bool on)
{
    if (on) {
        s_undo_until = esp_timer_get_time() + 6000000;
        lv_obj_remove_flag(s_undo, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_undo, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_m_delete(lv_event_t *e)
{
    if (!recorder_delete_recording(s_manage_first)) {
        show_undo(true);
    }
    lv_screen_load(s_list_screen);
}

static void on_undo(lv_event_t *e)
{
    if (recorder_undo_delete()) {
        ESP_LOGW(TAG, "couldn't put the recording back: a save took its place");
    }
    show_undo(false);
}

static void on_m_back(lv_event_t *e)
{
    lv_screen_load(s_manage_back);
}

static void name_done(void)
{
    lv_screen_load(s_name_back);
}

static void set_name(const char *name)
{
    if (recorder_set_name(s_name_slot, s_name_seq, name)) {
        ESP_LOGW(TAG, "clip %" PRIu32 " not named", s_name_seq);
    }
}

static void on_preset(lv_event_t *e)
{
    set_name(PRESETS[(uintptr_t)lv_event_get_user_data(e)]);
    name_done();
}

static void on_name_done(lv_event_t *e)
{
    name_done();
}

static void on_continue(lv_event_t *e)
{
    recorder_continue();
    lv_screen_load(s_main);   /* where the recording shows, with its STOP */
}

/* The keys' labels for the mode they're in, and the mode key's for the next. */
static void show_kb_mode(void)
{
    bool letters = s_tap.mode == MULTITAP_LETTERS;
    for (int k = 0; k < MULTITAP_KEYS; k++) {
        lv_label_set_text(s_kb_keys[k], multitap_label(&s_tap, k));
        lv_obj_set_style_text_font(s_kb_keys[k], letters ? &lv_font_montserrat_20 : &lv_font_montserrat_34, 0);
    }
    static const char *const NEXT[] = { "123", "#+=", "abc" };
    static const char *const HINT[] = { "Tap again: next letter", "Numbers", "Symbols" };
    lv_label_set_text(s_kb_mode, NEXT[s_tap.mode]);
    lv_label_set_text(s_kb_hint, HINT[s_tap.mode]);
}

static void on_kb_mode(lv_event_t *e)
{
    multitap_set_mode(&s_tap, (multitap_mode_t)((s_tap.mode + 1) % MULTITAP_MODES));
    show_kb_mode();
}

static void on_type(lv_event_t *e)
{
    rec_clip_t clip;
    bool same = !recorder_clip(s_name_slot, &clip) && clip.seq == s_name_seq;
    lv_textarea_set_text(s_kb_ta, same ? clip.name : "");
    multitap_reset(&s_tap);   /* letters again */
    show_kb_mode();
    lv_screen_load(s_kb_screen);
}

static void on_kb_ready(lv_event_t *e)
{
    /* Without the spaces around it; nothing typed clears the name. */
    char name[NAME_TYPED_MAX * 4 + 1];
    const char *t = lv_textarea_get_text(s_kb_ta);
    while (*t == ' ') {
        t++;
    }
    snprintf(name, sizeof(name), "%s", t);
    size_t n = strlen(name);
    while (n && name[n - 1] == ' ') {
        name[--n] = '\0';
    }
    set_name(name);
    name_done();
}

static void on_kb_cancel(lv_event_t *e)
{
    lv_screen_load(s_name_screen);
}

static void on_key(lv_event_t *e)
{
    int key = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    int len = (int)strlen(lv_textarea_get_text(s_kb_ta));
    bool replace;
    char c = multitap_press(&s_tap, key, (int64_t)(esp_timer_get_time() / 1000), len, &replace);
    if (replace) {
        lv_textarea_delete_char(s_kb_ta);
    } else if (len >= NAME_TYPED_MAX) {
        return;
    }
    lv_textarea_add_char(s_kb_ta, (uint32_t)(uint8_t)c);
}

static void on_backspace(lv_event_t *e)
{
    multitap_break(&s_tap);   /* in the same mode */
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        lv_textarea_set_text(s_kb_ta, "");   /* held: all of it */
    } else {
        lv_textarea_delete_char(s_kb_ta);
    }
}

static lv_obj_t *level_bar(lv_obj_t *parent, int w, int h, int y)
{
    lv_obj_t *b = lv_bar_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_align(b, LV_ALIGN_CENTER, 0, y);
    lv_bar_set_range(b, 0, 100);
    lv_obj_set_style_bg_color(b, GREEN, LV_PART_INDICATOR);
    return b;
}

static void build_settings(void)
{
    s_settings_screen = screen();
    s_set_list = round_list(s_settings_screen);
    round_row(s_set_list, LV_SYMBOL_LEFT " Back", NULL, NULL, on_back, NULL);
    static const char *const ROWS[] = { "Mic sensitivity", "Speaker volume", "Brightness", "Screen off after",
                                        "Sounds", "Always-on ring", "Tap to wake" };
    for (int k = 0; k < SET_ROWS; k++) {
        round_row(s_set_list, ROWS[k], "", &s_set_sub[k], on_set_row, (void *)(intptr_t)k);
    }
    round_row(s_set_list, "Storage", "", &s_storage_sub, NULL, NULL);
    lv_obj_t *off = round_row(s_set_list, LV_SYMBOL_POWER " Power off", NULL, NULL, on_power_off, NULL);
    lv_obj_set_style_text_color(lv_obj_get_child(off, 0), RED, 0);

    /* The dial: an arc around the rim, open at the bottom for Done, with the
     * value in the middle as it turns. */
    s_edit = screen();
    s_e_arc = lv_arc_create(s_edit);
    lv_obj_set_size(s_e_arc, 430, 430);
    lv_obj_center(s_e_arc);
    lv_arc_set_bg_angles(s_e_arc, 135, 45);
    lv_obj_set_style_arc_width(s_e_arc, 22, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_e_arc, 22, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(s_e_arc, 10, LV_PART_KNOB);   /* a knob a fingertip can hold */
    lv_obj_add_event_cb(s_e_arc, on_dial, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_e_arc, on_dial_released, LV_EVENT_RELEASED, NULL);
    s_e_title = label_at(s_edit, -72);
    lv_obj_set_style_text_color(s_e_title, lv_color_hex(0x9E9E9E), 0);
    s_e_value = label_at(s_edit, -10);
    lv_obj_set_style_text_font(s_e_value, &lv_font_montserrat_48, 0);
    s_e_level = level_bar(s_edit, 170, 10, 52);
    lv_obj_t *done = round_button(s_edit, 84, 0, 160, &lv_font_montserrat_28, LV_SYMBOL_OK, on_dial_done, NULL);
    lv_obj_set_style_bg_color(done, GREEN, 0);
}

static void build_confirm(void)
{
    /* The question in the middle; the answers on the ring's lower places,
     * Cancel on the left as Back is everywhere, the one that acts on the
     * right, in red. */
    s_confirm = screen();
    s_c_title = label_at(s_confirm, -62);
    lv_obj_set_style_text_font(s_c_title, &lv_font_montserrat_28, 0);
    s_c_why = label_at(s_confirm, -10);
    lv_obj_set_style_text_align(s_c_why, LV_TEXT_ALIGN_CENTER, 0);
    /* A little inside the ring: the words under them must clear the edge. */
    lv_obj_t *yes = ring_button_at(s_confirm, 2, 135, 84, "", RED, on_confirm_yes, "", &s_c_caption);
    s_c_icon = lv_obj_get_child(yes, 0);
    ring_button_at(s_confirm, 4, 135, 84, LV_SYMBOL_CLOSE, GREY, on_confirm_no, "Cancel", NULL);
}

static void build_manage(void)
{
    /* The recording in the middle; what can be done to it round it: Name
     * and Protect above, Delete (red) and Back below, as on the main screen. */
    s_manage = screen();
    s_m_title = label_at(s_manage, 0);
    lv_obj_set_width(s_m_title, 190);   /* clear of the ring buttons beside it */
    lv_label_set_long_mode(s_m_title, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_m_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_m_title, &lv_font_montserrat_28, 0);
    s_m_name = ring_button(s_manage, 5, 80, LV_SYMBOL_EDIT, GREY, on_m_name, "Name", NULL);
    s_m_keep = ring_button(s_manage, 1, 80, LV_SYMBOL_SAVE, GREY, on_m_keep, "Protect", &s_m_keep_label);
    ring_button(s_manage, 2, 80, LV_SYMBOL_TRASH, RED, on_m_delete, "Delete", NULL);
    ring_button(s_manage, 4, 80, LV_SYMBOL_LEFT, GREY, on_m_back, "Back", NULL);
}

static void build_naming(void)
{
    /* A round list: the title, Keep, typing a name, the presets, Done. */
    s_name_screen = screen();
    s_name_list = round_list(s_name_screen);
    lv_obj_t *title = round_row(s_name_list, "", "Name it, or protect it", NULL, NULL, NULL);
    s_name_title = lv_obj_get_child(title, 0);
    s_name_cont = round_row(s_name_list, "Keep recording", "on from the save, until Stop", NULL, on_continue, NULL);
    lv_obj_set_style_text_color(lv_obj_get_child(s_name_cont, 0), RED, 0);
    s_name_keep = round_row(s_name_list, LV_SYMBOL_SAVE " Protect", "", &s_name_keep_label, on_name_keep, NULL);
    round_row(s_name_list, LV_SYMBOL_KEYBOARD " Type a name", NULL, NULL, on_type, NULL);
    for (int i = 0; i < (int)(sizeof(PRESETS) / sizeof(PRESETS[0])); i++) {
        round_row(s_name_list, PRESETS[i], NULL, NULL, on_preset, (void *)(uintptr_t)i);
    }
    lv_obj_t *done = round_row(s_name_list, "Done", NULL, NULL, on_name_done, NULL);
    lv_obj_set_style_text_color(lv_obj_get_child(done, 0), lv_color_hex(0x9E9E9E), 0);

    /* A phone keypad bent round the rim: ten keys a fingertip wide, the name
     * in the middle, and under it delete, letters/numbers/symbols, and done.
     * Numbers read 1 to 9, 0 clockwise from the top. */
    s_kb_screen = screen();
    static const int RING[] = { 2, 3, 4, 5, 6, 7, 8, 9, 0, 1 };   /* abc at the top, clockwise */
    for (int i = 0; i < 10; i++) {
        int x, y;
        on_ring(i, 10, 178, &x, &y);
        lv_obj_t *b = button(s_kb_screen, 78, 78, y, &lv_font_montserrat_20, &s_kb_keys[RING[i]], on_key);
        lv_obj_set_x(b, x);
        lv_obj_set_style_bg_color(b, GREY, 0);
        lv_obj_set_user_data(b, (void *)(intptr_t)RING[i]);
    }
    s_kb_hint = label_at(s_kb_screen, -82);
    lv_obj_set_style_text_color(s_kb_hint, lv_color_hex(0x9E9E9E), 0);
    s_kb_ta = lv_textarea_create(s_kb_screen);
    lv_textarea_set_one_line(s_kb_ta, true);
    lv_textarea_set_max_length(s_kb_ta, NAME_TYPED_MAX);
    lv_textarea_set_placeholder_text(s_kb_ta, "Name");
    lv_obj_set_width(s_kb_ta, 230);
    lv_obj_align(s_kb_ta, LV_ALIGN_CENTER, 0, -32);
    lv_obj_set_style_text_font(s_kb_ta, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(s_kb_ta, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_remove_flag(s_kb_ta, LV_OBJ_FLAG_CLICKABLE);   /* typing goes in from the keys */
    lv_obj_add_state(s_kb_ta, LV_STATE_FOCUSED);          /* shows the cursor */
    lv_obj_t *del_label, *ok_label;
    lv_obj_t *del = button(s_kb_screen, 70, 70, 48, &lv_font_montserrat_28, &del_label, on_backspace);
    lv_obj_set_x(del, -80);
    lv_obj_set_style_bg_color(del, lv_color_hex(0x5D4037), 0);
    lv_label_set_text(del_label, LV_SYMBOL_BACKSPACE);
    /* A tap deletes a letter, a hold all of them: not its click, which comes
     * after a hold too. */
    lv_obj_remove_event_cb(del, on_backspace);
    lv_obj_add_event_cb(del, on_backspace, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(del, on_backspace, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_t *mode = button(s_kb_screen, 70, 70, 48, &lv_font_montserrat_20, &s_kb_mode, on_kb_mode);
    lv_obj_set_style_bg_color(mode, lv_color_hex(0x37474F), 0);
    lv_obj_t *ok = button(s_kb_screen, 70, 70, 48, &lv_font_montserrat_28, &ok_label, on_kb_ready);
    lv_obj_set_x(ok, 80);
    lv_obj_set_style_bg_color(ok, GREEN, 0);
    lv_label_set_text(ok_label, LV_SYMBOL_OK);
    multitap_reset(&s_tap);
    show_kb_mode();
}

static void build_player(void)
{
    s_player = screen();
    /* Progress around the rim; drag it to move. */
    s_p_arc = lv_arc_create(s_player);
    lv_obj_set_size(s_p_arc, 440, 440);
    lv_obj_center(s_p_arc);
    lv_arc_set_rotation(s_p_arc, 270);
    lv_arc_set_bg_angles(s_p_arc, 0, 360);
    lv_arc_set_range(s_p_arc, 0, 1000);
    lv_obj_set_style_arc_width(s_p_arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_p_arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_p_arc, GREEN, LV_PART_INDICATOR);
    lv_obj_add_event_cb(s_p_arc, on_p_arc, LV_EVENT_RELEASED, NULL);

    s_p_title = label_at(s_player, -125);
    lv_obj_set_width(s_p_title, 270);
    lv_label_set_long_mode(s_p_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_p_title, LV_TEXT_ALIGN_CENTER, 0);
    s_p_time = label_at(s_player, -78);
    lv_obj_set_style_text_font(s_p_time, &lv_font_montserrat_28, 0);

    lv_obj_t *back10 = round_button(s_player, 76, -108, 8, &lv_font_montserrat_20, "-10", on_p_skip, NULL);
    lv_obj_set_user_data(back10, (void *)(intptr_t)-10);
    lv_obj_set_style_bg_color(back10, GREY, 0);
    round_button(s_player, 96, 0, 8, &lv_font_montserrat_48, LV_SYMBOL_PAUSE, on_p_play, &s_p_play_label);
    lv_obj_t *fwd10 = round_button(s_player, 76, 108, 8, &lv_font_montserrat_20, "+10", on_p_skip, NULL);
    lv_obj_set_user_data(fwd10, (void *)(intptr_t)10);
    lv_obj_set_style_bg_color(fwd10, GREY, 0);

    /* Volume on the upper arc, inside the progress ring: quieter left,
     * louder right. */
    int x, y;
    on_ring(10, 12, 150, &x, &y);
    lv_obj_t *quieter = round_button(s_player, 62, x, y, &lv_font_montserrat_20, LV_SYMBOL_VOLUME_MID, on_p_volume, NULL);
    lv_obj_set_user_data(quieter, (void *)(intptr_t)-10);
    lv_obj_set_style_bg_color(quieter, GREY, 0);
    on_ring(2, 12, 150, &x, &y);
    lv_obj_t *louder = round_button(s_player, 62, x, y, &lv_font_montserrat_20, LV_SYMBOL_VOLUME_MAX, on_p_volume, NULL);
    lv_obj_set_user_data(louder, (void *)(intptr_t)10);
    lv_obj_set_style_bg_color(louder, GREY, 0);

    /* Speed, Back and More on the lower arc, inside the progress ring. */
    on_ring(7, 12, 150, &x, &y);
    lv_obj_t *speed = round_button(s_player, 66, x, y, &lv_font_montserrat_20, "", on_p_speed, &s_p_speed_label);
    lv_obj_set_style_bg_color(speed, GREY, 0);
    show_speed(100);
    on_ring(6, 12, 150, &x, &y);
    lv_obj_t *back = round_button(s_player, 66, x, y, &lv_font_montserrat_28, LV_SYMBOL_LEFT, on_p_back, NULL);
    lv_obj_set_style_bg_color(back, GREY, 0);
    on_ring(5, 12, 150, &x, &y);   /* Protect, Name, Delete: what a recording's own screen has */
    lv_obj_t *more = round_button(s_player, 66, x, y, &lv_font_montserrat_20, "More", on_p_more, NULL);
    lv_obj_set_style_bg_color(more, GREY, 0);
}

static void refresh_player(void)
{
    rec_play_t st;
    recorder_play_status(&st);
    bool ours = st.first == s_player_first;
    uint32_t at = ours ? st.block : 0, total = ours && st.blocks ? st.blocks : s_player_blocks;
    if (!lv_obj_has_state(s_p_arc, LV_STATE_PRESSED)) {   /* not while it's being dragged */
        lv_arc_set_value(s_p_arc, total ? (int32_t)((uint64_t)at * 1000 / total) : 0);
    }
    char a[16], b[16];
    format_len(at / REC_BLOCKS_PER_S, a, sizeof(a));
    format_len(total / REC_BLOCKS_PER_S, b, sizeof(b));
    lv_label_set_text_fmt(s_p_time, "%s / %s", a, b);
    lv_label_set_text(s_p_play_label, ours && !st.paused ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    if (s_p_volume_until && esp_timer_get_time() > s_p_volume_until) {
        s_p_volume_until = 0;
        lv_label_set_text(s_p_title, s_p_title_text);
    }
}

static void on_intro_done(lv_event_t *e);
static void build_aod_and_intro(void);

/* ---- Help: what each button and gesture does ---- */

typedef struct {
    const char *what, *does;
} help_line_t;

typedef struct {
    const char *topic;
    const help_line_t *lines;
    int n;
} help_topic_t;

#define LINES(a) a, (int)(sizeof(a) / sizeof(a[0]))

static const help_line_t HELP_TOP[] = {
    { "Tap", "Save the last 3 minutes" },  { "Tap again soon", "Keep recording on" },
    { "Tap while recording", "Stop" },     { "Tap while paused", "Resume" },
    { "Hold 3 s", "Turn off, keeping it" },
    { "Press when off", "Turn on" },       { "Hold 10 s", "Force off, if stuck" },
};
static const help_line_t HELP_BOTTOM[] = {
    { "Tap", "Screen on or off" },
    { "Dim ring, screen off", "Still listening" },
    { "Red dim ring", "Recording" },
    { "Tap the dim ring", "Wakes it (Tap to wake)" },
};
static const help_line_t HELP_MAIN[] = {
    { "SAVE", "Keep the last 3 minutes" },
    { "Saved again", "Only what's new since" },
    { "The ring", "Moves with the sound" },
    { "Red ring", "Recording: SAVE is STOP" },
    { LV_SYMBOL_PAUSE " Pause, held", "Stop listening" },
    { "Paused", "Tap RESUME" },
    { "Arc beside PWR", "PWR does what SAVE does" },
    { LV_SYMBOL_LIST " Recordings", "Play, protect, delete" },
    { LV_SYMBOL_SETTINGS " Settings", "Sound, screen, storage" },
    { "9 more fit", "Then saves replace the oldest" },
    { "Full, in red", "All protected: unprotect one" },
};
static const help_line_t HELP_LIST[] = {
    { "Tap one", "Play it" },
    { "More, or hold one", "Protect, name, delete" },
    { LV_SYMBOL_SAVE " Protected", "Never replaced" },
    { LV_SYMBOL_OK " Downloaded", "On the laptop" },
    { "Deleted one?", "Undo, for 6 seconds" },
    { LV_SYMBOL_TRASH " Delete downloaded", "Clears what's on the laptop" },
    { LV_SYMBOL_TRASH " Delete all", "At the end; asks first" },
};
static const help_line_t HELP_PLAY[] = {
    { "Drag the rim", "Jump to a moment" },
    { "-10  +10", "Back or on 10 s" },
    { "1x", "1.5x, 2x, same pitch" },
    { LV_SYMBOL_VOLUME_MID "  " LV_SYMBOL_VOLUME_MAX, "Quieter, louder" },
};
static const help_line_t HELP_NAME[] = {
    { "After a save", "Name, protect, go on" },
    { "Type a name", "Tap again: next letter" },
    { "Hold delete", "Clear the name" },
};
static const help_line_t HELP_LAPTOP[] = {
    { "Plug in USB", "Run tools/sync.py" },
    { "It downloads", "Joins parts, transcribes" },
    { "And sets the clock", "So new ones get a time" },
};
static const help_line_t HELP_ANY[] = {
    { "Swipe right", "Go back" },
};
static const help_topic_t HELP[] = {
    { "Top button", LINES(HELP_TOP) },          { "Bottom button", LINES(HELP_BOTTOM) },
    { "Main screen", LINES(HELP_MAIN) },        { "Recordings", LINES(HELP_LIST) },
    { "Playing", LINES(HELP_PLAY) },            { "Naming", LINES(HELP_NAME) },
    { "On the laptop", LINES(HELP_LAPTOP) },    { "Anywhere", LINES(HELP_ANY) },
};

static lv_obj_t *s_help, *s_help_list, *s_topic, *s_topic_list;

static void tour_open(int step);
static void on_tour_start(lv_event_t *e);

/* The first time, the tour; then the topics, with the tour first. */
static void on_open_help(lv_event_t *e)
{
    if (!settings_tour_seen()) {
        settings_set_tour_seen();
        tour_open(0);
        return;
    }
    round_list_show(s_help_list, 1);   /* the first topic in the middle, Back above */
    lv_screen_load(s_help);
}

static void on_help_back(lv_event_t *e)
{
    lv_screen_load(s_main);
}

static void open_topic(int i)
{
    const help_topic_t *t = &HELP[i];
    lv_obj_clean(s_topic_list);
    round_row(s_topic_list, LV_SYMBOL_LEFT " Help", NULL, NULL, on_open_help, NULL);
    lv_obj_t *title = round_row(s_topic_list, t->topic, NULL, NULL, NULL, NULL);
    lv_obj_set_style_text_color(lv_obj_get_child(title, 0), lv_theme_get_color_primary(title), 0);
    for (int i = 0; i < t->n; i++) {
        round_row(s_topic_list, t->lines[i].what, t->lines[i].does, NULL, NULL, NULL);
    }
    round_list_show(s_topic_list, 2);   /* its first line in the middle, the topic above */
    lv_screen_load(s_topic);
}

static void on_open_topic(lv_event_t *e)
{
    open_topic((int)(intptr_t)lv_event_get_user_data(e));
}

static void build_help(void)
{
    s_help = screen();
    s_help_list = round_list(s_help);
    round_row(s_help_list, LV_SYMBOL_LEFT " Back", NULL, NULL, on_help_back, NULL);
    round_row(s_help_list, "How to use", "A tour of the screen", NULL, on_tour_start, NULL);
    for (int i = 0; i < (int)(sizeof(HELP) / sizeof(HELP[0])); i++) {
        round_row(s_help_list, HELP[i].topic, NULL, NULL, on_open_topic, (void *)(intptr_t)i);
    }
    s_topic = screen();
    s_topic_list = round_list(s_topic);
}


/* Swiping right goes back, from every screen but the main one. Not when it
 * started on something that's dragged, and never as a tap as well: the
 * touch is ignored from here until the finger lifts. */
static bool tour_open_now(void);
static void tour_back(void);

static void on_swipe(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    lv_obj_t *from = lv_indev_get_active_obj();
    if (lv_indev_get_gesture_dir(indev) != LV_DIR_RIGHT ||
        (from && (lv_obj_check_type(from, &lv_slider_class) || lv_obj_check_type(from, &lv_arc_class) ||
                  lv_obj_check_type(from, &lv_switch_class)))) {
        return;
    }
    lv_obj_t *scr = lv_screen_active();
    if (scr == s_main) {
        if (tour_open_now()) {
            lv_indev_wait_release(indev);
            tour_back();
        }
        return;
    }
    lv_indev_wait_release(indev);
    if (scr == s_list_screen || scr == s_settings_screen) {
        on_back(e);
    } else if (scr == s_intro) {
        on_intro_done(e);
    } else if (scr == s_help) {
        on_help_back(e);
    } else if (scr == s_topic) {
        on_open_help(e);
    } else if (scr == s_confirm) {
        on_confirm_no(e);
    } else if (scr == s_edit) {
        on_dial_done(e);
    } else if (scr == s_player) {
        on_p_back(e);
    } else if (scr == s_manage) {
        on_m_back(e);
    } else if (scr == s_name_screen) {
        name_done();
    } else if (scr == s_kb_screen) {
        on_kb_cancel(e);
    }
}

/* ---- the main screen's bands ----
 *
 * The four buttons round SAVE are quarter-ring bands filling the circle
 * between SAVE and the rim, with gaps at the top and bottom for the status
 * and the battery. Left, the audio: Pause above, Recordings below. Right,
 * the device: Help above, Settings below, beside the side buttons, where a
 * finger brushing the screen as it presses one only opens something.
 * Each is drawn as an arc and takes only touches inside its own band. */

enum { BAND_PAUSE, BAND_HELP, BAND_SET, BAND_LIST, BANDS };

#define BAND_RI 108   /* inner radius: 10 px clear of SAVE's 98 */
#define BAND_RO 208   /* outer: inside the arc beside PWR (from 211) and the ring */
#define BAND_SEAM 4   /* px: the even black line between the two bands on each side */
#define BAND_PAD 3    /* a band's words keep at least this far from its edges */

/* Compass degrees, 0 at the top, clockwise. The two on each side meet at 9
 * and 3 o'clock, split by a seam of even width; the gaps at the top and
 * bottom hold the status and the battery. */
static const int16_t BAND_DEG[BANDS][2] = {
    [BAND_PAUSE] = { 270, 326 }, [BAND_HELP] = { 34, 90 }, [BAND_SET] = { 90, 146 }, [BAND_LIST] = { 214, 270 },
};
#define BAND_SEAM_DEG(i) ((i) == BAND_PAUSE || (i) == BAND_LIST ? 270 : 90)

static lv_obj_t *s_band[BANDS], *s_band_icon[BANDS], *s_band_text[BANDS], *s_band_sub[BANDS];
static lv_obj_t *s_band_col[BANDS];   /* the icon and words, placed by place_band_label() */
static int32_t s_hold;          /* Pause held: 0 to 1000 of the way to pausing */
static int64_t s_hold_hint_us;  /* a tap on Pause: "Hold to pause" until then */

static int band_of(lv_obj_t *obj)
{
    for (int i = 0; i < BANDS; i++) {
        if (s_band[i] == obj) {
            return i;
        }
    }
    return -1;
}

static void band_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    int i = band_of(obj);
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    lv_obj_init_draw_arc_dsc(obj, LV_PART_MAIN, &d);   /* its colour, pressed or not, and any fade */
    if (i < 0 || d.opa <= LV_OPA_MIN) {
        return;
    }
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    d.center.x = (a.x1 + a.x2) / 2;
    d.center.y = (a.y1 + a.y2) / 2;
    d.radius = BAND_RO;
    d.width = BAND_RO - BAND_RI;
    d.rounded = 0;
    /* LVGL's angles start at 3 o'clock. */
    int32_t start = (BAND_DEG[i][0] + 270) % 360, end = (BAND_DEG[i][1] + 270) % 360;
    d.start_angle = start;
    d.end_angle = end;
    lv_draw_arc(lv_event_get_layer(e), &d);
    if (i == BAND_PAUSE && s_hold > 0) {   /* filling amber as it's held */
        d.color = AMBER;
        d.end_angle = start + (end - start) * s_hold / 1000;
        lv_draw_arc(lv_event_get_layer(e), &d);
    }
    /* The seam to its neighbour on this side: a straight black line, the
     * same width all along (an angular gap widens toward the rim). Both
     * bands draw it, so it's whole whichever is drawn last. */
    float sa = BAND_SEAM_DEG(i) * (float)M_PI / 180.0f, sx = sinf(sa), sy = -cosf(sa);
    lv_draw_line_dsc_t l;
    lv_draw_line_dsc_init(&l);
    l.color = lv_color_black();
    l.width = BAND_SEAM;
    l.opa = LV_OPA_COVER;
    l.p1.x = d.center.x + sx * (BAND_RI - 2);
    l.p1.y = d.center.y + sy * (BAND_RI - 2);
    l.p2.x = d.center.x + sx * (BAND_RO + 2);
    l.p2.y = d.center.y + sy * (BAND_RO + 2);
    lv_draw_line(lv_event_get_layer(e), &l);
}

/* Only inside the band, out to the rim, and to the middle of the gap either
 * side of it, so no touch near a band lands nowhere. */
static void band_hit(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    lv_hit_test_info_t *info = lv_event_get_hit_test_info(e);
    int i = band_of(obj);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    float dx = (float)(info->point->x - (a.x1 + a.x2) / 2), dy = (float)(info->point->y - (a.y1 + a.y2) / 2);
    float r = sqrtf(dx * dx + dy * dy);
    float deg = atan2f(dx, -dy) * 180.0f / (float)M_PI;
    if (deg < 0) {
        deg += 360.0f;
    }
    info->res = i >= 0 && r >= BAND_RI - 6 && r <= BSP_LCD_H_RES / 2 && deg >= BAND_DEG[i][0] &&
                deg <= BAND_DEG[i][1];   /* at 9 and 3 o'clock, the seam is where one ends and the next begins */
}

static void hold_step(void *var, int32_t v)
{
    s_hold = v;
    /* Just the Pause band's corner, upper left: the whole screen each frame
     * would make the fill stutter. */
    lv_area_t a;
    lv_obj_get_coords((lv_obj_t *)var, &a);
    a.x2 = a.x1 + BSP_LCD_H_RES / 2;
    a.y2 = a.y1 + BSP_LCD_V_RES / 2 + 20;
    lv_obj_invalidate_area((lv_obj_t *)var, &a);
}

/* Pause takes a hold, filling the band, and a tap only says so: stopping
 * costs effort, listening again doesn't. Paused, a tap listens again. */
static void on_pause_band(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *band = s_band[BAND_PAUSE];
    bool paused = recorder_paused();
    if (code == LV_EVENT_PRESSED && !paused) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, band);
        lv_anim_set_exec_cb(&a, hold_step);
        lv_anim_set_values(&a, 0, 1000);
        lv_anim_set_duration(&a, 650);   /* the long press */
        lv_anim_start(&a);
    } else if (code == LV_EVENT_LONG_PRESSED && !paused) {
        lv_anim_delete(band, hold_step);
        hold_step(band, 0);
        recorder_set_paused(true);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_anim_delete(band, hold_step);
        hold_step(band, 0);
    } else if (code == LV_EVENT_SHORT_CLICKED) {
        if (paused) {
            recorder_set_paused(false);
        } else {
            s_hold_hint_us = esp_timer_get_time() + 2000000;
        }
    }
}

static void on_open_list(lv_event_t *e);
static void on_open_settings(lv_event_t *e);
static void on_open_help(lv_event_t *e);

/* A band: full screen, so its labels sit at screen positions; an icon over
 * a word, and for some a second line. */
static lv_obj_t *band(int i, const char *icon, const lv_font_t *icon_font, const char *text, bool sub)
{
    lv_obj_t *b = lv_obj_create(s_main);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_center(b);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_set_style_arc_width(b, BAND_RO - BAND_RI, 0);
    lv_obj_set_style_arc_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_arc_color(b, BAND_FILL, 0);
    lv_obj_set_style_arc_color(b, BAND_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(b, BAND_INK, 0);
    lv_obj_add_event_cb(b, band_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(b, band_hit, LV_EVENT_HIT_TEST, NULL);
    s_band[i] = b;

    lv_obj_t *col = lv_obj_create(b);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);   /* the words' own size, to fit in the band */
    s_band_col[i] = col;
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 4, 0);
    s_band_icon[i] = lv_label_create(col);
    lv_obj_set_style_text_font(s_band_icon[i], icon_font, 0);
    lv_label_set_text(s_band_icon[i], icon);
    s_band_text[i] = lv_label_create(col);
    lv_obj_set_style_text_font(s_band_text[i], &lv_font_montserrat_18, 0);   /* fits each band's middle */
    lv_obj_set_style_text_align(s_band_text[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_band_text[i], text);
    if (sub) {
        s_band_sub[i] = lv_label_create(col);
        lv_obj_set_style_text_font(s_band_sub[i], &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(s_band_sub[i], BAND_SUB, 0);
        lv_label_set_text(s_band_sub[i], "");
    }
    return b;
}

/* How far a point (from the screen's centre) is inside band i: to its
 * nearest edge, in pixels; negative outside. */
static float band_clearance(int i, float x, float y)
{
    float r = sqrtf(x * x + y * y);
    float deg = atan2f(x, -y) * 180.0f / (float)M_PI;   /* compass: 0 at 12 o'clock, clockwise */
    if (deg < 0) {
        deg += 360.0f;
    }
    float lo = BAND_DEG[i][0], hi = BAND_DEG[i][1];
    float side = fminf(deg - lo, hi - deg) * (float)M_PI / 180.0f * r - BAND_SEAM / 2.0f;   /* along the arc, near enough */
    return fminf(fminf(r - BAND_RI, BAND_RO - r), side);
}

/* Band i's icon and words at the band's middle, all four alike. Each line
 * is checked on its own (a narrow icon over a wide word leaves the box's
 * corner empty), and a warning is logged if one pokes out, or comes closer
 * than BAND_PAD to an edge, so a new wording can't do it quietly. Runs
 * again when the words change. */
static void place_band_label(int i)
{
    lv_obj_t *col = s_band_col[i];
    float a = (BAND_DEG[i][0] + BAND_DEG[i][1]) / 2.0f * (float)M_PI / 180.0f, r = (BAND_RI + BAND_RO) / 2.0f;
    lv_obj_align(col, LV_ALIGN_CENTER, (int32_t)lroundf(r * sinf(a)), (int32_t)lroundf(-r * cosf(a)));
    lv_obj_update_layout(col);
    float margin = 1e9f;
    for (uint32_t c = 0; c < lv_obj_get_child_count(col); c++) {
        lv_obj_t *line = lv_obj_get_child(col, (int32_t)c);
        if (lv_obj_has_flag(line, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        lv_area_t box;
        lv_obj_get_coords(line, &box);
        /* Its corners, from the screen's centre. */
        float x0 = box.x1 - BSP_LCD_H_RES / 2.0f, x1 = box.x2 + 1 - BSP_LCD_H_RES / 2.0f;
        float y0 = box.y1 - BSP_LCD_V_RES / 2.0f, y1 = box.y2 + 1 - BSP_LCD_V_RES / 2.0f;
        margin = fminf(margin, fminf(fminf(band_clearance(i, x0, y0), band_clearance(i, x1, y0)),
                                     fminf(band_clearance(i, x0, y1), band_clearance(i, x1, y1))));
    }
    if (margin < BAND_PAD) {
        ESP_LOGW(TAG, "band %d: \"%s\" is %.0f px from its band's edge", i, lv_label_get_text(s_band_text[i]),
                 margin);
    }
}

/* Sets a band's words, and places them again if they changed. */
static void set_band_text(int i, lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text)) {
        lv_label_set_text(label, text);
        place_band_label(i);
    }
}

static void build_bands(void)
{
    band(BAND_PAUSE, LV_SYMBOL_PAUSE, &lv_font_montserrat_28, "Pause", true);
    lv_label_set_text(s_band_sub[BAND_PAUSE], "hold");
    lv_obj_set_style_arc_color(s_band[BAND_PAUSE], AMBER, LV_STATE_CHECKED);   /* paused: Resume */
    lv_obj_add_event_cb(s_band[BAND_PAUSE], on_pause_band, LV_EVENT_ALL, NULL);
    band(BAND_HELP, "?", &lv_font_montserrat_34, "Help", false);
    lv_obj_add_event_cb(s_band[BAND_HELP], on_open_help, LV_EVENT_CLICKED, NULL);
    band(BAND_SET, LV_SYMBOL_SETTINGS, &lv_font_montserrat_28, "Settings", false);
    lv_obj_add_event_cb(s_band[BAND_SET], on_open_settings, LV_EVENT_CLICKED, NULL);
    band(BAND_LIST, LV_SYMBOL_LIST, &lv_font_montserrat_20, LV_SYMBOL_LIST " 0", true);
    /* The icon and the count on one line, big; how many more fit small
     * under it: the band is only so wide. */
    lv_obj_add_flag(s_band_icon[BAND_LIST], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(s_band_text[BAND_LIST], &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_font(s_band_sub[BAND_LIST], &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_band_sub[BAND_LIST], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_row(s_band_col[BAND_LIST], 2, 0);
    lv_obj_add_event_cb(s_band[BAND_LIST], on_open_list, LV_EVENT_CLICKED, NULL);
    for (int i = 0; i < BANDS; i++) {
        place_band_label(i);
    }

    /* Beside PWR on the rim, SAVE's colour: that button does what SAVE
     * does. PWR is 40 degrees above 3 o'clock; LVGL's angles start there. */
    s_pwr_arc = lv_arc_create(s_main);
    lv_obj_set_size(s_pwr_arc, 2 * 214, 2 * 214);
    lv_obj_center(s_pwr_arc);
    lv_arc_set_bg_angles(s_pwr_arc, 310, 330);
    lv_arc_set_value(s_pwr_arc, lv_arc_get_min_value(s_pwr_arc));
    lv_obj_remove_flag(s_pwr_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_pwr_arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_pwr_arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_pwr_arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_opa(s_pwr_arc, LV_OPA_TRANSP, LV_PART_KNOB);
}

/* ---- the tour: how to use it, on the real main screen ----
 *
 * The first time ? is tapped, and from Help after that. One idea a step,
 * shown on the screen it's about: the control it's about stays lit, the
 * rest dims, and the words sit where nothing is. The two side buttons are
 * lit on the rim beside them. Tap for the next step, swipe right for the one
 * before. The screen stays on while it's open. */

enum {
    T_RING = 1 << 0, T_SAVE = 1 << 1, T_PWR = 1 << 2, T_LIST = 1 << 3,
    T_PAUSE = 1 << 4, T_GEAR = 1 << 5, T_BOOT = 1 << 6, T_HELP = 1 << 7,
};

typedef struct {
    const char *title, *body;
    uint8_t lit;
    bool around;   /* the words above and below SAVE, which stays to be seen; else in its place */
} tour_step_t;

static const tour_step_t TOUR[] = {
    { "Always listening", "It keeps the last 3 minutes,\nthen lets them go. The ring\nmoves with what it hears.", T_RING, false },
    { "Heard something?", "SAVE, or the lit button,\nkeeps those 3 minutes.", T_SAVE | T_PWR, true },
    { "Still talking?", "Right after a save, Keep\nrecording goes on to STOP.", T_SAVE, true },
    { "Your recordings", "Play, name or protect them.\nWhen it's full, a save\nreplaces the oldest.", T_LIST, false },
    { "Pause, Settings", "Hold Pause to stop listening.\nSettings: mic, sound,\nscreen, storage.", T_PAUSE | T_GEAR, false },
    { "Screen off", "The lit button turns the\nscreen off. It keeps\nlistening all the same.", T_BOOT, false },
    { "On a laptop", "Plug in USB for WAV files\nand transcripts. Nothing\ngoes online.", T_HELP, false },
};
#define TOUR_N ((int)(sizeof(TOUR) / sizeof(TOUR[0])))

/* The side buttons' places on the rim, in LVGL's degrees (0 at 3 o'clock,
 * clockwise): PWR 40 degrees above 3 o'clock, BOOT 40 below. */
#define TOUR_PWR_DEG 320
#define TOUR_BOOT_DEG 40

static lv_obj_t *s_tour, *s_tour_title, *s_tour_body, *s_tour_hint;
static lv_obj_t *s_tour_mark[2], *s_tour_halo, *s_tour_dot[TOUR_N];
static int s_tour_step = -1;   /* -1: closed */

static bool tour_open_now(void)
{
    return s_tour_step >= 0;
}

/* The lit ones breathe, so the eye goes to them. */
static void tour_pulse(void *var, int32_t v)
{
    LV_UNUSED(var);
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_opa(s_tour_mark[i], (lv_opa_t)v, 0);
    }
    lv_obj_set_style_opa(s_tour_halo, (lv_opa_t)v, 0);
}

static void tour_show(int step)
{
    const tour_step_t *t = &TOUR[step];
    s_tour_step = step;
    lv_obj_t *lit[] = { s_ring, s_save, s_band[BAND_LIST], s_band[BAND_PAUSE], s_band[BAND_SET], s_band[BAND_HELP] };
    const uint8_t bits[] = { T_RING, T_SAVE, T_LIST, T_PAUSE, T_GEAR, T_HELP };
    for (uint32_t i = 0; i < lv_obj_get_child_count(s_main); i++) {
        lv_obj_t *o = lv_obj_get_child(s_main, (int32_t)i);
        if (o == s_tour) {
            continue;
        }
        /* Dim, to show where things are; gone where the tour's words go:
         * SAVE when they're in its place, and the status lines. */
        lv_opa_t opa = (o == s_save && !t->around) || o == s_batt || o == s_held || o == s_pwr_arc ? LV_OPA_TRANSP
                                                                                            : LV_OPA_20;
        for (int k = 0; k < (int)(sizeof(bits) / sizeof(bits[0])); k++) {
            if (o == lit[k] && (t->lit & bits[k])) {
                opa = LV_OPA_COVER;
            }
        }
        lv_obj_set_style_opa(o, opa, 0);
    }
    /* A ring round SAVE when it's lit (a lit band shows by being the one
     * not dimmed), and the side buttons lit on the rim. */
    lv_obj_set_flag(s_tour_halo, LV_OBJ_FLAG_HIDDEN, !(t->lit & T_SAVE));
    lv_obj_set_flag(s_tour_mark[0], LV_OBJ_FLAG_HIDDEN, !(t->lit & T_PWR));
    lv_obj_set_flag(s_tour_mark[1], LV_OBJ_FLAG_HIDDEN, !(t->lit & T_BOOT));
    /* The words: the title at the top, where nothing is; the rest across
     * the middle, the band the ring buttons leave free, or under SAVE when
     * it's the one lit. */
    lv_label_set_text(s_tour_title, t->title);
    lv_label_set_text(s_tour_body, t->body);
    lv_obj_align(s_tour_body, LV_ALIGN_CENTER, 0, t->around ? 150 : 0);
    /* Below, on the steps with room: how to go on, and how it ends. */
    lv_obj_set_flag(s_tour_hint, LV_OBJ_FLAG_HIDDEN, t->around);
    lv_label_set_text(s_tour_hint, step == 0 ? "Tap to go on" : step == TOUR_N - 1 ? "? shows this again" : "");
    for (int i = 0; i < TOUR_N; i++) {
        lv_obj_set_style_bg_color(s_tour_dot[i], i == step ? lv_color_white() : lv_color_hex(0x555555), 0);
    }
}

static void tour_close(void)
{
    s_tour_step = -1;
    lv_anim_delete(s_tour, tour_pulse);
    lv_obj_add_flag(s_tour, LV_OBJ_FLAG_HIDDEN);
    for (uint32_t i = 0; i < lv_obj_get_child_count(s_main); i++) {
        lv_obj_set_style_opa(lv_obj_get_child(s_main, (int32_t)i), LV_OPA_COVER, 0);
    }
}

static void tour_open(int step)
{
    lv_screen_load(s_main);
    lv_obj_move_foreground(s_tour);
    lv_obj_remove_flag(s_tour, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(s_main);   /* the buttons' places, for the rings round them */
    tour_show(step < 0 ? 0 : step >= TOUR_N ? TOUR_N - 1 : step);
    lv_anim_delete(s_tour, tour_pulse);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_tour);
    lv_anim_set_exec_cb(&a, tour_pulse);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_40);
    lv_anim_set_duration(&a, 800);
    lv_anim_set_reverse_duration(&a, 800);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

static void on_tour_tap(lv_event_t *e)
{
    if (s_tour_step + 1 < TOUR_N) {
        tour_show(s_tour_step + 1);
    } else {
        tour_close();
    }
}

/* Swipe right: the step before, or out from the first. */
static void tour_back(void)
{
    if (s_tour_step > 0) {
        tour_show(s_tour_step - 1);
    } else {
        tour_close();
    }
}

static void on_tour_start(lv_event_t *e)
{
    tour_open(0);
}

static lv_obj_t *tour_label(int y, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = label_at(s_tour, y);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

static lv_obj_t *tour_mark(int deg)
{
    lv_obj_t *m = lv_arc_create(s_tour);
    lv_obj_set_size(m, 452, 452);
    lv_obj_center(m);
    lv_arc_set_bg_angles(m, deg - 12, deg + 12);
    lv_arc_set_value(m, 0);
    lv_obj_remove_flag(m, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(m, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_color(m, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(m, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_opa(m, LV_OPA_TRANSP, LV_PART_KNOB);
    return m;
}

/* Over the main screen, above everything on it, hidden until it opens. */
static void build_tour(void)
{
    s_tour = lv_obj_create(s_main);
    lv_obj_remove_style_all(s_tour);
    lv_obj_set_size(s_tour, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_center(s_tour);
    lv_obj_add_flag(s_tour, LV_OBJ_FLAG_CLICKABLE);   /* a tap anywhere is Next, and reaches nothing under it */
    lv_obj_remove_flag(s_tour, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_tour, on_tour_tap, LV_EVENT_CLICKED, NULL);
    s_tour_halo = lv_obj_create(s_tour);
    lv_obj_remove_style_all(s_tour_halo);
    lv_obj_set_size(s_tour_halo, 196 + 18, 196 + 18);   /* round SAVE */
    lv_obj_center(s_tour_halo);
    lv_obj_set_style_radius(s_tour_halo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_tour_halo, 3, 0);
    lv_obj_set_style_border_color(s_tour_halo, lv_color_white(), 0);
    lv_obj_remove_flag(s_tour_halo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_tour_halo, LV_OBJ_FLAG_HIDDEN);
    s_tour_mark[0] = tour_mark(TOUR_PWR_DEG);
    s_tour_mark[1] = tour_mark(TOUR_BOOT_DEG);
    s_tour_title = tour_label(-186, &lv_font_montserrat_28, lv_color_white());   /* above the bands */
    s_tour_body = tour_label(0, &lv_font_montserrat_20, lv_color_hex(0xD0D0D0));
    s_tour_hint = tour_label(150, LV_FONT_DEFAULT, DIM);
    for (int i = 0; i < TOUR_N; i++) {
        lv_obj_t *d = lv_obj_create(s_tour);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 8, 8);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_align(d, LV_ALIGN_CENTER, (i - (TOUR_N - 1) / 2) * 16 - ((TOUR_N - 1) % 2) * 8, 204);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
        s_tour_dot[i] = d;
    }
    lv_obj_add_flag(s_tour, LV_OBJ_FLAG_HIDDEN);
}

static void build(void)
{
    s_main = screen();
    /* The state around the rim: what's held while listening, red and filling
     * while recording, dim while paused. Not touchable. */
    s_ring = lv_arc_create(s_main);
    lv_obj_set_size(s_ring, 452, 452);
    lv_obj_center(s_ring);
    lv_arc_set_rotation(s_ring, 270);
    lv_arc_set_bg_angles(s_ring, 0, 360);
    lv_arc_set_range(s_ring, 0, 1000);
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_ring, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(0x1E3036), LV_PART_MAIN);   /* there, even in a quiet room */
    lv_obj_set_style_opa(s_ring, LV_OPA_TRANSP, LV_PART_KNOB);

    /* SAVE in the middle, round, with what it would keep inside it: the one
     * capture action. Round it, the bands; what's going on in the gap above
     * them, the battery in the gap below. */
    s_held = label_at(s_main, -170);
    lv_obj_set_width(s_held, 280);
    lv_label_set_long_mode(s_held, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_held, LV_TEXT_ALIGN_CENTER, 0);
    s_save = button(s_main, 196, 196, 0, &lv_font_montserrat_48, &s_save_label, on_save);
    lv_obj_align(s_save_label, LV_ALIGN_CENTER, 0, -12);
    s_save_sub = lv_label_create(s_save);
    lv_obj_set_style_text_font(s_save_sub, &lv_font_montserrat_24, 0);
    lv_obj_align(s_save_sub, LV_ALIGN_CENTER, 0, 36);   /* "nothing new" 4 px inside SAVE's circle */
    s_batt = label_at(s_main, 156);
    lv_obj_set_style_text_font(s_batt, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(s_batt, LV_TEXT_ALIGN_CENTER, 0);   /* "charge soon" on a line of its own */
    lv_label_set_text(s_batt, "");
    build_bands();   /* after SAVE, so a touch at SAVE's corners reaches the band there */

    s_list_screen = screen();
    s_list = round_list(s_list_screen);
    lv_obj_t *undo_label;   /* over the list, low, for a few seconds after a delete */
    s_undo = button(s_list_screen, 220, 66, 156, &lv_font_montserrat_20, &undo_label, on_undo);
    lv_obj_set_style_bg_color(s_undo, AMBER, 0);
    lv_label_set_text(undo_label, LV_SYMBOL_REFRESH " Undo delete");
    lv_obj_add_flag(s_undo, LV_OBJ_FLAG_HIDDEN);

    build_settings();
    build_confirm();
    build_naming();
    build_manage();
    build_player();
    build_aod_and_intro();
    build_help();
    build_tour();
    lv_screen_load(settings_intro_seen() ? s_main : s_intro);
}

/* The Recordings band: how many, and how many more saves fit before one
 * replaces the oldest (amber at the last, red when full). */
static void show_room(int n)
{
    uint32_t used = 0, going = recorder_recording();
    bool replaceable = false;
    for (int i = 0; i < n; i++) {
        used += s_rows[i].parts;
        replaceable |= !s_rows[i].kept && s_rows[i].first != going;
    }
    uint32_t slots = recorder_slots(), free = slots > used ? slots - used : 0;
    /* The count big, by the list icon; under it how many more saves fit. */
    char count[24], room[24];
    snprintf(count, sizeof(count), LV_SYMBOL_LIST " %d", n);
    lv_obj_t *sub = s_band_sub[BAND_LIST];
    if (free == 1) {
        snprintf(room, sizeof(room), "1 more fits");
    } else if (free) {
        snprintf(room, sizeof(room), "%" PRIu32 " more fit", free);
    } else {
        snprintf(room, sizeof(room), "Full");   /* amber: saves replace the oldest; red: they can't */
    }
    set_band_text(BAND_LIST, s_band_text[BAND_LIST], count);
    set_band_text(BAND_LIST, sub, room);
    lv_obj_set_style_text_color(sub, free > 1 ? BAND_SUB : free == 1 || replaceable ? BAND_AMBER : BAND_RED, 0);
}

/* The dim ring's tick: once a second, its state; every minute it moves a
 * few pixels, so a ring lit for hours doesn't burn into the AMOLED. True
 * while it's showing, when refresh() has nothing else to do. */
static bool aod_tick(uint32_t *ticks)
{
    if (!atomic_load(&s_aod_on)) {
        return false;
    }
    if ((*ticks)++ % 4) {
        return true;
    }
    bool recording = recorder_save_state() == REC_SAVE_RECORDING;
    int64_t minute = esp_timer_get_time() / 60000000;
    int32_t size = 452 - (int32_t)(minute % 3) * 4;
    lv_obj_set_size(s_aod_ring, size, size);
    lv_obj_center(s_aod_ring);
    lv_arc_set_value(s_aod_ring, recording ? (int32_t)(recorder_recording_seconds() * 1000 / CONFIG_REC_KEEP_MAX_S)
                                           : 1000);
    lv_obj_set_style_arc_color(s_aod_ring, recording ? RED : recorder_paused() ? AMBER : lv_color_hex(0x546E7A),
                               LV_PART_INDICATOR);
    char len[16];
    format_len(recorder_recording_seconds(), len, sizeof(len));
    lv_label_set_text_fmt(s_aod_text, "%s%s", recording ? "REC " : recorder_paused() ? "Paused" : "",
                          recording ? len : "");
    lv_obj_set_y(s_aod_text, (int32_t)(minute % 5) * 3);
    return true;
}

static void on_aod_tap(lv_event_t *e)
{
    /* The setting decides, whether or not touch really slept. */
    if (!settings_tap_wake()) {
        ESP_LOGW(TAG, "tap on the dim ring with tap to wake off: ignored");
        return;
    }
    ESP_LOGI(TAG, "tap on the dim ring: waking");
    atomic_store(&s_wake_req, true);   /* the buttons task wakes it: not from LVGL's own task */
}

static void on_intro_done(lv_event_t *e)
{
    settings_set_intro_seen();
    lv_screen_load(s_main);
}

static void build_aod_and_intro(void)
{
    s_aod = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_aod, lv_color_black(), 0);
    lv_obj_remove_flag(s_aod, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_aod, on_aod_tap, LV_EVENT_CLICKED, NULL);
    s_aod_ring = lv_arc_create(s_aod);
    lv_obj_set_size(s_aod_ring, 452, 452);
    lv_obj_center(s_aod_ring);
    lv_arc_set_rotation(s_aod_ring, 270);
    lv_arc_set_bg_angles(s_aod_ring, 0, 360);
    lv_arc_set_range(s_aod_ring, 0, 1000);
    lv_obj_remove_flag(s_aod_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_aod_ring, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_aod_ring, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_aod_ring, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_opa(s_aod_ring, LV_OPA_TRANSP, LV_PART_KNOB);
    s_aod_text = lv_label_create(s_aod);
    lv_obj_set_style_text_font(s_aod_text, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_aod_text, RED, 0);
    lv_obj_align(s_aod_text, LV_ALIGN_CENTER, 0, 0);

    /* Once, the first time: what the two buttons do, which nothing else
     * on the device says. */
    s_intro = screen();
    lv_obj_t *title = label_at(s_intro, -128);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_label_set_text(title, "Two buttons");
    lv_obj_t *top = label_at(s_intro, -58);
    lv_obj_set_style_text_align(top, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(top, "Top: save the last 3 minutes.\nPress again to keep recording.\nHold 3 s to turn off.");
    lv_obj_t *bottom = label_at(s_intro, 24);
    lv_obj_set_style_text_align(bottom, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(bottom, "Bottom: screen on and off.\n? on the screen: all of it.");
    lv_obj_t *ok = round_button(s_intro, 88, 0, 122, &lv_font_montserrat_28, LV_SYMBOL_OK, on_intro_done, NULL);
    lv_obj_set_style_bg_color(ok, GREEN, 0);
}

/* The status line above SAVE: 24 px when it's one line that fits the gap
 * between the two top bands under the ring with 3 px to spare (205 px at
 * 170 above the middle), the longer ones at 20 on two lines. */
#define HELD_BIG_W 205
static void set_held(const char *text)
{
    if (!strcmp(lv_label_get_text(s_held), text)) {
        return;
    }
    lv_point_t size;
    lv_text_get_size(&size, text, &lv_font_montserrat_24, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    bool big = !strchr(text, '\n') && size.x <= HELD_BIG_W;
    lv_obj_set_style_text_font(s_held, big ? &lv_font_montserrat_24 : &lv_font_montserrat_20, 0);
    lv_label_set_text(s_held, text);
}

static void refresh(lv_timer_t *t)
{
    static uint32_t ticks;
    static int64_t done_at;
    static rec_save_state_t shown = -1;
    int64_t now = esp_timer_get_time();

    if (aod_tick(&ticks)) {
        return;   /* the dim ring, with the screen off: nothing else to draw */
    }
    if (tour_open_now()) {
        lv_display_trigger_activity(NULL);   /* reading it isn't idle: the screen stays on */
    }
    bool paused = recorder_paused();
    rec_save_state_t st = recorder_save_state();
    /* From the press on: the flash catches up behind it. */
    bool recording = st == REC_SAVE_RECORDING, saving = st == REC_SAVE_BUSY;
    /* Paused, the Pause band is Resume, amber; while recording there's one
     * thing to do, Stop, the big button, so it goes. */
    lv_obj_t *pb = s_band[BAND_PAUSE];
    lv_obj_set_flag(pb, LV_OBJ_FLAG_HIDDEN, recording);
    if (paused != lv_obj_has_state(pb, LV_STATE_CHECKED)) {
        lv_obj_set_state(pb, LV_STATE_CHECKED, paused);
        lv_label_set_text(s_band_icon[BAND_PAUSE], paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
        lv_obj_set_flag(s_band_sub[BAND_PAUSE], LV_OBJ_FLAG_HIDDEN, paused);
        set_band_text(BAND_PAUSE, s_band_text[BAND_PAUSE], paused ? "Resume" : "Pause");
        lv_obj_set_style_text_color(pb, paused ? lv_color_hex(0x1A1A1A) : BAND_INK, 0);   /* dark on amber */
    }
    /* The ring: the mic's level while listening, a sweep while saving, red
     * and filling while recording, empty while paused. */
    int32_t level = (int32_t)(recorder_level() * 1000);
    int32_t ring = recording ? (int32_t)(recorder_recording_seconds() * 1000 / CONFIG_REC_KEEP_MAX_S)
                   : saving  ? (int32_t)(now / 1000 % 1000)
                   : paused  ? 0
                             : level;
    lv_arc_set_value(s_ring, ring < 1000 ? ring : 1000);
    lv_obj_set_style_arc_color(s_ring, recording ? RED : saving ? lv_theme_get_color_primary(s_save)
                                       : level > 700 ? lv_color_hex(0x80CBC4) : lv_color_hex(0x546E7A),
                               LV_PART_INDICATOR);
    char held[16];
    const char *status;   /* the line above SAVE */
    bool offer = st == REC_SAVE_DONE && atomic_load(&s_quiet) && recorder_saved_ms() < CONTINUE_MS &&
                 recorder_can_continue();
    lv_obj_set_style_text_color(s_held, recording ? RED : paused ? AMBER : offer ? lv_color_white() : DIM, 0);
    if (recording) {
        status = "Recording";
        format_len(recorder_recording_seconds(), held, sizeof(held));
        lv_label_set_text(s_save_sub, held);
    } else if (saving) {
        status = "Saving...";
        lv_label_set_text(s_save_sub, "");
    } else if (paused) {
        status = "Paused: not listening";
        lv_label_set_text(s_save_sub, "paused");
    } else {
        bool hint = now < s_hold_hint_us;   /* a tap on Pause */
        bool nothing = now < atomic_load(&s_nothing_until);   /* a save with nothing new */
        status = nothing ? "Nothing new since\nthe last save"
                 : hint  ? "Hold to pause"
                 : offer ? "Saved. Press PWR again\nto keep recording"
                         : "Listening";
        if (hint || nothing) {
            lv_obj_set_style_text_color(s_held, lv_color_white(), 0);
        }
        /* What SAVE would keep: what's come since the last save. */
        uint32_t fresh = recorder_seconds();
        format_len(fresh, held, sizeof(held));
        if (fresh) {
            lv_label_set_text_fmt(s_save_sub, "last %s", held);
        } else {
            lv_label_set_text(s_save_sub, "nothing new");
        }
    }
    lv_bar_set_value(s_e_level, level / 10, LV_ANIM_OFF);   /* the mic dial's: paused or not */

    if (st != shown) {
        shown = st;
        done_at = now;
        /* Offer a name for what was just saved, on the screen: not after
         * PWR, which may be in a pocket. Not over a name being typed. */
        uint32_t slot, seq;
        if (st == REC_SAVE_DONE && !atomic_load(&s_quiet) && lv_screen_active() != s_kb_screen &&
            recorder_last_saved(&slot, &seq)) {
            lv_obj_t *active = lv_screen_active();
            open_naming(slot, seq, "Saved " LV_SYMBOL_OK,
                        active == s_name_screen ? s_name_back : active);
        }
    }
    lv_obj_t *active = lv_screen_active();
    if (atomic_exchange(&s_slept, false) && (active == s_name_screen || active == s_kb_screen)) {
        name_done();   /* the screen went off: don't wake up mid-name */
    } else if (active == s_name_screen && lv_display_get_inactive_time(NULL) > NAME_PROMPT_MS) {
        name_done();
    }
    bool recent = now - done_at < 2000000;
    bool full = st == REC_SAVE_FULL && now - done_at < 5000000;
    bool bad = (st == REC_SAVE_FAILED && recent) || full;
    if (recording) {
        lv_label_set_text(s_save_label, "STOP");
    } else if (saving) {
        lv_label_set_text(s_save_label, "...");
    } else if (st == REC_SAVE_DONE && recent) {
        lv_label_set_text(s_save_label, LV_SYMBOL_OK);
    } else if (bad) {
        lv_label_set_text(s_save_label, full ? "FULL" : "FAILED");
    } else if (paused) {
        lv_label_set_text(s_save_label, "RESUME");
    } else {
        lv_label_set_text(s_save_label, "SAVE");
    }
    set_held(full ? "Full: unprotect or\ndelete one" : status);
    /* SAVE's colour, and the arc beside PWR, which does what SAVE does. */
    bool amber = paused && !recording && !saving && !bad;
    lv_color_t accent = bad || recording ? RED : amber ? AMBER : lv_theme_get_color_primary(s_save);
    lv_obj_set_style_bg_color(s_save, accent, 0);
    lv_obj_set_style_arc_color(s_pwr_arc, accent, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_save, amber ? lv_color_hex(0x1A1A1A) : lv_color_white(), 0);
    lv_obj_set_style_text_font(s_save_label, amber ? &lv_font_montserrat_34 : &lv_font_montserrat_48, 0);

    uint32_t changes = recorder_changes(), going = recorder_recording();
    if (changes != s_list_changes || (ticks % 20 == 0 && lv_screen_active() != s_list_screen)) {
        int n = recorder_recordings(s_rows, MAX_ROWS);
        show_room(n);
    }
    if (!lv_obj_has_flag(s_undo, LV_OBJ_FLAG_HIDDEN) && now > s_undo_until) {
        show_undo(false);
    }
    if (lv_screen_active() == s_list_screen && (changes != s_list_changes || going != s_list_going)) {
        fill_list();
        s_list_going = going;
    }
    s_list_changes = changes;
    if (lv_screen_active() == s_player) {
        refresh_player();
    }

    if (ticks++ % 20 == 0) {   /* every 5 s */
        /* Hours left from how fast it has fallen since USB came out, once
         * it has fallen 3 %, read against a measured discharge since the
         * percent doesn't fall evenly: no guess about the load. */
        static int64_t since_us;
        static int since_pct = -1;
        pmu_power_t p;
        if (pmu_last_power(&p) == ESP_OK && p.battery_pct >= 0) {   /* no I2C on this task */
            if (p.usb || p.charging) {
                since_pct = -1;
            } else if (since_pct < 0 || p.battery_pct > since_pct) {
                since_pct = p.battery_pct;
                since_us = now;
            }
            char left[16] = "";
            int fallen = since_pct - p.battery_pct;
            if (since_pct >= 0 && fallen >= 3) {
                double hours = batt_hours_left(since_pct, p.battery_pct, (double)(now - since_us) / 3.6e9);
                if (hours >= 0) {
                    snprintf(left, sizeof(left), "   ~%d h", (int)(hours + 0.5));
                }
            }
            if (atomic_load(&s_battery_low) && !p.usb && !p.charging) {
                snprintf(left, sizeof(left), "\ncharge soon");   /* under it: one line is too wide for the gap */
            }
            /* A battery filled to its level, then the bolt while charging
             * or the plug on USB when it's full. */
            const char *cell = p.battery_pct >= 80   ? LV_SYMBOL_BATTERY_FULL
                               : p.battery_pct >= 55 ? LV_SYMBOL_BATTERY_3
                               : p.battery_pct >= 30 ? LV_SYMBOL_BATTERY_2
                               : p.battery_pct >= 10 ? LV_SYMBOL_BATTERY_1
                                                     : LV_SYMBOL_BATTERY_EMPTY;
            lv_label_set_text_fmt(s_batt, "%s %s%d%%%s", cell,
                                  p.charging ? LV_SYMBOL_CHARGE " " : p.usb ? LV_SYMBOL_USB " " : "", p.battery_pct,
                                  left);
            lv_obj_set_style_text_color(s_batt, atomic_load(&s_battery_low) ? RED : lv_color_white(), 0);
        } else {
            lv_label_set_text(s_batt, LV_SYMBOL_USB);
        }
    }
}

esp_err_t ui_start(void)
{
    power_lock();   /* made now, before any task sleeps or wakes the screen */
    power_unlock();
    s_rows = heap_caps_malloc(MAX_ROWS * sizeof(*s_rows), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_rows, ESP_ERR_NO_MEM, TAG, "no memory");
    ESP_RETURN_ON_ERROR(display_start(), TAG, "display");
    ESP_RETURN_ON_ERROR(esp_lv_adapter_lock(-1), TAG, "lock");
    build();
    lv_timer_create(refresh, 250, NULL);
    esp_lv_adapter_unlock();
    return ESP_OK;
}

/* ---- screenshots (tools/snap.py) ---- */

/* With the adapter's lock. */
static esp_err_t show_named(const char *screen)
{
    int n = recorder_recordings(s_rows, MAX_ROWS);
    rec_recording_t newest = n ? s_rows[0] : (rec_recording_t){ 0 };
    if (tour_open_now()) {
        tour_close();   /* a screen of its own, or the tour from its step */
    }
    if (!strcmp(screen, "main")) {
        lv_screen_load(s_main);
    } else if (!strcmp(screen, "list")) {
        on_open_list(NULL);
    } else if (!strcmp(screen, "help")) {
        round_list_show(s_help_list, 1);   /* the topics: a screenshot isn't the first ? */
        lv_screen_load(s_help);
    } else if (!strcmp(screen, "help-topic")) {
        open_topic(0);
    } else if (!strcmp(screen, "intro")) {
        lv_screen_load(s_intro);
    } else if (!strcmp(screen, "aod")) {
        uint32_t ticks = 0;
        lv_screen_load(s_aod);   /* as it shows, but at full brightness */
        atomic_store(&s_aod_on, true);
        aod_tick(&ticks);
        atomic_store(&s_aod_on, false);
    } else if (!strcmp(screen, "confirm")) {
        on_power_off(NULL);
    } else if (!strncmp(screen, "tour", 4) && (!screen[4] || (screen[4] >= '0' && screen[4] <= '9'))) {
        tour_open(screen[4] ? screen[4] - '0' : 0);   /* tour, or tour0 to tour6: that step */
    } else if (!strcmp(screen, "delete-all")) {
        on_open_list(NULL);
        on_delete_all(NULL);
    } else if (!strcmp(screen, "delete-downloaded")) {
        on_open_list(NULL);
        on_delete_downloaded(NULL);
    } else if (!strcmp(screen, "settings") || !strcmp(screen, "settings-tap")) {
        on_open_settings(NULL);
        if (screen[8]) {
            round_list_show(s_set_list, 1 + SET_TAP);   /* Tap to wake in the middle */
        }
    } else if (!strcmp(screen, "dial")) {
        open_dial(SET_GAIN);
    } else if (!strcmp(screen, "keyboard-123") || !strcmp(screen, "keyboard-sym")) {
        on_type(NULL);   /* empty, as for a new name */
        on_kb_mode(NULL);
        if (screen[9] == 's') {
            on_kb_mode(NULL);
        }
    } else if (!n) {
        return ESP_ERR_NOT_FOUND;   /* the rest show a recording */
    } else if (!strcmp(screen, "name") || !strcmp(screen, "keyboard")) {
        open_naming(newest.slots[0], newest.first, "Saved " LV_SYMBOL_OK, s_main);
        if (screen[0] == 'k') {
            on_type(NULL);
        }

    } else if (!strcmp(screen, "manage")) {
        open_manage(newest.first, s_list_screen);
    } else if (!strcmp(screen, "player")) {
        open_player(&newest);
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t ui_snapshot(const char *screen, uint8_t **rgb565, uint32_t *w, uint32_t *h)
{
    ui_wake();   /* LVGL's timers, which fill the labels, run only while it's awake */
    ESP_RETURN_ON_ERROR(esp_lv_adapter_lock(-1), TAG, "lock");
    esp_err_t err = screen && *screen ? show_named(screen) : ESP_OK;
    esp_lv_adapter_unlock();
    ESP_RETURN_ON_ERROR(err, TAG, "no screen %s", screen);
    vTaskDelay(pdMS_TO_TICKS(600));   /* a couple of refreshes: labels, the list, the player */

    uint32_t size = BSP_LCD_H_RES * BSP_LCD_V_RES * 2;
    uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "no memory");
    lv_draw_buf_t draw;
    ESP_RETURN_ON_ERROR(esp_lv_adapter_lock(-1), TAG, "lock");
    lv_result_t res = lv_draw_buf_init(&draw, BSP_LCD_H_RES, BSP_LCD_V_RES, LV_COLOR_FORMAT_RGB565, BSP_LCD_H_RES * 2,
                                       buf, size);
    if (res == LV_RESULT_OK) {
        lv_obj_t *active = lv_screen_active();
        ESP_LOGI(TAG, "snapshot of %s", active == s_aod ? "the dim ring" : active == s_player ? "the player" : "a screen");
        res = lv_snapshot_take_to_draw_buf(active, LV_COLOR_FORMAT_RGB565, &draw);
    }
    /* Never left asking to delete or power off, where a stray tap would. */
    if (screen && (!strcmp(screen, "confirm") || !strncmp(screen, "delete-", 7))) {
        lv_screen_load(s_main);
    }
    esp_lv_adapter_unlock();
    if (screen && !strcmp(screen, "player")) {
        recorder_play_stop();   /* opening it started playback */
    }
    if (res != LV_RESULT_OK || draw.header.w != BSP_LCD_H_RES || draw.header.h != BSP_LCD_V_RES) {
        free(buf);
        ESP_LOGE(TAG, "snapshot failed");
        return ESP_FAIL;
    }
    *rgb565 = buf;
    *w = BSP_LCD_H_RES;
    *h = BSP_LCD_V_RES;
    return ESP_OK;
}
