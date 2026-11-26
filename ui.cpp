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

#ifndef _WIN32
#define strtok_s strtok_r
#endif

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
        auto flush = [&]() {
                if (cur.empty()) return;
                segs.push_back({std::move(cur), color});
                cur.clear();
                cur.reserve(128);
        };
        for (int i = 0; raw[i];) {
                ColorCode kind;
                unsigned value;
                int n = color_code_at(raw + i, &kind, &value);
                if (!n) {
                        cur += raw[i++];
                        continue;
                }
                if (kind == CODE_RGB) {
                        flush();
                        color = ImVec4(((value >> 16) & 0xFF) / 255.0f, ((value >> 8) & 0xFF) / 255.0f, (value & 0xFF) / 255.0f, 1.0f);
                } else if (kind == CODE_PALETTE) {
                        flush();
                        color = color_palette((int)value);
                } else if (kind == CODE_RESET) {
                        flush();
                        color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
                }
                i += n;
        }
        flush();
        return segs;
}

struct ChatView {
        bool filtered;
        bool select_mode = false;
        std::vector<bool> selected;
        std::vector<float> heights;
        float cache_avail_w = 0.0f;
        int cache_wrap = 0;

        explicit ChatView(bool is_filtered) : filtered(is_filtered) {}
};

enum Action {
        ACT_NONE,
        ACT_LIVE,
        ACT_PARSE,
};

static std::vector<ChatLine> s_chat;
static size_t s_total_chars = 0;
static bool s_dirty = false;

static ChatView s_main_view(false);
static ChatView s_flt_view(true);

static bool s_show_filter = false;
static char s_kw_buf[4096] = "";
static bool s_use_regex = false;
static std::vector<int> s_flt_indices;
static size_t s_flt_chars = 0;
static std::string s_regex_error;

static bool s_open_about = false;

static int s_edit_line = -1;
static bool s_open_edit_popup = false;
static char s_edit_buf[8192] = "";

static Action s_confirm_action = ACT_NONE;
static bool s_open_confirm = false;

static float s_png_bg[4] = {0.0f, 0.0f, 0.0f, 0.0f};

static bool s_show_find = false;
static bool s_find_just_opened = false;
static char s_find_buf[256] = "";
static int s_find_result = -1;
static bool s_find_need_scroll = false;

static bool s_show_ts = true;
static bool s_live_mode = false;
static bool s_live_scroll = false;
static char s_path[PARSER_PATH_MAX] = "";

static int view_rows(const ChatView &v) {
        return (int)(v.filtered ? s_flt_indices.size() : s_chat.size());
}

static int view_line(const ChatView &v, int row) {
        return v.filtered ? s_flt_indices[row] : row;
}

static void clear_selection(ChatView &v) {
        v.select_mode = false;
        v.selected.clear();
}

static void invalidate_height_cache(void) {
        s_main_view.heights.clear();
        s_flt_view.heights.clear();
}

static std::string format_hms(int h, int m, int s) {
        char ts[32];
        snprintf(ts, sizeof(ts), "[%02d:%02d:%02d]", h, m, s);
        return ts;
}

static void set_line_text(ChatLine &line, const char *raw) {
        line.raw = raw;
        line.segments = parse_segments(raw);
        line.plain.clear();
        for (const ColorSeg &seg : line.segments)
                line.plain += seg.text;
}

static void add_line(const std::string &timestamp, const char *raw) {
        ChatLine line;
        line.timestamp = timestamp;
        set_line_text(line, raw);
        s_total_chars += line.plain.size() + 3;
        s_chat.push_back(std::move(line));
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
        if (s_show_ts && !line.timestamp.empty()) {
                const char *ts = line.timestamp.c_str();
                int ts_len = (int)line.timestamp.size();
                const float ts_w = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, ts, ts + ts_len).x;
                const float sp_w = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, " ", nullptr).x;
                if (dl)
                        dl->AddText(font, font_size, ImVec2(pen_x, pen_y), IM_COL32(140, 140, 140, 255), ts, ts + ts_len);
                pen_x += ts_w + sp_w;
        }
        for (const ColorSeg &seg : line.segments) {
                const ImU32 col = ImGui::ColorConvertFloat4ToU32(seg.color);
                const char *text = seg.text.c_str();
                for_each_word(text, text + seg.text.size(), [&](const char *word, const char *word_end) {
                        const float word_w = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, word, word_end).x;
                        if (pen_x > start_x && pen_x + word_w > max_x) {
                                pen_x = start_x;
                                pen_y += line_h;
                                total_h += line_h;
                        }
                        if (dl)
                                dl->AddText(font, font_size, ImVec2(pen_x, pen_y), col, word, word_end);
                        pen_x += word_w;
                });
        }
        ImGui::Dummy(ImVec2(avail_w, total_h));
        return total_h;
}

