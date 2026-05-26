#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_image_write.h"
#include "stb_truetype.h"
#include "parser.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <unordered_map>

static const float BASE_FONT_SIZE = 14.0f;
static const int BASE_PAD_X = 10;
static const int BASE_PAD_Y = 6;
static const int BASE_LINE_SPACING = 3;
static const int BASE_OUTLINE_R = 1;

static bool load_font_file(std::vector<unsigned char> &buf) {
        static const char *candidates[] = { "C:\\Windows\\Fonts\\arialbd.ttf", "C:\\Windows\\Fonts\\arial.ttf", "C:\\Windows\\Fonts\\verdanab.ttf", "C:\\Windows\\Fonts\\verdana.ttf" };
        for (const char *path : candidates) {
                FILE *f = fopen(path, "rb");
                if (!f) continue;
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (sz <= 0) { fclose(f); continue; }
                buf.resize((size_t)sz);
                size_t read_n = fread(buf.data(), 1, (size_t)sz, f);
                fclose(f);
                if ((long)read_n != sz) { buf.clear(); continue; }
                return true;
        }
        return false;
}

struct CachedGlyph {
        unsigned char *bmp;
        int bw, bh, bx0, by0;
};

static std::unordered_map<int, CachedGlyph> s_glyph_cache;

static const CachedGlyph *get_glyph(stbtt_fontinfo *font, float scale, int codepoint) {
        auto it = s_glyph_cache.find(codepoint);
        if (it != s_glyph_cache.end())
                return &it->second;
        CachedGlyph g;
        stbtt_GetCodepointBitmapBox(font, codepoint, scale, scale, &g.bx0, &g.by0, nullptr, nullptr);
        int bx1, by1;
        stbtt_GetCodepointBitmapBox(font, codepoint, scale, scale, &g.bx0, &g.by0, &bx1, &by1);
        g.bw = bx1 - g.bx0;
        g.bh = by1 - g.by0;
        if (g.bw <= 0 || g.bh <= 0) {
                g.bmp = nullptr;
                g.bw = 0;
                g.bh = 0;
        } else {
                g.bmp = (unsigned char *)malloc((size_t)(g.bw * g.bh));
                if (g.bmp)
                        stbtt_MakeCodepointBitmap(font, g.bmp, g.bw, g.bh, g.bw, scale, scale, codepoint);
        }
        auto ins = s_glyph_cache.insert({codepoint, g});
        return &ins.first->second;
}

static void free_glyph_cache(void) {
        for (auto &kv : s_glyph_cache)
                free(kv.second.bmp);
        s_glyph_cache.clear();
}

static inline void composite_pixel(unsigned char *dst, float sr, float sg, float sb, float sa) {
        float da = dst[3] / 255.0f;
        float out_a = sa + da * (1.0f - sa);
        if (out_a < 0.001f) return;
        dst[0] = (unsigned char)(((sa * sr + da * (1.0f - sa) * (dst[0] / 255.0f)) / out_a) * 255.0f + 0.5f);
        dst[1] = (unsigned char)(((sa * sg + da * (1.0f - sa) * (dst[1] / 255.0f)) / out_a) * 255.0f + 0.5f);
        dst[2] = (unsigned char)(((sa * sb + da * (1.0f - sa) * (dst[2] / 255.0f)) / out_a) * 255.0f + 0.5f);
        dst[3] = (unsigned char)(out_a * 255.0f + 0.5f);
}

static void render_glyph(unsigned char *img, int img_w, int img_h, int px, int py, int codepoint, float r, float g, float b, float alpha_mul, stbtt_fontinfo *font, float scale, int ascent) {
        const CachedGlyph *gl = get_glyph(font, scale, codepoint);
        if (!gl || !gl->bmp) return;
        int dst_x = px + gl->bx0;
        int dst_y = py + ascent + gl->by0;
        for (int gy = 0; gy < gl->bh; gy++) {
                for (int gx = 0; gx < gl->bw; gx++) {
                        int fx = dst_x + gx;
                        int fy = dst_y + gy;
                        if (fx < 0 || fx >= img_w || fy < 0 || fy >= img_h) continue;
                        float a = (gl->bmp[gy * gl->bw + gx] / 255.0f) * alpha_mul;
                        if (a < 0.004f) continue;
                        composite_pixel(img + (fy * img_w + fx) * 4, r, g, b, a);
                }
        }
}

