#include "parser.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

Config g_config;

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

static int read_int_key(const char *path, const char *section, const char *key, int fallback) {
        FILE *f = fopen(path, "r");
        if (!f) return fallback;
        char line[512];
        int in_section = 0;
        char want_section[128];
        snprintf(want_section, sizeof(want_section), "[%s]", section);
        size_t klen = strlen(key);
        int result = fallback;
        while (fgets(line, sizeof(line), f)) {
                char *p = trim(line);
                if (!*p || *p == ';' || *p == '#') continue;
                if (*p == '[') {
                        in_section = (strcmp(p, want_section) == 0);
                        continue;
                }
                if (!in_section) continue;
                if (strncmp(p, key, klen) == 0) {
                        char *eq = p + klen;
                        while (*eq == ' ' || *eq == '\t') eq++;
                        if (*eq != '=') continue;
                        eq++;
                        char *val = trim(eq);
                        result = atoi(val);
                        break;
                }
        }
        fclose(f);
        return result;
}

typedef struct {
        char key[64];
        int value;
} IniEntry;

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
        g_config.show_timestamps = read_int_key(ini, "General", "ShowTimestamps", 1);
        g_config.wrap_width = read_int_key(ini, "General", "WrapWidth", 500);
        if (g_config.wrap_width < 100) g_config.wrap_width = 100;
        if (g_config.wrap_width > 2000) g_config.wrap_width = 2000;
        g_config.png_bg_r = read_int_key(ini, "General", "PngBgR", 0);
        g_config.png_bg_g = read_int_key(ini, "General", "PngBgG", 0);
        g_config.png_bg_b = read_int_key(ini, "General", "PngBgB", 0);
        g_config.png_bg_a = read_int_key(ini, "General", "PngBgA", 0);
        g_config.png_scale = read_int_key(ini, "General", "PngScale", 2);
        if (g_config.png_scale < 1) g_config.png_scale = 1;
        if (g_config.png_scale > 3) g_config.png_scale = 3;
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
        write_all(ini, "General", entries, (int)(sizeof(entries) / sizeof(entries[0])));
}