static std::string build_plain(const ChatView &v) {
        std::string out;
        out.reserve(v.filtered ? s_flt_chars : s_total_chars);
        for (int row = 0, rows = view_rows(v); row < rows; row++) {
                const ChatLine &l = s_chat[view_line(v, row)];
                if (s_show_ts && !l.timestamp.empty()) { out += l.timestamp; out += ' '; }
                out += l.plain;
                out += "\r\n";
        }
        return out;
}

static std::vector<const ChatLine *> collect_lines(const ChatView &v, bool selected_only) {
        std::vector<const ChatLine *> lines;
        for (int row = 0, rows = view_rows(v); row < rows; row++)
                if (!selected_only || (row < (int)v.selected.size() && v.selected[row]))
                        lines.push_back(&s_chat[view_line(v, row)]);
        return lines;
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

static void filter_show_all(void) {
        s_flt_indices.resize(s_chat.size());
        for (int i = 0; i < (int)s_chat.size(); i++)
                s_flt_indices[i] = i;
        s_flt_chars = s_total_chars;
        clear_selection(s_flt_view);
        s_flt_view.heights.clear();
}

static void apply_filter(void) {
        s_flt_indices.clear();
        s_flt_chars = 0;
        s_regex_error.clear();
        clear_selection(s_flt_view);
        s_flt_view.heights.clear();
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
                filter_show_all();
                return;
        }
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
        auto matches_any = [&](const char *text, const std::vector<std::string> &kws, const std::vector<std::regex> &res) {
                for (size_t j = 0; j < kws.size(); j++)
                        if (match_line(text, kws[j], s_use_regex, s_use_regex ? &res[j] : nullptr))
                                return true;
                return false;
        };
        auto matches_all = [&](const char *text, const std::vector<std::string> &kws, const std::vector<std::regex> &res) {
                for (size_t j = 0; j < kws.size(); j++)
                        if (!match_line(text, kws[j], s_use_regex, s_use_regex ? &res[j] : nullptr))
                                return false;
                return true;
        };
        for (int i = 0; i < (int)s_chat.size(); i++) {
                const char *text = s_chat[i].plain.c_str();
                if (matches_any(text, kw_not, re_not)) continue;
                if (!matches_all(text, kw_and, re_and)) continue;
                if (!kw_or.empty() && !matches_any(text, kw_or, re_or)) continue;
                s_flt_indices.push_back(i);
                s_flt_chars += s_chat[i].plain.size() + 3;
        }
}

static void rebuild_totals(void) {
        s_total_chars = 0;
        for (auto &l : s_chat)
                s_total_chars += l.plain.size() + 3;
        s_flt_chars = 0;
        for (int k : s_flt_indices)
                s_flt_chars += s_chat[k].plain.size() + 3;
}

static void reset_chat(void) {
        s_chat.clear();
        s_total_chars = 0;
        s_dirty = false;
        s_find_result = -1;
        s_flt_indices.clear();
        s_flt_chars = 0;
        clear_selection(s_main_view);
        clear_selection(s_flt_view);
        invalidate_height_cache();
}

static void delete_selected(const ChatView &v) {
        std::vector<bool> to_del(s_chat.size(), false);
        for (int row = 0, rows = view_rows(v); row < rows && row < (int)v.selected.size(); row++)
                if (v.selected[row]) to_del[view_line(v, row)] = true;
        int write = 0;
        for (int read = 0; read < (int)s_chat.size(); read++) {
                if (!to_del[read]) {
                        if (write != read)
                                s_chat[write] = std::move(s_chat[read]);
                        write++;
                }
        }
        s_chat.resize(write);
        s_dirty = true;
        s_find_result = -1;
        s_flt_indices.clear();
        clear_selection(s_main_view);
        invalidate_height_cache();
        rebuild_totals();
        apply_filter();
}

