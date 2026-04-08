#include "chat.h"
#include <cstring>
#include <cstdio>
#include <cctype>

ImVec4 color_palette(int code) {
        switch (code) {
                case 0: return ImVec4(1.00f, 1.00f, 1.00f, 1.0f);
                case 1: return ImVec4(1.00f, 0.20f, 0.20f, 1.0f);
                case 2: return ImVec4(0.20f, 1.00f, 0.20f, 1.0f);
                case 3: return ImVec4(1.00f, 1.00f, 0.20f, 1.0f);
                case 4: return ImVec4(0.30f, 0.55f, 1.00f, 1.0f);
                case 5: return ImVec4(0.20f, 1.00f, 1.00f, 1.0f);
                case 6: return ImVec4(1.00f, 0.30f, 1.00f, 1.0f);
                case 7: return ImVec4(1.00f, 1.00f, 1.00f, 1.0f);
                case 8: return ImVec4(0.75f, 0.15f, 0.15f, 1.0f);
                case 9: return ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
                default: return ImVec4(1.00f, 1.00f, 1.00f, 1.0f);
        }
}

std::vector<ColorSeg> parse_segments(const char *raw) {
        std::vector<ColorSeg> segs;
        segs.reserve(8);
        ImVec4 color(1.0f, 1.0f, 1.0f, 1.0f);
        std::string cur;
        cur.reserve(128);
        for (int i = 0; raw[i]; i++) {
                if (raw[i] == '^' && raw[i + 1]) {
                        char nx = raw[i + 1];
                        if (nx == '#') {
                                int k = i + 2, h = 0;
                                while (raw[k] && h < 6 && isxdigit((unsigned char)raw[k])) {
                                        h++; k++;
                                }
                                if (h == 6) {
                                        if (!cur.empty()) { segs.push_back({std::move(cur), color}); cur.reserve(128); }
                                        char hex[7];
                                        memcpy(hex, raw + i + 2, 6);
                                        hex[6] = '\0';
                                        unsigned v;
                                        sscanf(hex, "%x", &v);
                                        color = ImVec4(((v >> 16) & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f, (v & 0xFF) / 255.0f, 1.0f);
                                        i = k - 1;
                                        continue;
                                }
                        }
                        if (nx >= '0' && nx <= '9') {
                                if (!cur.empty()) { segs.push_back({std::move(cur), color}); cur.reserve(128); }
                                color = color_palette(nx - '0');
                                i++;
                                continue;
                        }
                        if (nx == 'r' || nx == '~') {
                                if (!cur.empty()) { segs.push_back({std::move(cur), color}); cur.reserve(128); }
                                color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
                                i++;
                                continue;
                        }
                        if (nx == '*' || nx == '_') {
                                i++;
                                continue;
                        }
                }
                if (raw[i] == '~') {
                        int k = i + 1;
                        while (raw[k] && raw[k] != '~')
                                k++;
                        if (raw[k] == '~') {
                                i = k;
                                continue;
                        }
                }
                cur += raw[i];
        }
        if (!cur.empty())
                segs.push_back({std::move(cur), color});
        return segs;
}
