#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <windows.h>
#include <commdlg.h>
#include <cstdio>
#include <cstring>

#include "platform.h"

static HWND parent_hwnd(GLFWwindow *w) {
        return w ? glfwGetWin32Window(w) : NULL;
}

static UINT msgbox_flag(PlatformMsgKind kind) {
        switch (kind) {
                case PLATFORM_MSG_WARN: return MB_ICONWARNING;
                case PLATFORM_MSG_ERROR: return MB_ICONERROR;
                default: return MB_ICONINFORMATION;
        }
}

void platform_msgbox(GLFWwindow *parent, const char *title, const char *message, PlatformMsgKind kind) {
        MessageBoxA(parent_hwnd(parent), message, title, msgbox_flag(kind));
}

static void build_filter(char *buf, int buf_size, const char *desc, const char *ext) {
        int n = 0;
        n += snprintf(buf + n, buf_size - n, "%s (*.%s)", desc, ext);
        n++;
        if (n >= buf_size) return;
        n += snprintf(buf + n, buf_size - n, "*.%s", ext);
        n++;
        if (n >= buf_size) return;
        n += snprintf(buf + n, buf_size - n, "All Files");
        n++;
        if (n >= buf_size) return;
        n += snprintf(buf + n, buf_size - n, "*.*");
        n++;
        if (n >= buf_size) return;
        buf[n] = '\0';
}

bool platform_open_file_dialog(GLFWwindow *parent, const char *title, const char *filter_desc, const char *filter_ext, char *out, int out_size) {
        char filter[256];
        build_filter(filter, sizeof(filter), filter_desc, filter_ext);
        OPENFILENAMEA ofn = {};
        out[0] = '\0';
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = parent_hwnd(parent);
        ofn.lpstrFilter = filter;
        ofn.lpstrFile = out;
        ofn.nMaxFile = out_size;
        ofn.Flags = OFN_FILEMUSTEXIST;
        ofn.lpstrTitle = title;
        return GetOpenFileNameA(&ofn) != 0;
}

bool platform_save_file_dialog(GLFWwindow *parent, const char *title, const char *default_name, const char *filter_desc, const char *filter_ext, char *out, int out_size) {
        char filter[256];
        build_filter(filter, sizeof(filter), filter_desc, filter_ext);
        OPENFILENAMEA ofn = {};
        strncpy(out, default_name ? default_name : "", out_size - 1);
        out[out_size - 1] = '\0';
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = parent_hwnd(parent);
        ofn.lpstrFilter = filter;
        ofn.lpstrFile = out;
        ofn.nMaxFile = out_size;
        ofn.Flags = OFN_OVERWRITEPROMPT;
        ofn.lpstrDefExt = filter_ext;
        ofn.lpstrTitle = title;
        return GetSaveFileNameA(&ofn) != 0;
}

void platform_get_exe_dir(char *out, int out_size) {
        if (out_size <= 0) return;
        GetModuleFileNameA(NULL, out, out_size);
        out[out_size - 1] = '\0';
        char *last = strrchr(out, '\\');
        if (last) {
                *(last + 1) = '\0';
        } else {
                out[0] = '\0';
        }
}

void platform_local_time_hms(int *hour, int *minute, int *second) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        if (hour) *hour = st.wHour;
        if (minute) *minute = st.wMinute;
        if (second) *second = st.wSecond;
}
