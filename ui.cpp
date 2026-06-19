#include <GLFW/glfw3.h>
#include "imgui.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cctype>
#include <string>
#include <vector>
#include <regex>
#include <algorithm>

#include "parser.h"
#include "platform.h"

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

static std::vector<ChatLine> s_chat;
static size_t s_total_chars = 0;

static bool s_show_filter = false;
static char s_kw_buf[4096] = "";
static bool s_use_regex = false;
static std::vector<int> s_flt_indices;
static size_t s_flt_chars = 0;
static std::string s_regex_error;

static bool s_open_about = false;

static int s_edit_line = -1;
static bool s_open_edit_popup = false;
static char s_edit_buf[1024] = "";

static bool s_bulk_select_mode = false;
static std::vector<bool> s_bulk_sel;
static bool s_flt_bulk_select_mode = false;
static std::vector<bool> s_flt_bulk_sel;

static float s_png_bg[4] = {0.0f, 0.0f, 0.0f, 0.0f};

static bool s_show_find = false;
static bool s_find_just_opened = false;
static char s_find_buf[256] = "";
static int s_find_result = -1;
static bool s_find_need_scroll = false;

static bool s_show_ts = true;
static bool s_live_mode = false;
static bool s_live_scroll = false;
static char s_path[PARSER_PATH_MAX * 2] = "";

static std::vector<float> s_line_heights;
static float s_cache_avail_w = 0.0f;
static int s_cache_wrap = 0;

static void invalidate_height_cache(void) {
        s_line_heights.clear();
        s_cache_avail_w = 0.0f;
        s_cache_wrap = 0;
}

static float render_chat_line(const ChatLine &line) {
        ImFont *font = ImGui::GetFont();
        const float font_size = ImGui::GetFontSize();
        const float line_h = ImGui::GetTextLineHeightWithSpacing();
        const float char_h = ImGui::GetTextLineHeight();
        float avail_w = ImGui::GetContentRegionAvail().x;
        if (avail_w <= 0.0f) avail_w = 200.0f;
        const ImVec2 item_pos = ImGui::GetCursorScreenPos();
        const float start_x = item_pos.x;
        const float wrap_limit = (avail_w < (float)g_config.wrap_width) ? avail_w : (float)g_config.wrap_width;
        const float max_x = start_x + wrap_limit;
        const ImVec2 win_pos = ImGui::GetWindowPos();
        const float win_h = ImGui::GetWindowHeight();
        const float cull_margin = 300.0f;
        const bool should_draw = (item_pos.y < win_pos.y + win_h + cull_margin) && (item_pos.y > win_pos.y - cull_margin);
        ImDrawList *dl = should_draw ? ImGui::GetWindowDrawList() : nullptr;
        float pen_x = start_x;
        float pen_y = item_pos.y;
        float total_h = char_h;
        auto process_span = [&](const char *text, ImU32 col) {
                if (!text || !*text) return;
                const char *p = text;
                while (*p) {
                        const char *word_end = p;
                        while (*word_end && *word_end != ' ') word_end++;
                        if (*word_end == ' ') word_end++;
                        if (word_end == p) { p++; continue; }
                        const float word_w = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, p, word_end).x;
                        if (pen_x > start_x && pen_x + word_w > max_x) {
                                pen_x = start_x;
                                pen_y += line_h;
                                total_h += line_h;
                        }
                        if (dl)
                                dl->AddText(font, font_size, ImVec2(pen_x, pen_y), col, p, word_end);
                        pen_x += word_w;
                        p = word_end;
                }
        };
        if (s_show_ts && !line.timestamp.empty()) {
                const char *ts = line.timestamp.c_str();
                int ts_len = (int)line.timestamp.size();
                const float ts_w = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, ts, ts + ts_len).x;
                const float sp_w = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, " ", nullptr).x;
                if (dl)
                        dl->AddText(font, font_size, ImVec2(pen_x, pen_y), IM_COL32(140, 140, 140, 255), ts, ts + ts_len);
                pen_x += ts_w + sp_w;
        }
        for (const auto &seg : line.segments)
                process_span(seg.text.c_str(), ImGui::ColorConvertFloat4ToU32(seg.color));
        ImGui::Dummy(ImVec2(avail_w, total_h));
        return total_h;
}

static std::string build_plain(const std::vector<ChatLine> &lines) {
        std::string out;
        out.reserve(s_total_chars);
        for (auto &l : lines) {
                if (s_show_ts && !l.timestamp.empty()) { out += l.timestamp; out += ' '; }
                out += l.plain;
                out += "\r\n";
        }
        return out;
}

