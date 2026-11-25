/*
 * Round recorder: keeps the last CONFIG_REC_SECONDS heard by the Waveshare
 * ESP32-S3-Touch-AMOLED-1.75C's mics, and saves them when asked.
 *
 * Top button (PWR): what the big button on the screen does: save what's held,
 * or stop the recording going on; pressed again soon after a save, it keeps
 * recording on from it. Hold 3 s: power off. Off USB it warns at 15 %, and at
 * a low battery saves and powers off itself.
 * Bottom button (BOOT): screen on or off. The screen goes off by itself after
 * the time set in Settings; touch can't wake it, a button can.
 * Standby (Settings, off by default): off USB, quiet and still for a while,
 * the chip sleeps between short checks for sound.
 */
#include <inttypes.h>

#include "audio.h"
#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "batt_log.h"
#include "motion.h"
#include "pmu.h"
#include "recorder.h"
#include "settings.h"
#include "ui.h"
#include "usb_link.h"

static const char *TAG = "main";

#define PWR_GPIO GPIO_NUM_3    /* high while PWR is held (a BSS138 inverts it) */
#define BOOT_GPIO GPIO_NUM_0   /* low while held */
#define TAP_US 40000           /* shorter is a bounce */
#define OFF_HOLD_US 3000000
#define BATTERY_EVERY_US 5000000   /* the screen shows this reading too (pmu_last_power) */
#define LOW_READINGS 6         /* in a row, so one odd reading doesn't power it off */
#define FINISH_MS 30000        /* the longest a last save may take before the power goes */
#define WARN_PCT 15            /* off USB: a sound and a line on the screen, once */
#define CONTINUE_MS 10000      /* after a save, PWR again keeps recording; the screen says so */
#define STILL_EVERY_US 1000000 /* the motion sensor, read while listening */
#define CHECK_FIRST_MS 1000    /* standby: asleep this long before the first check for sound, */
#define CHECK_MAX_MS 4000      /* doubling up to this */
#define LISTEN_MS 250          /* each check awake: 100 ms to settle, the rest heard */
#define HEARD_READS 2          /* 20 ms reads over the room's hum in a check: sound */

typedef struct {
    gpio_num_t pin;
    int active;
    bool down;
    bool armed;   /* pressed since the task started, and not yet held long */
    int64_t since;
} button_t;

enum { NONE, TAP, HELD };

static int poll(button_t *b, int64_t now, int64_t hold_us)
{
    bool down = gpio_get_level(b->pin) == b->active;
    int ev = NONE;
    if (down && !b->down) {
        b->since = now;
        b->armed = true;
    } else if (!down && b->down && b->armed && now - b->since >= TAP_US) {
        ev = TAP;
    } else if (down && b->armed && now - b->since >= hold_us) {
        b->armed = false;
        ev = HELD;
    }
    if (!down) {
        b->armed = false;
    }
    b->down = down;
    return ev;
}

static void power_off(const char *why)
{
    ESP_LOGI(TAG, "powering off: %s", why);
    batt_log_power_off();
    ui_show_off();
    pmu_power_off();
}

/* Off USB: at WARN_PCT a warning, so the power-off doesn't surprise; at
 * CONFIG_REC_LOW_BATTERY_PCT, save, then power off before the battery gives
 * out mid-save. */
static void check_battery(void)
{
    static int low;
    static bool warned;
    pmu_power_t p;
    if (pmu_read_power(&p) != ESP_OK) {
        return;
    }
    batt_log_sample(&p, ui_awake(), recorder_recording() != 0, recorder_standby());
    bool on_battery = !p.usb && p.battery_pct >= 0;
    if (!on_battery || p.battery_pct > WARN_PCT) {
        warned = false;
        ui_battery_low(false);
    } else if (!warned) {
        warned = true;
        ESP_LOGW(TAG, "battery at %d%%", p.battery_pct);
        ui_battery_low(true);
        recorder_warn();
    }
    if (!CONFIG_REC_LOW_BATTERY_PCT || !on_battery || p.battery_pct > CONFIG_REC_LOW_BATTERY_PCT) {
        low = 0;
        return;
    }
    if (++low < LOW_READINGS) {
        return;
    }
    ESP_LOGW(TAG, "battery at %d%% (%d mV): saving", p.battery_pct, p.battery_mv);
    recorder_finish(true, FINISH_MS);
    power_off("battery low");
}

static int64_t s_moved_at;   /* when the board last moved, us */

/* Standby's way in: Settings has it on, it's off USB with nothing going on
 * and the screen off, and it has been quiet and still for a while. */
static bool standby_due(int64_t now)
{
    pmu_power_t p;
    rec_play_t play;
    recorder_play_status(&play);
    rec_save_state_t save = recorder_save_state();
    uint32_t after_ms = CONFIG_REC_STANDBY_AFTER_S * 1000;
    return settings_standby() && motion_ok() && pmu_last_power(&p) == ESP_OK && !p.usb && !p.charging &&
           !ui_awake() && !recorder_paused() && !play.first && save != REC_SAVE_BUSY &&
           save != REC_SAVE_RECORDING && recorder_quiet_ms() >= after_ms && now - s_moved_at >= after_ms * 1000LL;
}

/* Sleeps, waking to listen for a moment: after 1 s, then 2, then 4 s
 * apart. Sound, a move, USB or a button has it listening again; a button
 * press is then handled as usual. The ring keeps what it held. */
