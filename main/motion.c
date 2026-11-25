#include "motion.h"

#include <stdlib.h>

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "motion";

#define ADDR 0x6B
#define WHO_AM_I 0x00
#define ID 0x05
#define CTRL1 0x02   /* bit 6: registers auto-increment */
#define CTRL2 0x03   /* accelerometer: full scale (bits 6:4), rate (3:0) */
#define CTRL7 0x08   /* bit 0: accelerometer on */
#define AX_L 0x35
#define ACC_2G_LP21 0x0D   /* +-2 g, 21 Hz low-power: 16384 per g */
#define MOVED 1600         /* about 0.1 g on any axis */

static i2c_master_dev_handle_t s_dev;
static int16_t s_rest[3];
static bool s_ok, s_have_rest;

static esp_err_t put(uint8_t reg, uint8_t v)
{
    return i2c_master_transmit(s_dev, (uint8_t[]){ reg, v }, 2, 50);
}

esp_err_t motion_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = ADDR, .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "add");
    uint8_t reg = WHO_AM_I, id = 0;
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, &id, 1, 50);
    if (err != ESP_OK || id != ID) {
        ESP_LOGW(TAG, "no QMI8658 (%s, id 0x%02x)", esp_err_to_name(err), id);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(put(CTRL1, 0x40), TAG, "ctrl1");
    ESP_RETURN_ON_ERROR(put(CTRL2, ACC_2G_LP21), TAG, "ctrl2");
    ESP_RETURN_ON_ERROR(put(CTRL7, 0x01), TAG, "ctrl7");
    s_ok = true;
    ESP_LOGI(TAG, "QMI8658 ready, accelerometer at 21 Hz low power");
    return ESP_OK;
}

bool motion_ok(void)
{
    return s_ok;
}

bool motion_moved(void)
{
    if (!s_ok) {
        return false;
    }
    uint8_t reg = AX_L, raw[6];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, raw, sizeof(raw), 50) != ESP_OK) {
        return false;
    }
    int16_t a[3];
    bool moved = false;
    for (int i = 0; i < 3; i++) {
        a[i] = (int16_t)(raw[2 * i] | raw[2 * i + 1] << 8);
        moved |= s_have_rest && abs(a[i] - s_rest[i]) > MOVED;
    }
    if (moved || !s_have_rest) {
        /* Where it rests now; a slow drift adds up to a move in the end. */
        for (int i = 0; i < 3; i++) {
            s_rest[i] = a[i];
        }
        s_have_rest = true;
    }
    return moved;
}
