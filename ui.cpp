#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include "imgui.h"
#include <commdlg.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cctype>
#include <string>
#include <vector>
#include <regex>
#include <algorithm>

extern "C" {
#include "resource.h"
#include "parser.h"
#include "util.h"
}

#include "chat.h"

static char s_path[MAX_PATH * 2] = "";
static bool s_remove_ts = false;

static std::vector<ChatLine> s_chat;
static size_t s_total_chars = 0;

static bool s_show_filter = false;
static char s_kw_buf[4096] = "";
static bool s_use_regex = false;
static std::vector<int> s_flt_indices;
static size_t s_flt_chars = 0;
static std::string s_regex_error;

static bool s_open_about = false;

static int s_pending_delete = -1;
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

static char s_flt_ts_start[16] = "";
static char s_flt_ts_end[16] = "";

static void render_chat_line(const ChatLine &line) {
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
        const bool should_draw = (item_pos.y < win_pos.y + win_h + line_h) && (item_pos.y > win_pos.y - line_h);
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
        if (!s_remove_ts && !line.timestamp.empty()) {
                std::string ts = line.timestamp + " ";
                process_span(ts.c_str(), IM_COL32(140, 140, 140, 255));
        }
        for (const auto &seg : line.segments)
                process_span(seg.text.c_str(), ImGui::ColorConvertFloat4ToU32(seg.color));
        ImGui::Dummy(ImVec2(avail_w, total_h));
}

