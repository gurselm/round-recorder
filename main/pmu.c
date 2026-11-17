#include "pmu.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "pmu";

/* AXP2101 registers, from its datasheet. */
enum {
    AXP_ADDR = 0x34,
    R_POWER_STATUS = 0x00,     /* bit 5: VBUS good; bit 3: battery present */
    R_CHARGE_STATUS = 0x01,    /* bits 6-5: 01 while charging */
    R_COMMON = 0x10,           /* bit 0: power off */
    R_PWRKEY = 0x27,           /* bits 3-2: how long a PWR hold powers off */
    R_ADC_ON = 0x30,           /* bit 0: measure the battery's voltage */
    R_VBAT = 0x34,             /* and 0x35: 13 bits, 1 mV each */
    R_IRQ_ENABLE = 0x40,       /* three registers */
    R_IRQ_STATUS = 0x48,       /* three registers; writing 1 clears */
    R_DCDC_ON = 0x80,          /* bits 4-0: DCDC5-1 */
    R_LDO_ON = 0x90,           /* ALDO1-4, BLDO1-2, CPUSLDO, DLDO1 */
    R_LDO_ON2 = 0x91,          /* bit 0: DLDO2 */
    R_FUEL_GAUGE = 0xA4,       /* battery percent */
};

#define PWRKEY_OFF_MASK 0x0C
#define PWRKEY_OFF_10S 0x0C

static i2c_master_dev_handle_t s_axp;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static pmu_power_t s_last;
static bool s_last_ok;

static esp_err_t get(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_axp, &reg, 1, val, 1, 50);
}

static esp_err_t put(uint8_t reg, uint8_t val)
{
    const uint8_t msg[2] = { reg, val };
    return i2c_master_transmit(s_axp, msg, sizeof(msg), 50);
}

/* Sets the bits in mask to those of val, keeping the rest. */
static esp_err_t change(uint8_t reg, uint8_t mask, uint8_t val, uint8_t *was)
{
    uint8_t v;
    esp_err_t err = get(reg, &v);
    if (err == ESP_OK) {
        err = put(reg, (uint8_t)((v & ~mask) | (val & mask)));
    }
    if (was) {
        *was = v;
    }
    return err;
}

esp_err_t pmu_init(i2c_master_bus_handle_t bus)
{
    const i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev, &s_axp), TAG, "add device");

    /* A long hold on PWR powers off in hardware: the way out if the
     * firmware ever hangs. */
    ESP_RETURN_ON_ERROR(change(R_PWRKEY, PWRKEY_OFF_MASK, PWRKEY_OFF_10S, NULL), TAG, "no answer from the AXP2101");

    /* No interrupts: nothing here listens, and one left pending holds the
     * shared IRQ line low. */
    for (uint8_t i = 0; i < 3; i++) {
        ESP_RETURN_ON_ERROR(put(R_IRQ_ENABLE + i, 0x00), TAG, "irq enable");
        ESP_RETURN_ON_ERROR(put(R_IRQ_STATUS + i, 0xFF), TAG, "irq status");
    }

    ESP_RETURN_ON_ERROR(change(R_ADC_ON, 0x01, 0x01, NULL), TAG, "battery voltage");
    ESP_LOGI(TAG, "AXP2101 ready, PWR held 10 s powers off");
    return ESP_OK;
}

esp_err_t pmu_keep_rails(uint8_t dcdc, uint16_t ldo)
{
    ESP_RETURN_ON_FALSE(s_axp, ESP_ERR_INVALID_STATE, TAG, "no PMU");
    uint8_t dc_was, ldo_was, ldo2_was;
    /* Clearing an enable bit only: mask is what goes, val stays zero. The
     * DCDC register's top three bits aren't enables. */
    ESP_RETURN_ON_ERROR(change(R_DCDC_ON, (uint8_t)(0x1F & ~dcdc), 0, &dc_was), TAG, "dcdc");
    ESP_RETURN_ON_ERROR(change(R_LDO_ON, (uint8_t)~ldo, 0, &ldo_was), TAG, "ldo");
    ESP_RETURN_ON_ERROR(change(R_LDO_ON2, (ldo & 0x100) ? 0 : 0x01, 0, &ldo2_was), TAG, "dldo2");
    ESP_LOGI(TAG, "rails kept: DCDC %02x of %02x, LDO %02x of %02x, DLDO2 %d of %d", dc_was & dcdc & 0x1F,
             dc_was & 0x1F, ldo_was & ldo, ldo_was, (ldo2_was & (ldo >> 8)) & 1, ldo2_was & 1);
    return ESP_OK;
}

esp_err_t pmu_read_power(pmu_power_t *out)
{
    if (!s_axp) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t power, charge;
    ESP_RETURN_ON_ERROR(get(R_POWER_STATUS, &power), TAG, "power status");
    ESP_RETURN_ON_ERROR(get(R_CHARGE_STATUS, &charge), TAG, "charge status");
    bool battery = power & 0x08;
    pmu_power_t p = {
        .usb = power & 0x20,
        .charging = battery && (charge & 0x60) == 0x20,
        .battery_pct = -1,
        .battery_mv = 0,
    };
    if (battery) {
        uint8_t pct, v[2];
        if (get(R_FUEL_GAUGE, &pct) == ESP_OK && pct <= 100) {
            p.battery_pct = pct;
        }
        if (get(R_VBAT, &v[0]) == ESP_OK && get(R_VBAT + 1, &v[1]) == ESP_OK) {
            p.battery_mv = (v[0] & 0x1F) << 8 | v[1];
        }
    }
    *out = p;
    portENTER_CRITICAL(&s_lock);
    s_last = p;
    s_last_ok = true;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t pmu_last_power(pmu_power_t *out)
{
    portENTER_CRITICAL(&s_lock);
    bool ok = s_last_ok;
    *out = s_last;
    portEXIT_CRITICAL(&s_lock);
    return ok ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t pmu_power_off(void)
{
    if (!s_axp) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "powering off");
    return change(R_COMMON, 0x01, 0x01, NULL);
}
