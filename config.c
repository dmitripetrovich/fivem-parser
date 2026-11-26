#include "parser.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

#define INI_SECTION "General"
#define MAX_INI_ENTRIES 32

Config g_config;

typedef struct {
        char key[64];
        int value;
} IniEntry;

static void get_ini_path(char *out, int size) {
        platform_get_exe_dir(out, size);
        int used = (int)strlen(out);
        if (used < size)
                snprintf(out + used, (size_t)(size - used), "config.ini");
}

static char *trim(char *s) {
        while (*s == ' ' || *s == '\t') s++;
        char *end = s + strlen(s);
        while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
                *--end = '\0';
        return s;
}

static int read_all(const char *path, const char *section, IniEntry *entries, int max_entries) {
        FILE *f = fopen(path, "r");
        if (!f) return 0;
        char line[512];
        char want_section[128];
        snprintf(want_section, sizeof(want_section), "[%s]", section);
        int in_section = 0;
        int count = 0;
        while (count < max_entries && fgets(line, sizeof(line), f)) {
                char *p = trim(line);
                if (!*p || *p == ';' || *p == '#') continue;
                if (*p == '[') {
                        in_section = (strcmp(p, want_section) == 0);
                        continue;
                }
                char *eq = strchr(p, '=');
                if (!in_section || !eq) continue;
                *eq = '\0';
                snprintf(entries[count].key, sizeof(entries[count].key), "%s", trim(p));
                entries[count].value = atoi(trim(eq + 1));
                count++;
        }
        fclose(f);
        return count;
}

static int get_int(const IniEntry *entries, int n_entries, const char *key, int fallback, int lo, int hi) {
        int value = fallback;
        for (int i = 0; i < n_entries; i++) {
                if (strcmp(entries[i].key, key) == 0) {
                        value = entries[i].value;
                        break;
                }
        }
        if (value < lo) value = lo;
        if (value > hi) value = hi;
        return value;
}

static void write_all(const char *path, const char *section, IniEntry *entries, int n_entries) {
        FILE *f = fopen(path, "w");
        if (!f) return;
        fprintf(f, "[%s]\n", section);
        for (int i = 0; i < n_entries; i++)
                fprintf(f, "%s=%d\n", entries[i].key, entries[i].value);
        fclose(f);
}

void config_load(void) {
        char ini[PARSER_PATH_MAX];
        get_ini_path(ini, sizeof(ini));
        IniEntry entries[MAX_INI_ENTRIES];
        int n = read_all(ini, INI_SECTION, entries, MAX_INI_ENTRIES);
        g_config.show_timestamps = get_int(entries, n, "ShowTimestamps", 1, INT_MIN, INT_MAX) != 0;
        g_config.wrap_width = get_int(entries, n, "WrapWidth", 500, 100, 2000);
        g_config.png_bg_r = get_int(entries, n, "PngBgR", 0, 0, 255);
        g_config.png_bg_g = get_int(entries, n, "PngBgG", 0, 0, 255);
        g_config.png_bg_b = get_int(entries, n, "PngBgB", 0, 0, 255);
        g_config.png_bg_a = get_int(entries, n, "PngBgA", 0, 0, 255);
        g_config.png_scale = get_int(entries, n, "PngScale", 2, 1, 3);
}

void config_save(void) {
        char ini[PARSER_PATH_MAX];
        get_ini_path(ini, sizeof(ini));
        IniEntry entries[] = {
                { "ShowTimestamps", g_config.show_timestamps },
                { "WrapWidth", g_config.wrap_width },
                { "PngBgR", g_config.png_bg_r },
                { "PngBgG", g_config.png_bg_g },
                { "PngBgB", g_config.png_bg_b },
                { "PngBgA", g_config.png_bg_a },
                { "PngScale", g_config.png_scale },
        };
        write_all(ini, INI_SECTION, entries, (int)(sizeof(entries) / sizeof(entries[0])));
}
