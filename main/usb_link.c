#include "usb_link.h"

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rec_proto.h"
#include "rec_ring.h"
#include "recorder.h"
#include "ui.h"

static const char *TAG = "usb_link";

static void out(const char *line, size_t n)
{
    /* One driver call: the TX ring takes all of it or waits, so a log line
     * can come before or after it but not inside. */
    usb_serial_jtag_write_bytes(line, n, pdMS_TO_TICKS(1000));
}

static int64_t clock_time(void)
{
    return recorder_time();
}

static int snap(const char *screen, uint8_t **rgb565, uint32_t *w, uint32_t *h)
{
    return ui_snapshot(screen, rgb565, w, h) == ESP_OK ? 0 : -1;
}

static void link_task(void *arg)
{
    const rec_proto_t proto = {
        .version = "2",
        .slots = recorder_slots(),
        .seconds = CONFIG_REC_SECONDS,
        .list = recorder_list,
        .clip = recorder_clip,
        .read = recorder_read,
        .del = recorder_delete,
        .set_name = recorder_set_name,
        .set_kept = recorder_set_kept,
        .set_pulled = recorder_set_pulled,
        .set_time = recorder_set_time,
        .time = clock_time,
        .recording = recorder_recording,
        .snap = snap,
        .tap = ui_tap,
        .out = out,
    };
    char line[128];
    size_t len = 0;
    bool overflow = false;
    for (;;) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == '\n' || c == '\r') {
            if (len && !overflow) {
                line[len] = '\0';
                rec_proto_line(&proto, line);
            }
            len = 0;
            overflow = false;
        } else if (len < sizeof(line) - 1) {
            line[len++] = (char)c;
        } else {
            overflow = true;   /* not a command of ours: drop it to the newline */
        }
    }
}

esp_err_t usb_link_start(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 512;
    cfg.tx_buffer_size = 8192;   /* a DATA line is 4 KB */
    ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&cfg), TAG, "driver");
    /* Logs through the driver too, sharing its TX ring with the replies. */
    usb_serial_jtag_vfs_use_driver();
    return ESP_OK;
}

esp_err_t usb_link_serve(void)
{
    /* After recorder_start(): the task takes the slot count once. */
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(link_task, "usb_link", 10240, NULL, 5, NULL, 0) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}
