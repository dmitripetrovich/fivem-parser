#pragma once
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
bool export_chat_png(const char *output_path, const std::vector<ChatLine> &lines, int wrap_width, bool show_timestamps, float bg_r, float bg_g, float bg_b, float bg_a);