static std::string build_plain_indices(const std::vector<int> &indices) {
        std::string out;
        out.reserve(s_flt_chars);
        for (int idx : indices) {
                auto &l = s_chat[idx];
                if (s_show_ts && !l.timestamp.empty()) { out += l.timestamp; out += ' '; }
                out += l.plain;
                out += "\r\n";
        }
        return out;
}

static bool match_line(const char *text, const std::string &kw, bool use_regex, const std::regex *pat) {
        if (use_regex) {
                try {
                        return std::regex_search(text, *pat);
                } catch (...) {
                        return false;
                }
        }
        return stristr(text, kw.c_str()) != NULL;
}

static void apply_filter(void) {
        s_flt_indices.clear();
        s_flt_indices.reserve(s_chat.size());
        s_flt_chars = 0;
        s_regex_error.clear();
        s_flt_bulk_select_mode = false;
        s_flt_bulk_sel.clear();
        if (s_chat.empty())
                return;
        char tmp[4096];
        strncpy(tmp, s_kw_buf, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        std::vector<std::string> kw_or, kw_and, kw_not;
        char *ctx = NULL;
        char *tok = strtok_s(tmp, "\r\n", &ctx);
        while (tok) {
                while (*tok == ' ') tok++;
                if (!*tok) { tok = strtok_s(NULL, "\r\n", &ctx); continue; }
                if (*tok == '+') {
                        tok++;
                        while (*tok == ' ') tok++;
                        if (*tok) kw_and.push_back(tok);
                } else if (*tok == '-') {
                        tok++;
                        while (*tok == ' ') tok++;
                        if (*tok) kw_not.push_back(tok);
                } else {
                        kw_or.push_back(tok);
                }
                tok = strtok_s(NULL, "\r\n", &ctx);
        }
        if (kw_or.empty() && kw_and.empty() && kw_not.empty()) {
                for (int i = 0; i < (int)s_chat.size(); i++)
                        s_flt_indices.push_back(i);
        } else {
                std::vector<std::regex> re_or, re_and, re_not;
                if (s_use_regex) {
                        auto compile = [&](const std::vector<std::string> &src, std::vector<std::regex> &dst) -> bool {
                                for (auto &kw : src) {
                                        if (kw.size() > 256) {
                                                s_regex_error = "Regex too long (max 256 chars): " + kw.substr(0, 32) + "...";
                                                return false;
                                        }
                                        try {
                                                dst.push_back(std::regex(kw, std::regex::icase | std::regex::nosubs | std::regex::optimize));
                                        } catch (std::regex_error &) {
                                                s_regex_error = "Invalid regex: " + kw;
                                                return false;
                                        }
                                }
                                return true;
                        };
                        if (!compile(kw_or, re_or) || !compile(kw_and, re_and) || !compile(kw_not, re_not))
                                return;
                }
                for (int i = 0; i < (int)s_chat.size(); i++) {
                        const char *text = s_chat[i].plain.c_str();
                        bool excluded = false;
                        for (size_t j = 0; j < kw_not.size(); j++) {
                                if (match_line(text, kw_not[j], s_use_regex, s_use_regex ? &re_not[j] : nullptr)) {
                                        excluded = true;
                                        break;
                                }
                        }
                        if (excluded) continue;
                        bool all_and = true;
                        for (size_t j = 0; j < kw_and.size(); j++) {
                                if (!match_line(text, kw_and[j], s_use_regex, s_use_regex ? &re_and[j] : nullptr)) {
                                        all_and = false;
                                        break;
                                }
                        }
                        if (!all_and) continue;
                        if (!kw_or.empty()) {
                                bool any_or = false;
                                for (size_t j = 0; j < kw_or.size(); j++) {
                                        if (match_line(text, kw_or[j], s_use_regex, s_use_regex ? &re_or[j] : nullptr)) {
                                                any_or = true;
                                                break;
                                        }
                                }
                                if (!any_or) continue;
                        }
                        s_flt_indices.push_back(i);
                }
        }
        for (int idx : s_flt_indices)
                s_flt_chars += s_chat[idx].plain.size() + 3;
}

static void rebuild_totals(void) {
        s_total_chars = 0;
        for (auto &l : s_chat)
                s_total_chars += l.plain.size() + 3;
        s_flt_chars = 0;
        for (int k : s_flt_indices)
                s_flt_chars += s_chat[k].plain.size() + 3;
}

static void delete_bulk(const std::vector<bool> &sel, bool is_filter) {
        std::vector<bool> to_del(s_chat.size(), false);
        if (is_filter) {
                for (int j = 0; j < (int)sel.size(); j++)
                        if (sel[j] && j < (int)s_flt_indices.size())
                                to_del[s_flt_indices[j]] = true;
        } else {
                for (int j = 0; j < (int)sel.size() && j < (int)s_chat.size(); j++)
                        if (sel[j]) to_del[j] = true;
        }
        int write = 0;
        for (int read = 0; read < (int)s_chat.size(); read++) {
                if (!to_del[read]) {
                        if (write != read)
                                s_chat[write] = std::move(s_chat[read]);
                        write++;
                }
        }
        s_chat.resize(write);
        invalidate_height_cache();
        s_flt_indices.clear();
        for (int i = 0; i < (int)s_chat.size(); i++)
                s_flt_indices.push_back(i);
        rebuild_totals();
}

static void do_save(GLFWwindow *w, const std::string &text, const char *default_name) {
        if (text.empty()) {
                platform_msgbox(w, "Empty", "Nothing to save - parse a log first.", PLATFORM_MSG_INFO);
                return;
        }
        char fname[PARSER_PATH_MAX];
        if (!platform_save_file_dialog(w, "Save As", default_name, "Text Files", "txt", fname, sizeof(fname)))
                return;
        FILE *f = fopen(fname, "wb");
        if (f) {
                fwrite(text.c_str(), 1, text.size(), f);
                fclose(f);
                platform_msgbox(w, "Saved", "Saved.", PLATFORM_MSG_INFO);
        } else {
                platform_msgbox(w, "Error", "Failed to save file.", PLATFORM_MSG_ERROR);
        }
}

static void do_copy(GLFWwindow *w, const std::string &text) {
        if (!text.empty())
                glfwSetClipboardString(w, text.c_str());
}

static void do_browse(GLFWwindow *w) {
        char fname[PARSER_PATH_MAX];
        if (platform_open_file_dialog(w, "Select Session", "Log Files", "log", fname, sizeof(fname)))
                snprintf(s_path, sizeof(s_path), "%s", fname);
}

static void do_parse_log(GLFWwindow *w) {
        if (!s_path[0]) {
                platform_msgbox(w, "Error", "Please select a log file first.", PLATFORM_MSG_WARN);
                return;
        }
        FILE *f = fopen(s_path, "r");
        if (!f) {
                platform_msgbox(w, "Error", "Failed to open log file.", PLATFORM_MSG_ERROR);
                return;
        }
        if (s_live_mode) {
                scanner_stop();
                s_live_mode = false;
        }
        s_chat.clear();
        s_total_chars = 0;
        s_bulk_select_mode = false;
        s_bulk_sel.clear();
        s_find_result = -1;
        s_flt_indices.clear();
        s_flt_chars = 0;
        char line[8192];
        while (fgets(line, sizeof(line), f)) {
                char *tag = strstr(line, "[fivem-parser] ");
                if (!tag) continue;
                tag += 15;
                size_t len = strlen(tag);
                while (len > 0 && (tag[len - 1] == '\n' || tag[len - 1] == '\r'))
                        tag[--len] = '\0';
                if (!len) continue;
                ChatLine cl;
                cl.timestamp = "";
                if (line[0] == '[') {
                        char *end = strchr(line, ']');
                        if (end && end < tag) {
                                char numstr[64] = "";
                                int nlen = (int)(end - line) - 1;
                                if (nlen > 0 && nlen < (int)sizeof(numstr)) {
                                        memcpy(numstr, line + 1, (size_t)nlen);
                                        numstr[nlen] = '\0';
                                        long ms = atol(numstr);
                                        if (ms >= 0) {
                                                long total_sec = ms / 1000;
                                                int h = (int)(total_sec / 3600);
                                                int m = (int)((total_sec % 3600) / 60);
                                                int s = (int)(total_sec % 60);
                                                char ts[16];
                                                snprintf(ts, sizeof(ts), "[%02d:%02d:%02d]", h, m, s);
                                                cl.timestamp = ts;
                                        }
                                }
                        }
                }
                cl.raw = tag;
                char plain_buf[8192];
                snprintf(plain_buf, sizeof(plain_buf), "%s", tag);
                strip_color_codes(plain_buf);
                cl.plain = plain_buf;
                cl.segments = parse_segments(tag);
                s_total_chars += cl.plain.size() + 3;
                s_chat.push_back(std::move(cl));
        }
        fclose(f);
        invalidate_height_cache();
        if (s_chat.empty())
                platform_msgbox(w, "Empty", "No [fivem-parser] messages found in log.", PLATFORM_MSG_INFO);
}

static void do_export_png(GLFWwindow *w, const std::vector<ChatLine> &lines) {
        if (lines.empty()) {
                platform_msgbox(w, "Empty", "Nothing to export - parse a log first.", PLATFORM_MSG_INFO);
                return;
        }
        char fname[PARSER_PATH_MAX];
        if (!platform_save_file_dialog(w, "Export PNG", "chat_log.png", "PNG Image", "png", fname, sizeof(fname)))
                return;
        if (export_chat_png(fname, lines, g_config.wrap_width, g_config.png_scale, s_png_bg[0], s_png_bg[1], s_png_bg[2], s_png_bg[3])) {
                platform_msgbox(w, "Exported", "PNG exported successfully.", PLATFORM_MSG_INFO);
        } else {
                platform_msgbox(w, "Error", "Failed to export PNG.\n\nArial font was not found on this system.", PLATFORM_MSG_ERROR);
        }
}

void ui_init(GLFWwindow *w) {
        (void)w;
        s_show_ts = (g_config.show_timestamps != 0);
        s_png_bg[0] = g_config.png_bg_r / 255.0f;
        s_png_bg[1] = g_config.png_bg_g / 255.0f;
        s_png_bg[2] = g_config.png_bg_b / 255.0f;
        s_png_bg[3] = g_config.png_bg_a / 255.0f;
}

bool ui_get_show_timestamps(void) {
        return s_show_ts;
}

void ui_shutdown(void) {
        if (scanner_is_running())
                scanner_stop();
}

void ui_render(GLFWwindow *w) {
        if (s_live_mode && scanner_is_running()) {
                ScannedMsg msgs[1];
                int n = scanner_poll(msgs, 1);
                if (n > 0) {
                        int h, m, s;
                        platform_local_time_hms(&h, &m, &s);
                        char ts[16];
                        snprintf(ts, sizeof(ts), "[%02d:%02d:%02d]", h, m, s);
                        for (int i = 0; i < n; i++) {
                                ChatLine cl;
                                cl.timestamp = ts;
                                cl.raw = msgs[i].raw;
                                cl.plain = msgs[i].plain;
                                cl.segments = parse_segments(msgs[i].raw);
                                s_total_chars += strlen(msgs[i].plain) + 3;
                                s_chat.push_back(std::move(cl));
                                s_live_scroll = true;
                        }
                        invalidate_height_cache();
                }
        }
        ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::Begin("##main", nullptr, wf);
        if (ImGui::BeginMenuBar()) {
                if (ImGui::MenuItem("Filter Chat Log")) {
                        s_show_filter = true;
                        s_kw_buf[0] = '\0';
                        s_use_regex = false;
                        s_regex_error.clear();
                        s_flt_indices.clear();
                        for (int i = 0; i < (int)s_chat.size(); i++)
                                s_flt_indices.push_back(i);
                        s_flt_chars = s_total_chars;
                }
                if (ImGui::MenuItem("About"))
                        s_open_about = true;
                ImGui::EndMenuBar();
        }
        ImGui::Text("Log File:");
        ImGui::SameLine();
        {
                float avail = ImGui::GetContentRegionAvail().x;
                float browse_w = 75, parse_w = 60;
                float path_offset = browse_w + parse_w + ImGui::GetStyle().ItemSpacing.x * 2;
                ImGui::SetNextItemWidth(avail - path_offset);
                ImGui::InputText("##path", s_path, sizeof(s_path));
                ImGui::SameLine();
                if (ImGui::Button("Browse", ImVec2(browse_w, 0)))
                        do_browse(w);
                ImGui::SameLine();
                if (ImGui::Button("Parse", ImVec2(parse_w, 0)))
                        do_parse_log(w);
        }
        {
                ImGuiIO &io = ImGui::GetIO();
                if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
                        s_show_find = !s_show_find;
                        if (s_show_find) s_find_just_opened = true;
                        else s_find_result = -1;
                }
        }
        if (s_show_find) {
                ImGui::Text("Find:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(250);
                if (s_find_just_opened) { ImGui::SetKeyboardFocusHere(); s_find_just_opened = false; }
                bool enter = ImGui::InputText("##findbar", s_find_buf, sizeof(s_find_buf), ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                if ((ImGui::Button("Find##findbtn") || enter) && s_find_buf[0] && !s_chat.empty()) {
                        int n = (int)s_chat.size();
                        int start = (s_find_result >= 0) ? (s_find_result + 1) % n : 0;
                        s_find_result = -1;
                        for (int i = 0; i < n; i++) {
                                int idx = (start + i) % n;
                                if (stristr(s_chat[idx].plain.c_str(), s_find_buf)) {
                                        s_find_result = idx;
                                        s_find_need_scroll = true;
                                        break;
                                }
                        }
                }
                ImGui::SameLine();
                if (ImGui::Button("X##closefind")) {
                        s_show_find = false;
                        s_find_result = -1;
                }
                if (s_find_result < 0 && s_find_buf[0]) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(no match)");
                }
        }
        float footer = ImGui::GetFrameHeightWithSpacing() * 2 + 8;
        ImGui::BeginChild("##output", ImVec2(0, -footer), ImGuiChildFlags_Borders);
        if (!s_chat.empty()) {
                float avail_w = ImGui::GetContentRegionAvail().x;
                ImVec2 win_pos = ImGui::GetWindowPos();
                float win_h = ImGui::GetWindowHeight();
                const float cull_margin = 400.0f;
                if (avail_w != s_cache_avail_w || g_config.wrap_width != s_cache_wrap) {
                        s_line_heights.clear();
                        s_cache_avail_w = avail_w;
                        s_cache_wrap = g_config.wrap_width;
                }
                if (s_line_heights.size() != s_chat.size())
                        s_line_heights.resize(s_chat.size(), 0.0f);
                for (int i = 0; i < (int)s_chat.size(); i++) {
                        ImVec2 cursor = ImGui::GetCursorScreenPos();
                        float cached_h = s_line_heights[i];
                        bool force = (i == s_find_result) || s_bulk_select_mode;
                        bool off_screen = !force && cached_h > 0.0f && ((cursor.y > win_pos.y + win_h + cull_margin) || (cursor.y + cached_h < win_pos.y - cull_margin));
                        if (off_screen) {
                                ImGui::Dummy(ImVec2(avail_w, cached_h));
                                continue;
                        }
                        ImGui::PushID(i);
                        ImVec2 row_top = ImGui::GetCursorScreenPos();
                        if (s_bulk_select_mode && i < (int)s_bulk_sel.size()) {
                                bool chk = s_bulk_sel[i];
                                ImGui::Checkbox("##chk", &chk);
                                s_bulk_sel[i] = chk;
                                ImGui::SameLine(0, 4);
                        }
                        ImGui::BeginGroup();
                        float h = render_chat_line(s_chat[i]);
                        ImGui::EndGroup();
                        s_line_heights[i] = h;
                        if (i == s_find_result) {
                                ImVec2 row_bot = ImGui::GetCursorScreenPos();
                                ImVec2 wpos = ImGui::GetWindowPos();
                                float rw = ImGui::GetWindowWidth();
                                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(wpos.x, row_top.y), ImVec2(wpos.x + rw, row_bot.y), IM_COL32(255, 220, 0, 50));
                                if (s_find_need_scroll) {
                                        ImGui::SetScrollHereY(0.5f);
                                        s_find_need_scroll = false;
                                }
                        }
                        if (!s_bulk_select_mode && ImGui::BeginPopupContextItem("##ctx", ImGuiPopupFlags_MouseButtonRight)) {
                                if (ImGui::MenuItem("Edit Line")) {
                                        s_edit_line = i;
                                        strncpy(s_edit_buf, s_chat[i].raw.c_str(), sizeof(s_edit_buf) - 1);
                                        s_edit_buf[sizeof(s_edit_buf) - 1] = '\0';
                                        s_open_edit_popup = true;
                                }
                                if (ImGui::MenuItem("Select Line(s)")) {
                                        s_bulk_select_mode = true;
                                        s_bulk_sel.assign(s_chat.size(), false);
                                        s_bulk_sel[i] = true;
                                }
                                ImGui::EndPopup();
                        }
                        ImGui::PopID();
                }
        }
        if (s_live_scroll) {
                ImGui::SetScrollHereY(1.0f);
                s_live_scroll = false;
        }
        ImGui::EndChild();
        if (s_open_edit_popup) {
                ImGui::OpenPopup("Edit Line");
                s_open_edit_popup = false;
        }
        if (ImGui::BeginPopupModal("Edit Line", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::SetNextItemWidth(500);
                ImGui::InputText("##ei", s_edit_buf, sizeof(s_edit_buf));
                ImGui::Spacing();
                if (ImGui::Button("Apply", ImVec2(100, 0))) {
                        if (s_edit_line >= 0 && s_edit_line < (int)s_chat.size()) {
                                ChatLine &cl = s_chat[s_edit_line];
                                cl.raw = s_edit_buf;
                                cl.segments = parse_segments(s_edit_buf);
                                cl.plain.clear();
                                for (auto &seg : cl.segments) cl.plain += seg.text;
                                invalidate_height_cache();
                                rebuild_totals();
                        }
                        s_edit_line = -1;
                        ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(100, 0))) {
                        s_edit_line = -1;
                        ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
        }
        if (s_bulk_select_mode) {
                int sel_count = 0;
                for (bool b : s_bulk_sel) if (b) sel_count++;
                ImGui::Text("%d line(s) selected", sel_count);
                char del_label[64];
                snprintf(del_label, sizeof(del_label), "Delete (%d)###bsel_del", sel_count);
                if (ImGui::Button(del_label, ImVec2(110, 0))) {
                        delete_bulk(s_bulk_sel, false);
                        s_bulk_select_mode = false;
                        s_bulk_sel.clear();
                }
                ImGui::SameLine();
                if (ImGui::Button("Export PNG###bsel_exp", ImVec2(95, 0))) {
                        std::vector<ChatLine> sel_lines;
                        for (int j = 0; j < (int)s_bulk_sel.size(); j++)
                                if (s_bulk_sel[j] && j < (int)s_chat.size())
                                        sel_lines.push_back(s_chat[j]);
                        do_export_png(w, sel_lines);
                        s_bulk_select_mode = false;
                        s_bulk_sel.clear();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel##bsel", ImVec2(70, 0))) {
                        s_bulk_select_mode = false;
                        s_bulk_sel.clear();
                }
                ImGui::SameLine();
                if (ImGui::Button("All##bsel", ImVec2(50, 0)))
                        for (auto &&b : s_bulk_sel) b = true;
                ImGui::SameLine();
                if (ImGui::Button("None##bsel", ImVec2(55, 0)))
                        for (auto &&b : s_bulk_sel) b = false;
        } else {
                if (s_live_mode)
                        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "LIVE  %u messages", (unsigned)s_chat.size());
                else
                        ImGui::Text("%u characters  |  %u messages", (unsigned)s_total_chars, (unsigned)s_chat.size());
                ImGui::Checkbox("Timestamps", &s_show_ts);
                ImGui::SameLine();
                ImGui::Text("Wrap:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(55);
                if (ImGui::InputInt("##wrap", &g_config.wrap_width, 0, 0)) {
                        if (g_config.wrap_width < 100) g_config.wrap_width = 100;
                        if (g_config.wrap_width > 2000) g_config.wrap_width = 2000;
                        invalidate_height_cache();
                }
                ImGui::SameLine();
                float bw = 72, bw_exp = 92, bg_btn = 20;
                float sp = ImGui::GetStyle().ItemSpacing.x;
                float total = bw * 3 + bw_exp + bg_btn + sp * 4;
                ImGui::SameLine(ImGui::GetWindowWidth() - total - ImGui::GetStyle().WindowPadding.x);
                if (s_live_mode) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.2f, 0.2f, 1.0f));
                        if (ImGui::Button("STOP", ImVec2(bw, 0))) {
                                scanner_stop();
                                ScannedMsg drain[64];
                                int dn;
                                while ((dn = scanner_poll(drain, 64)) > 0) {
                                        int h, m, s;
                                        platform_local_time_hms(&h, &m, &s);
                                        char ts[16];
                                        snprintf(ts, sizeof(ts), "[%02d:%02d:%02d]", h, m, s);
                                        for (int di = 0; di < dn; di++) {
                                                ChatLine cl;
                                                cl.timestamp = ts;
                                                cl.raw = drain[di].raw;
                                                cl.plain = drain[di].plain;
                                                cl.segments = parse_segments(drain[di].raw);
                                                s_total_chars += strlen(drain[di].plain) + 3;
                                                s_chat.push_back(std::move(cl));
                                        }
                                }
                                invalidate_height_cache();
                                s_live_mode = false;
                        }
                        ImGui::PopStyleColor(2);
                } else {
                        if (ImGui::Button("LIVE", ImVec2(bw, 0))) {
                                s_chat.clear();
                                s_total_chars = 0;
                                s_bulk_select_mode = false;
                                s_bulk_sel.clear();
                                s_find_result = -1;
                                s_flt_indices.clear();
                                s_flt_chars = 0;
                                invalidate_height_cache();
                                if (scanner_start())
                                        s_live_mode = true;
                                else
                                        platform_msgbox(w, "Error", "Failed to start scanner.", PLATFORM_MSG_ERROR);
                        }
                }
                ImGui::SameLine();
                if (ImGui::Button("SAVE AS", ImVec2(bw, 0))) {
                        std::string plain = build_plain(s_chat);
                        do_save(w, plain, "chat_log.txt");
                }
                ImGui::SameLine();
                if (ImGui::Button("COPY", ImVec2(bw, 0))) {
                        std::string plain = build_plain(s_chat);
                        do_copy(w, plain);
                }
                ImGui::SameLine();
                {
                        ImVec4 bg_col(s_png_bg[0], s_png_bg[1], s_png_bg[2], s_png_bg[3]);
                        if (ImGui::ColorButton("##pngbg", bg_col, ImGuiColorEditFlags_AlphaPreview | ImGuiColorEditFlags_NoTooltip, ImVec2(bg_btn, ImGui::GetFrameHeight()))) ImGui::OpenPopup("##pngbg_popup");
                        if (ImGui::BeginPopup("##pngbg_popup")) {
                                ImGui::Text("PNG Background");
                                if (ImGui::ColorPicker4("##bgpicker", s_png_bg, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreview)) {
                                        g_config.png_bg_r = (int)(s_png_bg[0] * 255.0f + 0.5f);
                                        g_config.png_bg_g = (int)(s_png_bg[1] * 255.0f + 0.5f);
                                        g_config.png_bg_b = (int)(s_png_bg[2] * 255.0f + 0.5f);
                                        g_config.png_bg_a = (int)(s_png_bg[3] * 255.0f + 0.5f);
                                }
                                ImGui::Separator();
                                ImGui::Text("PNG Scale (sharpness)");
                                ImGui::SliderInt("##pngscale", &g_config.png_scale, 1, 3, "%dx");
                                ImGui::EndPopup();
                        }
                }
                ImGui::SameLine();
                if (ImGui::Button("EXPORT PNG", ImVec2(bw_exp, 0)))
                        do_export_png(w, s_chat);
        }
        if (s_open_about) {
                ImGui::OpenPopup("About");
                s_open_about = false;
        }
        if (ImGui::BeginPopupModal("About", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("fivem-parser");
                ImGui::Text("Version 1.1.5");
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::Text("https://github.com/dmitripetrovich");
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                if (ImGui::Button("OK", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
                        ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
        }
        ImGui::End();
        if (s_show_filter) {
                ImGui::SetNextWindowSize(ImVec2(640, 490), ImGuiCond_FirstUseEver);
                if (ImGui::Begin("Filter Chat Log", &s_show_filter)) {
                        ImGui::Text("Keywords (one per line): +must have -exclude or plain OR");
                        ImGui::InputTextMultiline("##flt_kw", s_kw_buf, sizeof(s_kw_buf), ImVec2(-1, 80));
                        if (ImGui::Button("FILTER", ImVec2(90, 0)))
                                apply_filter();
                        ImGui::SameLine();
                        if (ImGui::Button("CLEAR", ImVec2(90, 0))) {
                                s_kw_buf[0] = '\0';
                                s_regex_error.clear();
                                s_flt_indices.clear();
                                for (int i = 0; i < (int)s_chat.size(); i++)
                                        s_flt_indices.push_back(i);
                                s_flt_chars = s_total_chars;
                        }
                        ImGui::SameLine();
                        ImGui::Checkbox("Regex", &s_use_regex);
                        if (!s_regex_error.empty()) {
                                ImGui::SameLine();
                                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", s_regex_error.c_str());
                        }
                        float flt_footer = ImGui::GetFrameHeightWithSpacing() + 8;
                        ImGui::BeginChild("##flt_out", ImVec2(0, -flt_footer), ImGuiChildFlags_Borders);
                        if (!s_flt_indices.empty()) {
                                for (int i = 0; i < (int)s_flt_indices.size(); i++) {
                                        int chat_idx = s_flt_indices[i];
                                        ImGui::PushID(i);
                                        if (s_flt_bulk_select_mode && i < (int)s_flt_bulk_sel.size()) {
                                                bool chk = s_flt_bulk_sel[i];
                                                ImGui::Checkbox("##fchk", &chk);
                                                s_flt_bulk_sel[i] = chk;
                                                ImGui::SameLine(0, 4);
                                        }
                                        ImGui::BeginGroup();
                                        render_chat_line(s_chat[chat_idx]);
                                        ImGui::EndGroup();
                                        if (!s_flt_bulk_select_mode && ImGui::BeginPopupContextItem("##fctx", ImGuiPopupFlags_MouseButtonRight)) {
                                                if (ImGui::MenuItem("Edit Line")) {
                                                        s_edit_line = chat_idx;
                                                        strncpy(s_edit_buf, s_chat[chat_idx].raw.c_str(), sizeof(s_edit_buf) - 1);
                                                        s_edit_buf[sizeof(s_edit_buf) - 1] = '\0';
                                                        s_open_edit_popup = true;
                                                }
                                                if (ImGui::MenuItem("Select Line(s)")) {
                                                        s_flt_bulk_select_mode = true;
                                                        s_flt_bulk_sel.assign(s_flt_indices.size(), false);
                                                        s_flt_bulk_sel[i] = true;
                                                }
                                                ImGui::EndPopup();
                                        }
                                        ImGui::PopID();
                                }
                        }
                        ImGui::EndChild();
                        if (s_flt_bulk_select_mode) {
                                int flt_sel_count = 0;
                                for (bool b : s_flt_bulk_sel) if (b) flt_sel_count++;
                                ImGui::Text("%d selected", flt_sel_count);
                                ImGui::SameLine();
                                char flt_del_label[64];
                                snprintf(flt_del_label, sizeof(flt_del_label), "Delete (%d)###fsel_del", flt_sel_count);
                                if (ImGui::Button(flt_del_label, ImVec2(110, 0))) {
                                        delete_bulk(s_flt_bulk_sel, true);
                                        s_flt_bulk_select_mode = false;
                                        s_flt_bulk_sel.clear();
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Export PNG###fsel_exp", ImVec2(95, 0))) {
                                        std::vector<ChatLine> sel_lines;
                                        for (int j = 0; j < (int)s_flt_bulk_sel.size(); j++)
                                                if (s_flt_bulk_sel[j] && j < (int)s_flt_indices.size())
                                                        sel_lines.push_back(s_chat[s_flt_indices[j]]);
                                        do_export_png(w, sel_lines);
                                        s_flt_bulk_select_mode = false;
                                        s_flt_bulk_sel.clear();
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Cancel##fsel", ImVec2(70, 0))) {
                                        s_flt_bulk_select_mode = false;
                                        s_flt_bulk_sel.clear();
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("All##fsel", ImVec2(50, 0)))
                                        for (auto &&b : s_flt_bulk_sel) b = true;
                                ImGui::SameLine();
                                if (ImGui::Button("None##fsel", ImVec2(55, 0)))
                                        for (auto &&b : s_flt_bulk_sel) b = false;
                        } else {
                                ImGui::Text("%u characters and %u lines", (unsigned)s_flt_chars, (unsigned)s_flt_indices.size());
                                ImGui::SameLine();
                                float fw = 90;
                                float fsp = ImGui::GetStyle().ItemSpacing.x;
                                ImGui::SameLine(ImGui::GetWindowWidth() - fw * 3 - fsp * 2 - ImGui::GetStyle().WindowPadding.x);
                                if (ImGui::Button("SAVE AS##flt", ImVec2(fw, 0))) {
                                        std::string plain = build_plain_indices(s_flt_indices);
                                        do_save(w, plain, "filtered_chat.txt");
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("COPY##flt", ImVec2(fw, 0))) {
                                        std::string plain = build_plain_indices(s_flt_indices);
                                        do_copy(w, plain);
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("EXPORT PNG##flt", ImVec2(fw, 0))) {
                                        std::vector<ChatLine> flt_lines;
                                        flt_lines.reserve(s_flt_indices.size());
                                        for (int idx : s_flt_indices)
                                                flt_lines.push_back(s_chat[idx]);
                                        do_export_png(w, flt_lines);
                                }
                        }
                }
                ImGui::End();
        }
}