static bool do_save(GLFWwindow *w, const std::string &text, const char *default_name) {
        if (text.empty()) {
                platform_msgbox(w, "Empty", "Nothing to save - parse a log first.", PLATFORM_MSG_INFO);
                return false;
        }
        char fname[PARSER_PATH_MAX];
        if (!platform_save_file_dialog(w, "Save As", default_name, "Text Files", "txt", fname, sizeof(fname)))
                return false;
        FILE *f = fopen(fname, "wb");
        bool ok = f && fwrite(text.c_str(), 1, text.size(), f) == text.size();
        if (f && fclose(f) != 0) ok = false;
        if (ok)
                platform_msgbox(w, "Saved", "Saved.", PLATFORM_MSG_INFO);
        else
                platform_msgbox(w, "Error", "Failed to save file.", PLATFORM_MSG_ERROR);
        return ok;
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
        reset_chat();
        char line[8192];
        while (fgets(line, sizeof(line), f)) {
                char *tag = strstr(line, "[fivem-parser] ");
                if (!tag) continue;
                tag += 15;
                size_t len = strlen(tag);
                while (len > 0 && (tag[len - 1] == '\n' || tag[len - 1] == '\r'))
                        tag[--len] = '\0';
                if (!len) continue;
                std::string timestamp;
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
                                                timestamp = format_hms((int)(total_sec / 3600), (int)((total_sec % 3600) / 60), (int)(total_sec % 60));
                                        }
                                }
                        }
                }
                add_line(timestamp, tag);
        }
        fclose(f);
        if (s_chat.empty())
                platform_msgbox(w, "Empty", "No [fivem-parser] messages found in log.", PLATFORM_MSG_INFO);
}

static void start_live(GLFWwindow *w) {
        reset_chat();
        if (scanner_start())
                s_live_mode = true;
        else
                platform_msgbox(w, "Error", "Failed to start scanner.", PLATFORM_MSG_ERROR);
}

static void run_action(GLFWwindow *w, Action action) {
        if (action == ACT_LIVE)
                start_live(w);
        else if (action == ACT_PARSE)
                do_parse_log(w);
}

static void request_action(GLFWwindow *w, Action action) {
        if (s_dirty && !s_chat.empty()) {
                s_confirm_action = action;
                s_open_confirm = true;
        } else {
                run_action(w, action);
        }
}

static void pull_live_messages(void) {
        static ScannedMsg msgs[32];
        int n;
        while ((n = scanner_poll(msgs, 32)) > 0) {
                int h, m, s;
                platform_local_time_hms(&h, &m, &s);
                std::string ts = format_hms(h, m, s);
                for (int i = 0; i < n; i++)
                        add_line(ts, msgs[i].raw);
                s_live_scroll = true;
                s_dirty = true;
        }
}

static bool do_export_png(GLFWwindow *w, const std::vector<const ChatLine *> &lines) {
        if (lines.empty()) {
                platform_msgbox(w, "Empty", "Nothing to export - parse a log first.", PLATFORM_MSG_INFO);
                return false;
        }
        char fname[PARSER_PATH_MAX];
        if (!platform_save_file_dialog(w, "Export PNG", "chat_log.png", "PNG Image", "png", fname, sizeof(fname)))
                return false;
        switch (export_chat_png(fname, lines, g_config.wrap_width, g_config.png_scale, s_png_bg[0], s_png_bg[1], s_png_bg[2], s_png_bg[3])) {
                case PNG_OK:
                        platform_msgbox(w, "Exported", "PNG exported successfully.", PLATFORM_MSG_INFO);
                        return true;
                case PNG_NO_FONT:
                        platform_msgbox(w, "Error", "Failed to export PNG.\n\nArial font was not found on this system.", PLATFORM_MSG_ERROR);
                        return false;
                case PNG_TOO_LARGE:
                        platform_msgbox(w, "Error", "Failed to export PNG.\n\nToo many lines for one image. Select fewer lines and export them in parts.", PLATFORM_MSG_ERROR);
                        return false;
                default:
                        platform_msgbox(w, "Error", "Failed to export PNG.\n\nThe file could not be written.", PLATFORM_MSG_ERROR);
                        return false;
        }
}