static void render_string(unsigned char *img, int img_w, int img_h, int *pen_x, int pen_y, const char *text, int len, float r, float g, float b, float alpha_mul, stbtt_fontinfo *font, float scale, int ascent) {
        for (int i = 0; i < len && text[i]; i++) {
                int cp = (unsigned char)text[i];
                render_glyph(img, img_w, img_h, *pen_x, pen_y, cp, r, g, b, alpha_mul, font, scale, ascent);
                int advance, lsb;
                stbtt_GetCodepointHMetrics(font, cp, &advance, &lsb);
                if (text[i + 1]) {
                        int kern = stbtt_GetCodepointKernAdvance(font, cp, (unsigned char)text[i + 1]);
                        advance += kern;
                }
                *pen_x += (int)(advance * scale + 0.5f);
        }
}

static void render_string_outlined(unsigned char *img, int img_w, int img_h, int *pen_x, int pen_y, const char *text, int len, float r, float g, float b, stbtt_fontinfo *font, float scale, int ascent, int outline_r) {
        int start_x = *pen_x;
        static const int OX[] = {-1, 0, 1, -1, 1, -1, 0, 1};
        static const int OY[] = {-1, -1, -1, 0, 0, 1, 1, 1};
        for (int o = 0; o < 8; o++) {
                int tmp_x = start_x + OX[o] * outline_r;
                render_string(img, img_w, img_h, &tmp_x, pen_y + OY[o] * outline_r, text, len, 0.0f, 0.0f, 0.0f, 0.85f, font, scale, ascent);
        }
        render_string(img, img_w, img_h, pen_x, pen_y, text, len, r, g, b, 1.0f, font, scale, ascent);
}

static int measure_string_width(stbtt_fontinfo *font, float scale, const char *text, int len) {
        int w = 0;
        for (int i = 0; i < len && text[i]; i++) {
                int cp = (unsigned char)text[i];
                int advance, lsb;
                stbtt_GetCodepointHMetrics(font, cp, &advance, &lsb);
                if (text[i + 1]) {
                        int kern = stbtt_GetCodepointKernAdvance(font, cp, (unsigned char)text[i + 1]);
                        advance += kern;
                }
                w += (int)(advance * scale + 0.5f);
        }
        return w;
}

