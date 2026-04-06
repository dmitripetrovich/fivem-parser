#include "resource.h"
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
        g_config.remove_timestamps = GetPrivateProfileIntA("General", "RemoveTimestamps", 0, ini);
}

void config_save(void) {
        char ini[MAX_PATH];
        get_ini_path(ini, MAX_PATH);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", g_config.remove_timestamps);
        WritePrivateProfileStringA("General", "RemoveTimestamps", buf, ini);
}
