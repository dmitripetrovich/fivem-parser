#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_image_write.h"
#include "stb_truetype.h"
#include "parser.h"
#include "platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <unordered_map>

static int decode_utf8(const char *text, int len, int pos, int *codepoint) {
        unsigned char c = (unsigned char)text[pos];
        if (c < 0x80) {
                *codepoint = c;
                return 1;
        }
        if ((c & 0xE0) == 0xC0 && pos + 1 < len) {
                unsigned char c1 = (unsigned char)text[pos + 1];
                if ((c1 & 0xC0) == 0x80) {
                        *codepoint = ((c & 0x1F) << 6) | (c1 & 0x3F);
                        if (*codepoint >= 0x80) return 2;
                }
        }
        if ((c & 0xF0) == 0xE0 && pos + 2 < len) {
                unsigned char c1 = (unsigned char)text[pos + 1];
                unsigned char c2 = (unsigned char)text[pos + 2];
                if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80) {
                        *codepoint = ((c & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
                        if (*codepoint >= 0x800) return 3;
                }
        }
        if ((c & 0xF8) == 0xF0 && pos + 3 < len) {
                unsigned char c1 = (unsigned char)text[pos + 1];
                unsigned char c2 = (unsigned char)text[pos + 2];
                unsigned char c3 = (unsigned char)text[pos + 3];
                if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80) {
                        *codepoint = ((c & 0x07) << 18) | ((c1 & 0x3F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
                        if (*codepoint >= 0x10000 && *codepoint <= 0x10FFFF) return 4;
                }
        }
        *codepoint = 0xFFFD;
        return 1;
}

static const float BASE_FONT_SIZE = 14.0f;
static const int BASE_PAD_X = 10;
static const int BASE_PAD_Y = 6;
static const int BASE_LINE_SPACING = 3;
static const int BASE_OUTLINE_R = 1;
static const int MAX_IMAGE_DIM = 32000;
static const size_t MAX_IMAGE_PIXELS = 64 * 1024 * 1024;

static bool load_export_font(std::vector<unsigned char> &buf) {
        static const char *candidates[] = { "arialbd.ttf", "arial.ttf", "verdanab.ttf", "verdana.ttf" };
        for (const char *name : candidates) {
                char path[PARSER_PATH_MAX];
                if (!platform_font_path(name, path, sizeof(path))) continue;
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

struct Glyph {
        int index;
        int advance;
        bool rasterized;
        unsigned char *bmp;
        int bw, bh, bx0, by0;
};

struct Font {
        stbtt_fontinfo info;
        float scale;
        int ascent;
        int line_h;
        std::unordered_map<int, Glyph> glyphs;

        ~Font() {
                for (auto &kv : glyphs)
                        free(kv.second.bmp);
        }

        Glyph &glyph(int codepoint) {
                auto it = glyphs.find(codepoint);
                if (it != glyphs.end())
                        return it->second;
                Glyph g = {};
                int lsb;
                g.index = stbtt_FindGlyphIndex(&info, codepoint);
                stbtt_GetGlyphHMetrics(&info, g.index, &g.advance, &lsb);
                return glyphs.insert({codepoint, g}).first->second;
        }

        void rasterize(Glyph &g) {
                g.rasterized = true;
                int bx1, by1;
                stbtt_GetGlyphBitmapBox(&info, g.index, scale, scale, &g.bx0, &g.by0, &bx1, &by1);
                g.bw = bx1 - g.bx0;
                g.bh = by1 - g.by0;
                if (g.bw <= 0 || g.bh <= 0) {
                        g.bw = 0;
                        g.bh = 0;
                        return;
                }
                g.bmp = (unsigned char *)malloc((size_t)(g.bw * g.bh));
                if (g.bmp)
                        stbtt_MakeGlyphBitmap(&info, g.bmp, g.bw, g.bh, g.bw, scale, scale, g.index);
        }
};

struct Canvas {
        unsigned char *pixels;
        int w, h;
};

template <typename F>
static int walk_glyphs(Font &font, const char *text, int len, F &&fn) {
        int x = 0;
        for (int i = 0; i < len && text[i];) {
                int cp;
                int bytes = decode_utf8(text, len, i, &cp);
                Glyph &g = font.glyph(cp);
                fn(g, x);
                int advance = g.advance;
                int next_i = i + bytes;
                if (next_i < len && text[next_i]) {
                        int next_cp;
                        decode_utf8(text, len, next_i, &next_cp);
                        advance += stbtt_GetGlyphKernAdvance(&font.info, g.index, font.glyph(next_cp).index);
                }
                x += (int)(advance * font.scale + 0.5f);
                i = next_i;
        }
        return x;
}

static int text_width(Font &font, const char *text, int len) {
        return walk_glyphs(font, text, len, [](Glyph &, int) {});
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

static void draw_glyph(Font &font, Canvas &img, Glyph &g, int px, int py, float r, float gr, float b, float alpha_mul) {
        if (!g.rasterized) font.rasterize(g);
        if (!g.bmp) return;
        int dst_x = px + g.bx0;
        int dst_y = py + font.ascent + g.by0;
        for (int gy = 0; gy < g.bh; gy++) {
                for (int gx = 0; gx < g.bw; gx++) {
                        int fx = dst_x + gx;
                        int fy = dst_y + gy;
                        if (fx < 0 || fx >= img.w || fy < 0 || fy >= img.h) continue;
                        float a = (g.bmp[gy * g.bw + gx] / 255.0f) * alpha_mul;
                        if (a < 0.004f) continue;
                        composite_pixel(img.pixels + (fy * img.w + fx) * 4, r, gr, b, a);
                }
        }
}

static void draw_text(Font &font, Canvas &img, int px, int py, const char *text, int len, float r, float g, float b, float alpha_mul) {
        walk_glyphs(font, text, len, [&](Glyph &glyph, int x) {
                draw_glyph(font, img, glyph, px + x, py, r, g, b, alpha_mul);
        });
}

static void draw_text_outlined(Font &font, Canvas &img, int px, int py, const char *text, int len, const ImVec4 &color, int outline_r) {
        static const int OX[] = {-1, 0, 1, -1, 1, -1, 0, 1};
        static const int OY[] = {-1, -1, -1, 0, 0, 1, 1, 1};
        for (int o = 0; o < 8; o++)
                draw_text(font, img, px + OX[o] * outline_r, py + OY[o] * outline_r, text, len, 0.0f, 0.0f, 0.0f, 0.85f);
        draw_text(font, img, px, py, text, len, color.x, color.y, color.z, 1.0f);
}

static int layout_line(Font &font, const ChatLine &line, int content_w, Canvas *img, int origin_x, int origin_y, int outline_r) {
        int pen_x = 0;
        int rows = 1;
        for (const ColorSeg &seg : line.segments) {
                const char *text = seg.text.c_str();
                for_each_word(text, text + seg.text.size(), [&](const char *word, const char *word_end) {
                        int len = (int)(word_end - word);
                        int word_w = text_width(font, word, len);
                        if (pen_x > 0 && pen_x + word_w > content_w) {
                                pen_x = 0;
                                rows++;
                        }
                        if (img)
                                draw_text_outlined(font, *img, origin_x + pen_x, origin_y + (rows - 1) * font.line_h, word, len, seg.color, outline_r);
                        pen_x += word_w;
                });
        }
        return rows;
}

static PngResult render_png(const char *output_path, const std::vector<const ChatLine *> &lines, const std::vector<unsigned char> &font_data, int wrap_width, int render_scale, float bg_r, float bg_g, float bg_b, float bg_a) {
        const float font_size = BASE_FONT_SIZE * render_scale;
        const int pad_x = BASE_PAD_X * render_scale;
        const int pad_y = BASE_PAD_Y * render_scale;
        const int line_spacing = BASE_LINE_SPACING * render_scale;
        const int outline_r = BASE_OUTLINE_R * render_scale;
        wrap_width *= render_scale;
        Font font;
        if (!stbtt_InitFont(&font.info, font_data.data(), 0)) return PNG_NO_FONT;
        font.scale = stbtt_ScaleForPixelHeight(&font.info, font_size);
        int ascent, descent;
        stbtt_GetFontVMetrics(&font.info, &ascent, &descent, nullptr);
        font.ascent = (int)ceilf(ascent * font.scale);
        font.line_h = font.ascent + (int)ceilf(-descent * font.scale) + line_spacing;
        int max_w = 64;
        for (const ChatLine *line : lines) {
                int w = 0;
                for (const ColorSeg &seg : line->segments)
                        w += text_width(font, seg.text.c_str(), (int)seg.text.size());
                if (w > max_w) max_w = w;
        }
        int content_w = (max_w < wrap_width) ? max_w : wrap_width;
        long long total_rows = 0;
        for (const ChatLine *line : lines)
                total_rows += layout_line(font, *line, content_w, nullptr, 0, 0, outline_r);
        long long img_w = content_w + pad_x * 2 + outline_r * 2;
        long long img_h = total_rows * font.line_h + pad_y * 2 + outline_r * 2;
        if (img_h > MAX_IMAGE_DIM || img_w > MAX_IMAGE_DIM) return PNG_TOO_LARGE;
        size_t pixel_count = (size_t)img_w * (size_t)img_h;
        if (pixel_count > MAX_IMAGE_PIXELS) return PNG_TOO_LARGE;
        std::vector<unsigned char> pixels;
        try {
                pixels.resize(pixel_count * 4, 0);
        } catch (...) {
                return PNG_TOO_LARGE;
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
        Canvas img = { pixels.data(), (int)img_w, (int)img_h };
        const int start_x = pad_x + outline_r;
        int pen_y = pad_y + outline_r;
        for (const ChatLine *line : lines)
                pen_y += layout_line(font, *line, content_w, &img, start_x, pen_y, outline_r) * font.line_h;
        return stbi_write_png(output_path, img.w, img.h, 4, pixels.data(), img.w * 4) ? PNG_OK : PNG_WRITE_FAILED;
}

PngResult export_chat_png(const char *output_path, const std::vector<const ChatLine *> &lines, int wrap_width, int render_scale, float bg_r, float bg_g, float bg_b, float bg_a) {
        if (lines.empty()) return PNG_WRITE_FAILED;
        if (render_scale < 1) render_scale = 1;
        if (render_scale > 3) render_scale = 3;
        std::vector<unsigned char> font_data;
        if (!load_export_font(font_data)) return PNG_NO_FONT;
        PngResult result = PNG_TOO_LARGE;
        for (int scale = render_scale; scale >= 1 && result == PNG_TOO_LARGE; scale--)
                result = render_png(output_path, lines, font_data, wrap_width, scale, bg_r, bg_g, bg_b, bg_a);
        return result;
}
