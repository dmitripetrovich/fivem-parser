#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
        int remove_timestamps;
        int wrap_width;
        int png_bg_r, png_bg_g, png_bg_b, png_bg_a;
} Config;

extern Config g_config;

void config_load(void);
void config_save(void);

typedef struct {
        char timestamp[64];
        char raw[8192];
        char plain[8192];
} ChatEntry;

typedef struct {
        ChatEntry *entries;
        int count;
        int capacity;
} ChatLog;

ChatLog *parse_log_chat(const char *path, int remove_timestamps);
void chatlog_free(ChatLog *log);

#ifdef __cplusplus
}
#endif