static void standby(int64_t *battery_at)
{
    recorder_set_standby(true);
    gpio_wakeup_enable(PWR_GPIO, GPIO_INTR_HIGH_LEVEL);
    gpio_wakeup_enable(BOOT_GPIO, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    uint32_t sleep_ms = CHECK_FIRST_MS, checks = 0;
    const char *why = NULL;
    while (!why) {
        esp_sleep_enable_timer_wakeup(sleep_ms * 1000ULL);
        esp_light_sleep_start();
        checks++;
        if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
            why = "a button";
            break;
        }
        uint32_t heard = recorder_heard();
        vTaskDelay(pdMS_TO_TICKS(LISTEN_MS));
        int64_t now = esp_timer_get_time();
        if (recorder_heard() - heard >= HEARD_READS) {
            why = "sound";
        } else if (motion_moved()) {
            s_moved_at = now;
            why = "a move";
        } else if (ui_awake()) {
            why = "the screen";
        }
        if (now - *battery_at > BATTERY_EVERY_US) {
            *battery_at = now;
            check_battery();   /* at a low battery it saves and powers off from here */
        }
        pmu_power_t p;
        if (!why && pmu_last_power(&p) == ESP_OK && (p.usb || p.charging)) {
            why = "USB";
        }
        sleep_ms = sleep_ms * 2 > CHECK_MAX_MS ? CHECK_MAX_MS : sleep_ms * 2;
    }
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    gpio_wakeup_disable(PWR_GPIO);
    gpio_wakeup_disable(BOOT_GPIO);
    recorder_set_standby(false);
    ESP_LOGI(TAG, "standby ended by %s after %" PRIu32 " checks", why, checks);
}

static void buttons_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = BIT64(PWR_GPIO) | BIT64(BOOT_GPIO),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&io);
    /* PWR may still be held from turning the board on: that isn't a press. */
    button_t pwr = { .pin = PWR_GPIO, .active = 1, .down = gpio_get_level(PWR_GPIO) == 1 };
    button_t boot = { .pin = BOOT_GPIO, .active = 0, .down = gpio_get_level(BOOT_GPIO) == 0 };
    int64_t battery_at = 0, still_at = 0;
    s_moved_at = esp_timer_get_time();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        int64_t now = esp_timer_get_time();
        int ev = poll(&pwr, now, OFF_HOLD_US);
        if (ev == TAP && recorder_paused()) {
            recorder_set_paused(false);   /* PWR does what SAVE shows: paused, listen again */
            ui_wake();
        } else if (ev == TAP) {
            /* Quietly: no name prompt from a press that may be in a pocket. A
             * second press soon after a save keeps recording on from it. */
            ui_quiet_save();
            if (recorder_saved_ms() < CONTINUE_MS && recorder_can_continue()) {
                recorder_continue();
            } else {
                /* Saves what's come since the last save, or stops the
                 * recording going on. */
                if (!recorder_save() && recorder_seconds() == 0) {
                    ui_nothing_new();
                }
            }
            ui_wake();   /* to show it */
        } else if (ev == HELD || ui_take_power_off()) {
            recorder_finish(false, FINISH_MS);   /* a recording going on keeps what it has */
            power_off(ev == HELD ? "PWR held" : "Settings");
        }
        if (now - battery_at > BATTERY_EVERY_US) {
            battery_at = now;
            check_battery();
        }
        if (ui_take_wake_request()) {
            ui_wake();   /* a tap on the dim ring */
        }
        if (poll(&boot, now, INT64_MAX) == TAP) {
            if (ui_awake()) {
                ui_sleep();
            } else {
                ui_wake();
            }
        }
        ui_sleep_if_idle((uint32_t)settings_screen_off_s() * 1000);
        settings_commit();
        if (now - still_at > STILL_EVERY_US) {
            still_at = now;
            if (motion_moved()) {
                s_moved_at = now;
            }
            if (standby_due(now)) {
                standby(&battery_at);
            }
        }
    }
}

void app_main(void)
{
    /* With the mics' I2S always on the chip can't light-sleep; it can still
     * slow down between bursts of work. */
    esp_pm_config_t pm = { .max_freq_mhz = 160, .min_freq_mhz = 40, .light_sleep_enable = false };
    ESP_ERROR_CHECK(esp_pm_configure(&pm));
    ESP_ERROR_CHECK(usb_link_start());
    ESP_ERROR_CHECK(bsp_i2c_init());
    if (pmu_init(bsp_i2c_get_handle()) == ESP_OK) {
        /* Only DCDC1 (VCC3V3) and ALDO1 (A3V3, the codecs) feed anything. */
        pmu_keep_rails(BIT(0), BIT(0));
    }
    motion_init(bsp_i2c_get_handle());   /* without it, no standby */
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(audio_init(settings_mic_gain_db(), settings_volume()));
    ESP_ERROR_CHECK(recorder_start());
    batt_log_init();   /* without room for it, no log */
    ESP_ERROR_CHECK(usb_link_serve());
    ESP_ERROR_CHECK(ui_start());
    xTaskCreatePinnedToCore(buttons_task, "buttons", 4096, NULL, 4, NULL, 0);
    ESP_LOGI(TAG, "round recorder running");
}
