/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

/*
 * Minimal AXP2101 access (muse-gadget-sdk's muse_pmu, less the power-key
 * latch: GPIO3 reads PWR on the 1.75C): battery and USB status, unused rails
 * off, and soft power-off.
 */

typedef struct {
    int battery_pct;   /* -1 when no battery is connected */
    int battery_mv;
    bool charging;
    bool usb;
} pmu_power_t;

/* Holding PWR 10 s cuts the power whatever the firmware does. */
esp_err_t pmu_init(i2c_master_bus_handle_t bus);

/* Turns off every rail but these, by their enable bits: dcdc has DCDC1-5 in
 * bits 0-4; ldo has ALDO1-4, BLDO1-2, CPUSLDO and DLDO1 in bits 0-7 and DLDO2
 * in bit 8. Turns nothing on. */
esp_err_t pmu_keep_rails(uint8_t dcdc, uint16_t ldo);

esp_err_t pmu_read_power(pmu_power_t *out);

/* The last reading pmu_read_power() made, without touching the I2C bus: for
 * the screen's task, which mustn't wait on a bus it shares with touch. */
esp_err_t pmu_last_power(pmu_power_t *out);

/* Soft power-off: the AXP2101 cuts every rail. PWR turns the board back on. */
esp_err_t pmu_power_off(void);