static std::string build_plain(const std::vector<ChatLine> &lines) {
        std::string out;
        out.reserve(s_total_chars);
        for (auto &l : lines) {
                if (!s_remove_ts && !l.timestamp.empty()) { out += l.timestamp; out += ' '; }
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
                if (!s_remove_ts && !l.timestamp.empty()) { out += l.timestamp; out += ' '; }
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

static int parse_time_secs(const char *s) {
        if (!s || !s[0]) return -1;
        int h = 0, m = 0, sec = 0;
        if (sscanf(s, "%d:%d:%d", &h, &m, &sec) >= 2)
                return h * 3600 + m * 60 + sec;
        return -1;
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
        int ts_s = parse_time_secs(s_flt_ts_start);
        int ts_e = parse_time_secs(s_flt_ts_end);
        if (ts_s >= 0 || ts_e >= 0) {
                s_flt_indices.erase(
                        std::remove_if(s_flt_indices.begin(), s_flt_indices.end(), [&](int idx) {
                                int t = parse_time_secs(s_chat[idx].timestamp.c_str());
                                if (t < 0) return false;
                                if (ts_s >= 0 && t < ts_s) return true;
                                if (ts_e >= 0 && t > ts_e) return true;
                                return false;
                        }),
                        s_flt_indices.end()
                );
        }
        for (int idx : s_flt_indices)
                s_flt_chars += s_chat[idx].plain.size() + s_chat[idx].timestamp.size() + 3;
}

static void delete_chat_line(int idx) {
        if (idx < 0 || idx >= (int)s_chat.size()) return;
        s_chat.erase(s_chat.begin() + idx);
        s_total_chars = 0;
        for (auto &l : s_chat)
                s_total_chars += l.plain.size() + l.timestamp.size() + 3;
        for (int j = (int)s_flt_indices.size() - 1; j >= 0; j--) {
                if (s_flt_indices[j] == idx)
                        s_flt_indices.erase(s_flt_indices.begin() + j);
                else if (s_flt_indices[j] > idx)
                        s_flt_indices[j]--;
        }
        s_flt_chars = 0;
        for (int k : s_flt_indices)
                s_flt_chars += s_chat[k].plain.size() + s_chat[k].timestamp.size() + 3;
}

static void do_find_latest(GLFWwindow *w) {
        char path[MAX_PATH * 2];
        if (!find_latest_log(path, sizeof(path))) {
                MessageBoxA(glfwGetWin32Window(w), "No session found.\n\n" "Expected location:\n" "%LOCALAPPDATA%\\FiveM\\FiveM.app\\logs\\\n\n" "Launch FiveM and join a server first.", "Error", MB_ICONWARNING);
                return;
        }
        snprintf(s_path, sizeof(s_path), "%s", path);
}

static void do_browse(GLFWwindow *w) {
        char initdir[MAX_PATH + 32] = "";
        get_logs_dir(initdir, sizeof(initdir));
        OPENFILENAMEA ofn = {};
        char fname[MAX_PATH] = "";
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = glfwGetWin32Window(w);
        ofn.lpstrFilter = "Session\0CitizenFX_log_*.log\0" "All Log Files\0*.log\0" "All Files\0*.*\0";
        ofn.lpstrFile = fname;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST;
        ofn.lpstrTitle = "Select Session";
        ofn.lpstrInitialDir = initdir[0] ? initdir : NULL;
        if (GetOpenFileNameA(&ofn))
                snprintf(s_path, sizeof(s_path), "%s", fname);
}

static void do_parse(GLFWwindow *w) {
        if (!s_path[0]) {
                MessageBoxA(glfwGetWin32Window(w), "Please select or auto-detect a log file first.", "Error", MB_ICONWARNING);
                return;
        }
        ChatLog *log = parse_log_chat(s_path, 0);
        if (!log) {
                MessageBoxA(glfwGetWin32Window(w), "Failed to open log file.\n\n" "Make sure FiveM has been launched at least once " "and the file exists at the specified path.", "Error", MB_ICONERROR);
                return;
        }
        std::vector<ChatLine> tmp;
        tmp.reserve((size_t)log->count);
        s_total_chars = 0;
        for (int i = 0; i < log->count; i++) {
                ChatEntry *e = &log->entries[i];
                ChatLine cl;
                cl.timestamp = e->timestamp;
                cl.raw = e->raw;
                cl.plain = e->plain;
                cl.segments = parse_segments(e->raw);
                s_total_chars += strlen(e->plain) + strlen(e->timestamp) + 3;
                tmp.push_back(std::move(cl));
        }
        chatlog_free(log);
        s_chat = std::move(tmp);
        s_bulk_select_mode = false;
        s_bulk_sel.clear();
        s_find_result = -1;
        s_flt_indices.clear();
        s_flt_indices.shrink_to_fit();
        s_flt_chars = 0;
        if (s_show_filter) {
                s_flt_indices.reserve(s_chat.size());
                for (int i = 0; i < (int)s_chat.size(); i++)
                        s_flt_indices.push_back(i);
                s_flt_chars = s_total_chars;
        }
}

static void do_save(GLFWwindow *w, const std::string &text, const char *default_name) {
        if (text.empty()) {
                MessageBoxA(glfwGetWin32Window(w), "Nothing to save - parse a log first.", "Empty", MB_ICONINFORMATION);
                return;
        }
        OPENFILENAMEA ofn = {};
        char fname[MAX_PATH];
        strncpy(fname, default_name, MAX_PATH - 1);
        fname[MAX_PATH - 1] = '\0';
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = glfwGetWin32Window(w);
        ofn.lpstrFilter = "Text Files (*.txt)\0*.txt\0All Files\0*.*\0";
        ofn.lpstrFile = fname;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_OVERWRITEPROMPT;
        ofn.lpstrDefExt = "txt";
        if (!GetSaveFileNameA(&ofn))
                return;
        FILE *f = fopen(fname, "wb");
        if (f) {
                fwrite(text.c_str(), 1, text.size(), f);
                fclose(f);
                MessageBoxA(glfwGetWin32Window(w), "Saved.", "Saved", MB_ICONINFORMATION);
        } else {
                MessageBoxA(glfwGetWin32Window(w), "Failed to save file.", "Error", MB_ICONERROR);
        }
}

static void do_copy(GLFWwindow *w, const std::string &text) {
        if (!text.empty())
                glfwSetClipboardString(w, text.c_str());
}

static void do_export_png(GLFWwindow *w, const std::vector<ChatLine> &lines) {
        if (lines.empty()) {
                MessageBoxA(glfwGetWin32Window(w), "Nothing to export - parse a log first.", "Empty", MB_ICONINFORMATION);
                return;
        }
        OPENFILENAMEA ofn = {};
        char fname[MAX_PATH] = "chat_log.png";
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = glfwGetWin32Window(w);
        ofn.lpstrFilter = "PNG Image (*.png)\0*.png\0All Files\0*.*\0";
        ofn.lpstrFile = fname;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_OVERWRITEPROMPT;
        ofn.lpstrDefExt = "png";
        if (!GetSaveFileNameA(&ofn)) return;
        if (export_chat_png(fname, lines, g_config.wrap_width, !s_remove_ts, s_png_bg[0], s_png_bg[1], s_png_bg[2], s_png_bg[3])) {
                MessageBoxA(glfwGetWin32Window(w), "PNG exported successfully.", "Exported", MB_ICONINFORMATION);
        } else {
                MessageBoxA(glfwGetWin32Window(w), "Failed to export PNG.\n\nArial font was not found on this system.", "Error", MB_ICONERROR);
        }
}

void ui_init(GLFWwindow *w) {
        (void)w;
        s_remove_ts = (g_config.remove_timestamps != 0);
        s_png_bg[0] = g_config.png_bg_r / 255.0f;
        s_png_bg[1] = g_config.png_bg_g / 255.0f;
        s_png_bg[2] = g_config.png_bg_b / 255.0f;
        s_png_bg[3] = g_config.png_bg_a / 255.0f;
        char path[MAX_PATH * 2];
        if (find_latest_log(path, sizeof(path)))
                snprintf(s_path, sizeof(s_path), "%s", path);
}

bool ui_get_remove_timestamps(void) {
        return s_remove_ts;
}

void ui_shutdown(void) {}

void ui_render(GLFWwindow *w) {
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
        float avail = ImGui::GetContentRegionAvail().x;
        float browse_w = 75, find_w = 85;
        float path_offset = browse_w + find_w + ImGui::GetStyle().ItemSpacing.x * 2;
        ImGui::SetNextItemWidth(avail - path_offset);
        ImGui::InputText("##path", s_path, sizeof(s_path));
        ImGui::SameLine();
        if (ImGui::Button("Browse", ImVec2(75, 0)))
                do_browse(w);
        ImGui::SameLine();
        if (ImGui::Button("Find Latest", ImVec2(85, 0)))
                do_find_latest(w);
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
                for (int i = 0; i < (int)s_chat.size(); i++) {
                        ImGui::PushID(i);
                        ImVec2 row_top = ImGui::GetCursorScreenPos();
                        if (s_bulk_select_mode && i < (int)s_bulk_sel.size()) {
                                bool chk = s_bulk_sel[i];
                                ImGui::Checkbox("##chk", &chk);
                                s_bulk_sel[i] = chk;
                                ImGui::SameLine(0, 4);
                        }
                        ImGui::BeginGroup();
                        render_chat_line(s_chat[i]);
                        ImGui::EndGroup();
                        if (i == s_find_result) {
                                ImVec2 row_bot = ImGui::GetCursorScreenPos();
                                ImVec2 wpos = ImGui::GetWindowPos();
                                float rw = ImGui::GetWindowWidth();
                                ImGui::GetWindowDrawList()->AddRectFilled(
                                        ImVec2(wpos.x, row_top.y),
                                        ImVec2(wpos.x + rw, row_bot.y),
                                        IM_COL32(255, 220, 0, 50)
                                );
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
        ImGui::EndChild();
        if (s_pending_delete >= 0) {
                delete_chat_line(s_pending_delete);
                s_pending_delete = -1;
        }
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
                                s_total_chars = 0;
                                for (auto &l : s_chat) s_total_chars += l.plain.size() + l.timestamp.size() + 3;
                                s_flt_chars = 0;
                                for (int k : s_flt_indices) s_flt_chars += s_chat[k].plain.size() + s_chat[k].timestamp.size() + 3;
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
                        for (int j = (int)s_bulk_sel.size() - 1; j >= 0; j--)
                                if (s_bulk_sel[j]) delete_chat_line(j);
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
                ImGui::Text("%u characters  |  %u messages", (unsigned)s_total_chars, (unsigned)s_chat.size());
                ImGui::Checkbox("Remove timestamps", &s_remove_ts);
                ImGui::SameLine();
                ImGui::Text("Wrap:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(55);
                if (ImGui::InputInt("##wrap", &g_config.wrap_width, 0, 0)) {
                        if (g_config.wrap_width < 100) g_config.wrap_width = 100;
                        if (g_config.wrap_width > 2000) g_config.wrap_width = 2000;
                }
                ImGui::SameLine();
                float bw = 72, bw_exp = 92, bg_btn = 20;
                float sp = ImGui::GetStyle().ItemSpacing.x;
                float total = bw * 3 + bw_exp + bg_btn + sp * 4;
                ImGui::SameLine(ImGui::GetWindowWidth() - total - ImGui::GetStyle().WindowPadding.x);
                if (ImGui::Button("PARSE", ImVec2(bw, 0)))
                        do_parse(w);
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
                ImGui::Text(APP_TITLE);
                ImGui::Text("Version " APP_VERSION);
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::Text("https://github.com/bd53");
                ImGui::Text("https://discord.gg/users/colonelfuhrberger");
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                float btn_w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
                if (ImGui::Button("OK", ImVec2(btn_w, 0)))
                        ImGui::CloseCurrentPopup();
                ImGui::SameLine();
                if (ImGui::Button("Close", ImVec2(btn_w, 0)))
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
                                s_flt_ts_start[0] = '\0';
                                s_flt_ts_end[0] = '\0';
                                s_regex_error.clear();
                                s_flt_indices.clear();
                                for (int i = 0; i < (int)s_chat.size(); i++)
                                        s_flt_indices.push_back(i);
                                s_flt_chars = s_total_chars;
                        }
                        ImGui::SameLine();
                        ImGui::Checkbox("Regex", &s_use_regex);
                        ImGui::Text("Time:");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(75);
                        ImGui::InputText("##ts_start", s_flt_ts_start, sizeof(s_flt_ts_start));
                        ImGui::SameLine();
                        ImGui::Text("to");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(75);
                        ImGui::InputText("##ts_end", s_flt_ts_end, sizeof(s_flt_ts_end));
                        ImGui::SameLine();
                        ImGui::TextDisabled("HH:MM:SS");
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
                        if (s_pending_delete >= 0) {
                                delete_chat_line(s_pending_delete);
                                s_pending_delete = -1;
                        }
                        if (s_flt_bulk_select_mode) {
                                int flt_sel_count = 0;
                                for (bool b : s_flt_bulk_sel) if (b) flt_sel_count++;
                                ImGui::Text("%d selected", flt_sel_count);
                                ImGui::SameLine();
                                char flt_del_label[64];
                                snprintf(flt_del_label, sizeof(flt_del_label), "Delete (%d)###fsel_del", flt_sel_count);
                                if (ImGui::Button(flt_del_label, ImVec2(110, 0))) {
                                        std::vector<int> to_delete;
                                        for (int j = 0; j < (int)s_flt_bulk_sel.size(); j++)
                                                if (s_flt_bulk_sel[j] && j < (int)s_flt_indices.size())
                                                        to_delete.push_back(s_flt_indices[j]);
                                        std::sort(to_delete.begin(), to_delete.end(), std::greater<int>());
                                        to_delete.erase(std::unique(to_delete.begin(), to_delete.end()), to_delete.end());
                                        for (int idx : to_delete)
                                                delete_chat_line(idx);
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
