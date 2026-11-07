#pragma once

#include "esp_err.h"

/* The USB serial port: logs, and the clip transfer (rec_proto.h). */
esp_err_t usb_link_start(void);   /* logs, as early as possible */
esp_err_t usb_link_serve(void);   /* commands, once the recorder is up */
