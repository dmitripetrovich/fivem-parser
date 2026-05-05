#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_image_write.h"
#include "stb_truetype.h"
#include "chat.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

static const float FONT_SIZE = 14.0f;
static const int PAD_X = 10;
static const int PAD_Y = 6;
static const int LINE_SPACING = 3;
static const int OUTLINE_R = 1;

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
                fread(buf.data(), 1, (size_t)sz, f);
                fclose(f);
                return true;
        }
        return false;
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
        int bx0, by0, bx1, by1;
        stbtt_GetCodepointBitmapBox(font, codepoint, scale, scale, &bx0, &by0, &bx1, &by1);
        int bw = bx1 - bx0;
        int bh = by1 - by0;
        if (bw <= 0 || bh <= 0) return;
        unsigned char *bmp = (unsigned char *)malloc((size_t)(bw * bh));
        if (!bmp) return;
        stbtt_MakeCodepointBitmap(font, bmp, bw, bh, bw, scale, scale, codepoint);
        int dst_x = px + bx0;
        int dst_y = py + ascent + by0;
        for (int gy = 0; gy < bh; gy++) {
                for (int gx = 0; gx < bw; gx++) {
                        int fx = dst_x + gx;
                        int fy = dst_y + gy;
                        if (fx < 0 || fx >= img_w || fy < 0 || fy >= img_h) continue;
                        float a = (bmp[gy * bw + gx] / 255.0f) * alpha_mul;
                        if (a < 0.004f) continue;
                        composite_pixel(img + (fy * img_w + fx) * 4, r, g, b, a);
                }
        }
        free(bmp);
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

static void render_string_outlined(unsigned char *img, int img_w, int img_h, int *pen_x, int pen_y, const char *text, int len, float r, float g, float b, stbtt_fontinfo *font, float scale, int ascent) {
        int start_x = *pen_x;
        static const int OX[] = {-1, 0, 1, -1, 1, -1, 0, 1};
        static const int OY[] = {-1, -1, -1, 0, 0, 1, 1, 1};
        for (int o = 0; o < 8; o++) {
                int tmp_x = start_x + OX[o] * OUTLINE_R;
                render_string(img, img_w, img_h, &tmp_x, pen_y + OY[o] * OUTLINE_R, text, len, 0.0f, 0.0f, 0.0f, 0.85f, font, scale, ascent);
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

bool export_chat_png(const char *output_path, const std::vector<ChatLine> &lines, int wrap_width, bool show_timestamps, float bg_r, float bg_g, float bg_b, float bg_a) {
        if (lines.empty()) return false;
        std::vector<unsigned char> font_data;
        if (!load_font_file(font_data)) return false;
        stbtt_fontinfo font;
        if (!stbtt_InitFont(&font, font_data.data(), 0)) return false;
        float scale = stbtt_ScaleForPixelHeight(&font, FONT_SIZE);
        int ascent, descent, line_gap;
        stbtt_GetFontVMetrics(&font, &ascent, &descent, &line_gap);
        int scaled_ascent  = (int)ceilf(ascent * scale);
        int scaled_descent = (int)ceilf(-descent * scale);
        int line_h = scaled_ascent + scaled_descent + LINE_SPACING;
        int max_w = 64;
        for (const ChatLine &cl : lines) {
                int w = 0;
                if (show_timestamps && !cl.timestamp.empty()) {
                        std::string ts = cl.timestamp + " ";
                        w += measure_string_width(&font, scale, ts.c_str(), (int)ts.size());
                }
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
                if (show_timestamps && !cl.timestamp.empty()) {
                        std::string ts = cl.timestamp + " ";
                        count_span(ts.c_str(), (int)ts.size());
                }
                for (const ColorSeg &seg : cl.segments)
                        count_span(seg.text.c_str(), (int)seg.text.size());
                total_rows += rows;
        }
        int img_w = content_w + PAD_X * 2 + OUTLINE_R * 2;
        int img_h = total_rows * line_h + PAD_Y * 2 + OUTLINE_R * 2;
        std::vector<unsigned char> pixels((size_t)(img_w * img_h * 4), 0);
        if (bg_a > 0.001f) {
                unsigned char br = (unsigned char)(bg_r * 255.0f + 0.5f);
                unsigned char bg = (unsigned char)(bg_g * 255.0f + 0.5f);
                unsigned char bb = (unsigned char)(bg_b * 255.0f + 0.5f);
                unsigned char ba = (unsigned char)(bg_a * 255.0f + 0.5f);
                for (int j = 0; j < img_w * img_h; j++) {
                        pixels[j*4+0] = br;
                        pixels[j*4+1] = bg;
                        pixels[j*4+2] = bb;
                        pixels[j*4+3] = ba;
                }
        }
        const int start_x = PAD_X + OUTLINE_R;
        const int content_right = start_x + content_w;
        int pen_y = PAD_Y + OUTLINE_R;
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
                                render_string_outlined(pixels.data(), img_w, img_h, &pen_x, pen_y, p, (int)(word_end - p), r, g, b, &font, scale, scaled_ascent);
                                p = word_end;
                        }
                };
                if (show_timestamps && !cl.timestamp.empty()) {
                        std::string ts = cl.timestamp + " ";
                        render_span(ts.c_str(), (int)ts.size(), 0.55f, 0.55f, 0.55f);
                }
                for (const ColorSeg &seg : cl.segments)
                        render_span(seg.text.c_str(), (int)seg.text.size(), seg.color.x, seg.color.y, seg.color.z);
                pen_y += line_h;
        }
        int ok = stbi_write_png(output_path, img_w, img_h, 4, pixels.data(), img_w * 4);
        return ok != 0;
}
