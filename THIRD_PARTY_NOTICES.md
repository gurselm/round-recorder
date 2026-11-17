# Third-party notices

The round recorder's own code is under the MIT license (`LICENSE`). The
firmware it builds also contains the code below, each under its own license.
The full license texts come with each component: ESP-IDF in its own tree,
the rest under `managed_components/` once `idf.py build` has fetched them.

## Linked into the firmware

| Component | License |
|---|---|
| [ESP-IDF](https://github.com/espressif/esp-idf) v6.0 (FreeRTOS, newlib, TinyUSB and the rest of its components) | Apache 2.0; its components carry their own licenses (MIT, BSD and others), listed in ESP-IDF's `COPYRIGHT.rst` |
| [LVGL](https://github.com/lvgl/lvgl) 9.5 | MIT |
| Montserrat font, as built into LVGL | SIL Open Font License 1.1 |
| Font Awesome symbols, as built into LVGL | SIL Open Font License 1.1 |
| [waveshare/esp32_s3_touch_amoled_1_75c](https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_1_75c) (board support) | Apache 2.0 |
| waveshare/esp_lcd_touch_cst9217 | Apache 2.0 |
| espressif/esp_lvgl_adapter, esp_codec_dev, esp_lcd_co5300, esp_lcd_touch, esp_io_expander, esp_lcd_panel_io_additions, esp_lv_decoder, esp_lv_fs, esp_mmap_assets, button, knob, usb | Apache 2.0 |
| espressif/libpng (libpng) | libpng license |
| espressif/zlib (zlib) | zlib license |
| espressif/qoi (QOI) | MIT |

## Laptop tools

`tools/pull.py` and `tools/sync.py` use only the Python standard library.
Transcripts use [faster-whisper](https://github.com/SYSTRAN/faster-whisper)
(MIT) and the Whisper models (MIT) if you install them.