static void draw_chat_rows(ChatView &v) {
        const int rows = view_rows(v);
        if (rows == 0)
                return;
        float avail_w = ImGui::GetContentRegionAvail().x;
        ImVec2 win_pos = ImGui::GetWindowPos();
        float win_h = ImGui::GetWindowHeight();
        const float cull_margin = 400.0f;
        if (avail_w != v.cache_avail_w || g_config.wrap_width != v.cache_wrap) {
                v.heights.clear();
                v.cache_avail_w = avail_w;
                v.cache_wrap = g_config.wrap_width;
        }
        if ((int)v.heights.size() != rows)
                v.heights.resize(rows, 0.0f);
        for (int row = 0; row < rows; row++) {
                const int idx = view_line(v, row);
                const bool is_found = !v.filtered && idx == s_find_result;
                ImVec2 cursor = ImGui::GetCursorScreenPos();
                float cached_h = v.heights[row];
                bool force = is_found || v.select_mode;
                bool off_screen = !force && cached_h > 0.0f && ((cursor.y > win_pos.y + win_h + cull_margin) || (cursor.y + cached_h < win_pos.y - cull_margin));
                if (off_screen) {
                        ImGui::Dummy(ImVec2(avail_w, cached_h));
                        continue;
                }
                ImGui::PushID(row);
                ImVec2 row_top = ImGui::GetCursorScreenPos();
                if (v.select_mode && row < (int)v.selected.size()) {
                        bool chk = v.selected[row];
                        ImGui::Checkbox("##chk", &chk);
                        v.selected[row] = chk;
                        ImGui::SameLine(0, 4);
                }
                ImGui::BeginGroup();
                v.heights[row] = render_chat_line(s_chat[idx]);
                ImGui::EndGroup();
                if (is_found) {
                        ImVec2 row_bot = ImGui::GetCursorScreenPos();
                        ImVec2 wpos = ImGui::GetWindowPos();
                        float rw = ImGui::GetWindowWidth();
                        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(wpos.x, row_top.y), ImVec2(wpos.x + rw, row_bot.y), IM_COL32(255, 220, 0, 50));
                        if (s_find_need_scroll) {
                                ImGui::SetScrollHereY(0.5f);
                                s_find_need_scroll = false;
                        }
                }
                if (!v.select_mode && ImGui::BeginPopupContextItem("##ctx", ImGuiPopupFlags_MouseButtonRight)) {
                        if (ImGui::MenuItem("Edit Line")) {
                                s_edit_line = idx;
                                snprintf(s_edit_buf, sizeof(s_edit_buf), "%s", s_chat[idx].raw.c_str());
                                s_open_edit_popup = true;
                        }
                        if (ImGui::MenuItem("Select Line(s)")) {
                                v.select_mode = true;
                                v.selected.assign(rows, false);
                                v.selected[row] = true;
                        }
                        ImGui::EndPopup();
                }
                ImGui::PopID();
        }
}

