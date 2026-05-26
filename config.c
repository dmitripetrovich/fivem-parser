#include <windows.h>
#include "parser.h"
#include <stdio.h>
#include <string.h>

Config g_config;

static void get_ini_path(char *out, int size) {
        GetModuleFileNameA(NULL, out, size);
        out[size - 1] = '\0';
        char *last = strrchr(out, '\\');
        if (last) {
                *(last + 1) = '\0';
        } else {
                out[0] = '\0';
        }
        int used = (int)strlen(out);
        int remaining = size - used - 1;
        if (remaining > 0)
                strncat(out, "config.ini", (size_t)remaining);
}

void config_load(void) {
        char ini[MAX_PATH];
        get_ini_path(ini, MAX_PATH);
        g_config.show_timestamps = GetPrivateProfileIntA("General", "ShowTimestamps", 1, ini);
        g_config.wrap_width = GetPrivateProfileIntA("General", "WrapWidth", 500, ini);
        if (g_config.wrap_width < 100) g_config.wrap_width = 100;
        if (g_config.wrap_width > 2000) g_config.wrap_width = 2000;
        g_config.png_bg_r = GetPrivateProfileIntA("General", "PngBgR", 0, ini);
        g_config.png_bg_g = GetPrivateProfileIntA("General", "PngBgG", 0, ini);
        g_config.png_bg_b = GetPrivateProfileIntA("General", "PngBgB", 0, ini);
        g_config.png_bg_a = GetPrivateProfileIntA("General", "PngBgA", 0, ini);
        g_config.png_scale = GetPrivateProfileIntA("General", "PngScale", 2, ini);
        if (g_config.png_scale < 1) g_config.png_scale = 1;
        if (g_config.png_scale > 3) g_config.png_scale = 3;
}

void config_save(void) {
        char ini[MAX_PATH];
        get_ini_path(ini, MAX_PATH);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", g_config.show_timestamps);
        WritePrivateProfileStringA("General", "ShowTimestamps", buf, ini);
        snprintf(buf, sizeof(buf), "%d", g_config.wrap_width);
        WritePrivateProfileStringA("General", "WrapWidth", buf, ini);
        snprintf(buf, sizeof(buf), "%d", g_config.png_bg_r);
        WritePrivateProfileStringA("General", "PngBgR", buf, ini);
        snprintf(buf, sizeof(buf), "%d", g_config.png_bg_g);
        WritePrivateProfileStringA("General", "PngBgG", buf, ini);
        snprintf(buf, sizeof(buf), "%d", g_config.png_bg_b);
        WritePrivateProfileStringA("General", "PngBgB", buf, ini);
        snprintf(buf, sizeof(buf), "%d", g_config.png_bg_a);
        WritePrivateProfileStringA("General", "PngBgA", buf, ini);
        snprintf(buf, sizeof(buf), "%d", g_config.png_scale);
        WritePrivateProfileStringA("General", "PngScale", buf, ini);
}
