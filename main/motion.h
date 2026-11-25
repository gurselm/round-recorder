#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

/*
 * The QMI8658 motion sensor on the board's I2C bus, accelerometer only, in
 * its low-power mode: just enough to tell whether the board has been moved.
 * The board package doesn't list it, but it's there (0x6b).
 */

esp_err_t motion_init(i2c_master_bus_handle_t bus);

/* Whether it has moved since the last call: tilted or shaken by more than
 * about a tenth of g against where it last rested. False if there's no
 * sensor (and motion_ok() says so). Called a second or so apart. */
bool motion_moved(void);
bool motion_ok(void);
