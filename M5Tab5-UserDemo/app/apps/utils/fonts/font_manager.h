/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <lvgl.h>

namespace fonts {

/**
 * @brief Load a Chinese-capable font from SD card ("/sd/font.ttf") via LVGL tiny_ttf.
 *
 * Returns nullptr if SD card/font file is missing or tiny_ttf is unavailable.
 */
const lv_font_t* get_sd_ttf_font(int32_t font_size);

}  // namespace fonts