static void draw_selection_bar(GLFWwindow *w, ChatView &v) {
        int count = 0;
        for (bool b : v.selected) if (b) count++;
        if (v.filtered) {
                ImGui::Text("%d selected", count);
                ImGui::SameLine();
        } else {
                ImGui::Text("%d line(s) selected", count);
        }
        bool done = false;
        char del_label[64];
        snprintf(del_label, sizeof(del_label), "Delete (%d)###sel_del", count);
        if (ImGui::Button(del_label, ImVec2(110, 0))) {
                delete_selected(v);
                done = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Export PNG###sel_exp", ImVec2(95, 0))) {
                do_export_png(w, collect_lines(v, true));
                done = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel##sel", ImVec2(70, 0)))
                done = true;
        ImGui::SameLine();
        if (ImGui::Button("All##sel", ImVec2(50, 0)))
                v.selected.assign(v.selected.size(), true);
        ImGui::SameLine();
        if (ImGui::Button("None##sel", ImVec2(55, 0)))
                v.selected.assign(v.selected.size(), false);
        if (done)
                clear_selection(v);
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
        if (s_live_mode)
                pull_live_messages();
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
                        filter_show_all();
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
                        request_action(w, ACT_PARSE);
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
        {
                const bool at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
                draw_chat_rows(s_main_view);
                if (s_live_scroll && at_bottom)
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
                                set_line_text(s_chat[s_edit_line], s_edit_buf);
                                s_dirty = true;
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
        if (s_main_view.select_mode) {
                draw_selection_bar(w, s_main_view);
        } else {
                if (s_live_mode) {
                        const char *note = "";
                        switch (scanner_status()) {
                                case SCANNER_SEARCHING: note = "  |  waiting for FiveM"; break;
                                case SCANNER_DENIED: note = "  |  cannot read FiveM's memory, try running as administrator"; break;
                                default: break;
                        }
                        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "LIVE  %u messages%s", (unsigned)s_chat.size(), note);
                } else {
                        ImGui::Text("%u characters  |  %u messages", (unsigned)s_total_chars, (unsigned)s_chat.size());
                }
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
                                pull_live_messages();
                                s_live_mode = false;
                        }
                        ImGui::PopStyleColor(2);
                } else {
                        if (ImGui::Button("LIVE", ImVec2(bw, 0)))
                                request_action(w, ACT_LIVE);
                }
                ImGui::SameLine();
                if (ImGui::Button("SAVE AS", ImVec2(bw, 0))) {
                        if (do_save(w, build_plain(s_main_view), "chat_log.txt"))
                                s_dirty = false;
                }
                ImGui::SameLine();
                if (ImGui::Button("COPY", ImVec2(bw, 0)))
                        do_copy(w, build_plain(s_main_view));
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
                if (ImGui::Button("EXPORT PNG", ImVec2(bw_exp, 0))) {
                        if (do_export_png(w, collect_lines(s_main_view, false)))
                                s_dirty = false;
                }
        }
        if (s_open_confirm) {
                ImGui::OpenPopup("Discard Chat");
                s_open_confirm = false;
        }
        if (ImGui::BeginPopupModal("Discard Chat", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("The current chat has %u unsaved messages. Discard them?", (unsigned)s_chat.size());
                ImGui::Spacing();
                if (ImGui::Button("Discard", ImVec2(100, 0))) {
                        Action action = s_confirm_action;
                        s_confirm_action = ACT_NONE;
                        ImGui::CloseCurrentPopup();
                        run_action(w, action);
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(100, 0))) {
                        s_confirm_action = ACT_NONE;
                        ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
        }
        if (s_open_about) {
                ImGui::OpenPopup("About");
                s_open_about = false;
        }
        if (ImGui::BeginPopupModal("About", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("fivem-parser");
                ImGui::Text("Version %s", PARSER_VERSION);
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
                                filter_show_all();
                        }
                        ImGui::SameLine();
                        ImGui::Checkbox("Regex", &s_use_regex);
                        if (!s_regex_error.empty()) {
                                ImGui::SameLine();
                                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", s_regex_error.c_str());
                        }
                        float flt_footer = ImGui::GetFrameHeightWithSpacing() + 8;
                        ImGui::BeginChild("##flt_out", ImVec2(0, -flt_footer), ImGuiChildFlags_Borders);
                        draw_chat_rows(s_flt_view);
                        ImGui::EndChild();
                        if (s_flt_view.select_mode) {
                                draw_selection_bar(w, s_flt_view);
                        } else {
                                ImGui::Text("%u characters and %u lines", (unsigned)s_flt_chars, (unsigned)s_flt_indices.size());
                                ImGui::SameLine();
                                float fw = 90;
                                float fsp = ImGui::GetStyle().ItemSpacing.x;
                                ImGui::SameLine(ImGui::GetWindowWidth() - fw * 3 - fsp * 2 - ImGui::GetStyle().WindowPadding.x);
                                if (ImGui::Button("SAVE AS##flt", ImVec2(fw, 0)))
                                        do_save(w, build_plain(s_flt_view), "filtered_chat.txt");
                                ImGui::SameLine();
                                if (ImGui::Button("COPY##flt", ImVec2(fw, 0)))
                                        do_copy(w, build_plain(s_flt_view));
                                ImGui::SameLine();
                                if (ImGui::Button("EXPORT PNG##flt", ImVec2(fw, 0)))
                                        do_export_png(w, collect_lines(s_flt_view, false));
                        }
                }
                ImGui::End();
        }
}
