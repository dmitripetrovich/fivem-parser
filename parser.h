#pragma once

#define PARSER_VERSION "1.1.7"

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
} ScannedMsg;

typedef enum {
        SCANNER_IDLE,
        SCANNER_SEARCHING,
        SCANNER_DENIED,
        SCANNER_ATTACHED,
} ScannerStatus;

int scanner_start(void);
void scanner_stop(void);
int scanner_is_running(void);
int scanner_poll(ScannedMsg *out, int max_count);
ScannerStatus scanner_status(void);

typedef enum {
        CODE_NONE,
        CODE_PALETTE,
        CODE_RGB,
        CODE_RESET,
        CODE_IGNORED,
} ColorCode;

int color_code_at(const char *s, ColorCode *kind, unsigned *value);
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

enum PngResult {
        PNG_OK,
        PNG_NO_FONT,
        PNG_TOO_LARGE,
        PNG_WRITE_FAILED,
};

ImVec4 color_palette(int code);
std::vector<ColorSeg> parse_segments(const char *raw);
PngResult export_chat_png(const char *output_path, const std::vector<const ChatLine *> &lines, int wrap_width, int scale, float bg_r, float bg_g, float bg_b, float bg_a);

template <typename F>
void for_each_word(const char *p, const char *end, F &&fn) {
        while (p < end && *p) {
                const char *word_end = p;
                while (word_end < end && *word_end && *word_end != ' ') word_end++;
                if (word_end < end && *word_end == ' ') word_end++;
                fn(p, word_end);
                p = word_end;
        }
}
#endif