bool export_chat_png(const char *output_path, const std::vector<ChatLine> &lines, int wrap_width, int render_scale, float bg_r, float bg_g, float bg_b, float bg_a) {
        if (lines.empty()) return false;
        if (render_scale < 1) render_scale = 1;
        if (render_scale > 3) render_scale = 3;
        const float font_size = BASE_FONT_SIZE * render_scale;
        const int pad_x = BASE_PAD_X * render_scale;
        const int pad_y = BASE_PAD_Y * render_scale;
        const int line_spacing = BASE_LINE_SPACING * render_scale;
        const int outline_r = BASE_OUTLINE_R * render_scale;
        wrap_width *= render_scale;
        std::vector<unsigned char> font_data;
        if (!load_font_file(font_data)) return false;
        stbtt_fontinfo font;
        if (!stbtt_InitFont(&font, font_data.data(), 0)) return false;
        float scale = stbtt_ScaleForPixelHeight(&font, font_size);
        int ascent, descent, line_gap;
        stbtt_GetFontVMetrics(&font, &ascent, &descent, &line_gap);
        int scaled_ascent  = (int)ceilf(ascent * scale);
        int scaled_descent = (int)ceilf(-descent * scale);
        int line_h = scaled_ascent + scaled_descent + line_spacing;
        int max_w = 64;
        for (const ChatLine &cl : lines) {
                int w = 0;
                for (const ColorSeg &seg : cl.segments)
                        w += measure_string_width(&font, scale, seg.text.c_str(), (int)seg.text.size());
                if (w > max_w) max_w = w;
        }
        int content_w = (max_w < wrap_width) ? max_w : wrap_width;
        int total_rows = 0;
        for (const ChatLine &cl : lines) {
                int pen_x = 0;
                int rows = 1;
                auto count_span = [&](const char *text, int len) {
                        const char *p = text;
                        const char *end = text + len;
                        while (p < end && *p) {
                                const char *word_end = p;
                                while (word_end < end && *word_end && *word_end != ' ') word_end++;
                                if (word_end < end && *word_end == ' ') word_end++;
                                if (word_end == p) { p++; continue; }
                                int word_w = measure_string_width(&font, scale, p, (int)(word_end - p));
                                if (pen_x > 0 && pen_x + word_w > content_w) {
                                        pen_x = 0;
                                        rows++;
                                }
                                pen_x += word_w;
                                p = word_end;
                        }
                };
                for (const ColorSeg &seg : cl.segments)
                        count_span(seg.text.c_str(), (int)seg.text.size());
                total_rows += rows;
        }
        int img_w = content_w + pad_x * 2 + outline_r * 2;
        int img_h = total_rows * line_h + pad_y * 2 + outline_r * 2;
        if (img_h > 32000 || img_w > 32000) return false;
        size_t pixel_count = (size_t)img_w * (size_t)img_h;
        if (pixel_count > 64 * 1024 * 1024) return false;
        std::vector<unsigned char> pixels;
        try {
                pixels.resize(pixel_count * 4, 0);
        } catch (...) {
                return false;
        }
        if (bg_a > 0.001f) {
                unsigned char br = (unsigned char)(bg_r * 255.0f + 0.5f);
                unsigned char bg = (unsigned char)(bg_g * 255.0f + 0.5f);
                unsigned char bb = (unsigned char)(bg_b * 255.0f + 0.5f);
                unsigned char ba = (unsigned char)(bg_a * 255.0f + 0.5f);
                unsigned char tmpl[4] = {br, bg, bb, ba};
                for (size_t j = 0; j < pixel_count; j++)
                        memcpy(pixels.data() + j * 4, tmpl, 4);
        }
        const int start_x = pad_x + outline_r;
        const int content_right = start_x + content_w;
        int pen_y = pad_y + outline_r;
        for (const ChatLine &cl : lines) {
                int pen_x = start_x;
                auto render_span = [&](const char *text, int len, float r, float g, float b) {
                        const char *p = text;
                        const char *end = text + len;
                        while (p < end && *p) {
                                const char *word_end = p;
                                while (word_end < end && *word_end && *word_end != ' ') word_end++;
                                if (word_end < end && *word_end == ' ') word_end++;
                                if (word_end == p) { p++; continue; }
                                int word_w = measure_string_width(&font, scale, p, (int)(word_end - p));
                                if (pen_x > start_x && pen_x + word_w > content_right) {
                                        pen_x = start_x;
                                        pen_y += line_h;
                                }
                                render_string_outlined(pixels.data(), img_w, img_h, &pen_x, pen_y, p, (int)(word_end - p), r, g, b, &font, scale, scaled_ascent, outline_r);
                                p = word_end;
                        }
                };
                for (const ColorSeg &seg : cl.segments)
                        render_span(seg.text.c_str(), (int)seg.text.size(), seg.color.x, seg.color.y, seg.color.z);
                pen_y += line_h;
        }
        free_glyph_cache();
        int ok = stbi_write_png(output_path, img_w, img_h, 4, pixels.data(), img_w * 4);
        return ok != 0;
}
