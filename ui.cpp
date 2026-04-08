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


static void render_chat_line(const ChatLine &line) {
        bool has_prev = false;
        if (!line.timestamp.empty()) {
                ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.55f, 1.0f), "%s ", line.timestamp.c_str());
                has_prev = true;
        }
        for (size_t i = 0; i < line.segments.size(); i++) {
                if (has_prev)
                        ImGui::SameLine(0, 0);
                ImGui::TextColored(line.segments[i].color, "%s", line.segments[i].text.c_str());
                has_prev = true;
        }
        if (!has_prev)
                ImGui::TextUnformatted("");
}

static std::string build_plain(const std::vector<ChatLine> &lines) {
        std::string out;
        out.reserve(s_total_chars);
        for (auto &l : lines) {
                if (!l.timestamp.empty()) { out += l.timestamp; out += ' '; }
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
                if (!l.timestamp.empty()) { out += l.timestamp; out += ' '; }
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
        ChatLog *log = parse_log_chat(s_path, s_remove_ts ? 1 : 0);
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
        if (export_chat_png(fname, lines)) {
                MessageBoxA(glfwGetWin32Window(w), "PNG exported successfully.", "Exported", MB_ICONINFORMATION);
        } else {
                MessageBoxA(glfwGetWin32Window(w), "Failed to export PNG.\n\nArial font was not found on this system.", "Error", MB_ICONERROR);
        }
}

void ui_init(GLFWwindow *w) {
        (void)w;
        s_remove_ts = (g_config.remove_timestamps != 0);
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
        float footer = ImGui::GetFrameHeightWithSpacing() * 2 + 8;
        ImGui::BeginChild("##output", ImVec2(0, -footer), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        if (!s_chat.empty()) {
                ImGuiListClipper clipper;
                clipper.Begin((int)s_chat.size());
                while (clipper.Step()) {
                        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                                ImGui::PushID(i);
                                ImGui::BeginGroup();
                                render_chat_line(s_chat[i]);
                                ImGui::EndGroup();
                                if (ImGui::BeginPopupContextItem("##ctx", ImGuiPopupFlags_MouseButtonRight)) {
                                        if (ImGui::MenuItem("Edit Line")) {
                                                s_edit_line = i;
                                                strncpy(s_edit_buf, s_chat[i].raw.c_str(), sizeof(s_edit_buf) - 1);
                                                s_edit_buf[sizeof(s_edit_buf) - 1] = '\0';
                                                s_open_edit_popup = true;
                                        }
                                        if (ImGui::MenuItem("Delete Line"))
                                                s_pending_delete = i;
                                        ImGui::EndPopup();
                                }
                                ImGui::PopID();
                        }
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
        ImGui::Text("%u characters  |  %u messages", (unsigned)s_total_chars, (unsigned)s_chat.size());
        ImGui::Checkbox("Remove timestamps", &s_remove_ts);
        ImGui::SameLine();
        float bw = 72, bw_exp = 92;
        float sp = ImGui::GetStyle().ItemSpacing.x;
        float total = bw * 3 + bw_exp + sp * 3;
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
        if (ImGui::Button("EXPORT PNG", ImVec2(bw_exp, 0)))
                do_export_png(w, s_chat);
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
                ImGui::Text("https://t.me/enclaimed");
                ImGui::Text("https://discord.gg/users/death_enclaimed");
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                if (ImGui::Button("OK", ImVec2(120, 0)))
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
                        ImGui::BeginChild("##flt_out", ImVec2(0, -flt_footer), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
                        if (!s_flt_indices.empty()) {
                                ImGuiListClipper clipper;
                                clipper.Begin((int)s_flt_indices.size());
                                while (clipper.Step()) {
                                        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                                                int chat_idx = s_flt_indices[i];
                                                ImGui::PushID(i);
                                                ImGui::BeginGroup();
                                                render_chat_line(s_chat[chat_idx]);
                                                ImGui::EndGroup();
                                                if (ImGui::BeginPopupContextItem("##fctx", ImGuiPopupFlags_MouseButtonRight)) {
                                                        if (ImGui::MenuItem("Edit Line")) {
                                                                s_edit_line = chat_idx;
                                                                strncpy(s_edit_buf, s_chat[chat_idx].raw.c_str(), sizeof(s_edit_buf) - 1);
                                                                s_edit_buf[sizeof(s_edit_buf) - 1] = '\0';
                                                                s_open_edit_popup = true;
                                                        }
                                                        if (ImGui::MenuItem("Delete Line"))
                                                                s_pending_delete = chat_idx;
                                                        ImGui::EndPopup();
                                                }
                                                ImGui::PopID();
                                        }
                                }
                        }
                        ImGui::EndChild();
                        if (s_pending_delete >= 0) {
                                delete_chat_line(s_pending_delete);
                                s_pending_delete = -1;
                        }
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
                ImGui::End();
        }
}
