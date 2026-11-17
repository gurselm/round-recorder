#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

/*
 * The board's AXP2101 power chip, as much as the recorder needs: battery and
 * USB status, turning unused rails off, and powering off. The PWR button
 * itself is read on GPIO3, not here.
 */

typedef struct {
    int battery_pct;   /* -1 when no battery is connected */
    int battery_mv;
    bool charging;
    bool usb;
} pmu_power_t;

/* Also makes holding PWR 10 s cut the power whatever the firmware does. */
esp_err_t pmu_init(i2c_master_bus_handle_t bus);

/* Turns off every rail but these, by their enable bits: dcdc has DCDC1-5 in
 * bits 0-4; ldo has ALDO1-4, BLDO1-2, CPUSLDO and DLDO1 in bits 0-7 and DLDO2
 * in bit 8. Turns nothing on. */
esp_err_t pmu_keep_rails(uint8_t dcdc, uint16_t ldo);

esp_err_t pmu_read_power(pmu_power_t *out);

/* The last reading pmu_read_power() made, without touching the I2C bus: for
 * the screen's task, which mustn't wait on a bus it shares with touch. */
esp_err_t pmu_last_power(pmu_power_t *out);

/* The AXP2101 cuts every rail. PWR turns the board back on. */
esp_err_t pmu_power_off(void);
