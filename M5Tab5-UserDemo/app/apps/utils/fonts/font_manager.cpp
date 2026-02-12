/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "font_manager.h"
#include <hal/hal.h>
#include <mooncake_log.h>
#include <cstdio>
#if defined(ESP_PLATFORM)
#include <dirent.h>
#include <cstring>
#endif

#if LV_USE_TINY_TTF
#include <src/libs/tiny_ttf/lv_tiny_ttf.h>
#endif

static const std::string _tag = "font";

namespace {

struct CachedFont {
    int32_t size = 0;
    lv_font_t* font = nullptr;
};

CachedFont s_cached;

static const char* kFontFsPathPrimary = "/sd/font.ttf";

static void dump_sd_root()
{
#if !defined(ESP_PLATFORM)
    return;
#else
    DIR* dir = opendir("/sd");
    if (!dir) {
        mclog::tagWarn(_tag, "opendir /sd failed");
        return;
    }
    int n = 0;
    while (auto* e = readdir(dir)) {
        if (e->d_name[0] == '\0') {
            continue;
        }
        mclog::tagInfo(_tag, "sd: {}", e->d_name);
        if (++n >= 24) {
            break;
        }
    }
    closedir(dir);
#endif
}

static bool file_exists(const char* path)
{
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        return false;
    }
    std::fclose(f);
    return true;
}

static std::string to_lower_copy(const std::string& s)
{
    std::string out = s;
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
    }
    return out;
}

static bool locate_font(std::string& out_fs_path, std::string& out_lvgl_path)
{
    out_fs_path.clear();
    out_lvgl_path.clear();

    if (file_exists(kFontFsPathPrimary)) {
        out_fs_path = kFontFsPathPrimary;
        out_lvgl_path = "S:/sd/font.ttf";
        return true;
    }

#if defined(ESP_PLATFORM)
    // FATFS can be configured case-sensitive; try locate font.ttf case-insensitively.
    DIR* dir = opendir("/sd");
    if (!dir) {
        return false;
    }
    std::string want = "font.ttf";
    while (auto* e = readdir(dir)) {
        if (e->d_name[0] == '\0') {
            continue;
        }
        std::string name = e->d_name;
        if (to_lower_copy(name) == want) {
            out_fs_path = std::string("/sd/") + name;
            out_lvgl_path = std::string("S:/sd/") + name;
            closedir(dir);
            return true;
        }
    }
    closedir(dir);
#endif

    return false;
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

    if (!GetHAL()->ensureSdCardMounted()) {
        mclog::tagWarn(_tag, "sd not mounted");
        return nullptr;
    }

    if (s_cached.font) {
        lv_tiny_ttf_destroy(s_cached.font);
        s_cached.font = nullptr;
        s_cached.size = 0;
    }

    std::string fs_path;
    std::string lvgl_path;
    if (!locate_font(fs_path, lvgl_path)) {
        mclog::tagWarn(_tag, "missing /sd/font.ttf");
        dump_sd_root();
        return nullptr;
    }

    // Chinese-heavy UIs are glyph-dense. The default tiny_ttf glyph cache (256) can thrash and cause long
    // render times, potentially tripping the task watchdog. Use a larger cache and disable kerning.
    lv_font_t* f = lv_tiny_ttf_create_file_ex(lvgl_path.c_str(), font_size, LV_FONT_KERNING_NONE, 2048);
    if (!f) {
        mclog::tagError(_tag, "lv_tiny_ttf_create_file failed");
        return nullptr;
    }

    s_cached.size = font_size;
    s_cached.font = f;
    mclog::tagInfo(_tag, "loaded font size={} path={}", font_size, lvgl_path);
    return s_cached.font;
#endif
}
