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
static const int OUTLINE_R = 1; // utline radius in pixels

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

bool export_chat_png(const char *output_path, const std::vector<ChatLine> &lines) {
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
                if (!cl.timestamp.empty()) {
                        std::string ts = cl.timestamp + " ";
                        w += measure_string_width(&font, scale, ts.c_str(), (int)ts.size());
                }
                for (const ColorSeg &seg : cl.segments)
                        w += measure_string_width(&font, scale, seg.text.c_str(), (int)seg.text.size());
                if (w > max_w) max_w = w;
        }
        int img_w = max_w + PAD_X * 2 + OUTLINE_R * 2;
        int img_h = (int)lines.size() * line_h + PAD_Y * 2 + OUTLINE_R * 2;
        std::vector<unsigned char> pixels((size_t)(img_w * img_h * 4), 0);
        for (int li = 0; li < (int)lines.size(); li++) {
                const ChatLine &cl = lines[li];
                int pen_x = PAD_X + OUTLINE_R;
                int pen_y = PAD_Y + OUTLINE_R + li * line_h;
                if (!cl.timestamp.empty()) {
                        std::string ts = cl.timestamp + " ";
                        render_string_outlined(pixels.data(), img_w, img_h, &pen_x, pen_y, ts.c_str(), (int)ts.size(), 0.55f, 0.55f, 0.55f, &font, scale, scaled_ascent);
                }
                for (const ColorSeg &seg : cl.segments) {
                        render_string_outlined(pixels.data(), img_w, img_h, &pen_x, pen_y, seg.text.c_str(), (int)seg.text.size(), seg.color.x, seg.color.y, seg.color.z, &font, scale, scaled_ascent);
                }
        }
        int ok = stbi_write_png(output_path, img_w, img_h, 4, pixels.data(), img_w * 4);
        return ok != 0;
}
