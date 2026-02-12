/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "font_manager.h"
#include <hal/hal.h>
#include <mooncake_log.h>
#include <cstdio>

#if LV_USE_TINY_TTF
#include <lv_tiny_ttf.h>
#endif

static const std::string _tag = "font";

namespace {

struct CachedFont {
    int32_t size = 0;
    lv_font_t* font = nullptr;
};

CachedFont s_cached;

bool ensure_sd_ready()
{
    if (!GetHAL()->ensureSdCardMounted()) {
        mclog::tagWarn(_tag, "sd not mounted");
        return false;
    }

    FILE* f = std::fopen("/sd/font.ttf", "rb");
    if (!f) {
        mclog::tagWarn(_tag, "missing /sd/font.ttf");
        return false;
    }
    std::fclose(f);
    return true;
}

}  // namespace

const lv_font_t* fonts::get_sd_ttf_font(int32_t font_size)
{
#if !LV_USE_TINY_TTF
    (void)font_size;
    mclog::tagWarn(_tag, "LV_USE_TINY_TTF disabled");
    return nullptr;
#else
    if (font_size <= 0) {
        return nullptr;
    }

    if (s_cached.font && s_cached.size == font_size) {
        return s_cached.font;
    }

    if (!ensure_sd_ready()) {
        return nullptr;
    }

    if (s_cached.font) {
        lv_tiny_ttf_destroy(s_cached.font);
        s_cached.font = nullptr;
        s_cached.size = 0;
    }

    lv_font_t* f = lv_tiny_ttf_create_file("S:/sd/font.ttf", font_size);
    if (!f) {
        mclog::tagError(_tag, "lv_tiny_ttf_create_file failed");
        return nullptr;
    }

    s_cached.size = font_size;
    s_cached.font = f;
    mclog::tagInfo(_tag, "loaded /sd/font.ttf size={}", font_size);
    return s_cached.font;
#endif
}

