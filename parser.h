#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
        int show_timestamps;
        int wrap_width;
        int png_bg_r, png_bg_g, png_bg_b, png_bg_a;
        int png_scale;
} Config;

extern Config g_config;

void config_load(void);
void config_save(void);

typedef struct {
        char raw[8192];
        char plain[8192];
} ScannedMsg;

int scanner_start(void);
void scanner_stop(void);
int scanner_is_running(void);
int scanner_poll(ScannedMsg *out, int max_count);

void strip_color_codes(char *s);
const char *stristr(const char *h, const char *n);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <string>
#include <vector>
#include "imgui.h"

struct ColorSeg {
        std::string text;
        ImVec4 color;
};

struct ChatLine {
        std::string timestamp;
        std::string raw;
        std::string plain;
        std::vector<ColorSeg> segments;
};

ImVec4 color_palette(int code);
std::vector<ColorSeg> parse_segments(const char *raw);
bool export_chat_png(const char *output_path, const std::vector<ChatLine> &lines, int wrap_width, int scale, float bg_r, float bg_g, float bg_b, float bg_a);
#endif
