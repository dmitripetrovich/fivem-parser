#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

struct GLFWwindow;

typedef enum {
        PLATFORM_MSG_INFO,
        PLATFORM_MSG_WARN,
        PLATFORM_MSG_ERROR,
} PlatformMsgKind;

#ifndef PARSER_PATH_MAX
#define PARSER_PATH_MAX 1024
#endif

void platform_msgbox(struct GLFWwindow *parent, const char *title, const char *message, PlatformMsgKind kind);
bool platform_open_file_dialog(struct GLFWwindow *parent, const char *title, const char *filter_desc, const char *filter_ext, char *out, int out_size);
bool platform_save_file_dialog(struct GLFWwindow *parent, const char *title, const char *default_name, const char *filter_desc, const char *filter_ext, char *out, int out_size);
void platform_get_exe_dir(char *out, int out_size);
bool platform_font_path(const char *file_name, char *out, int out_size);
void platform_local_time_hms(int *hour, int *minute, int *second);

#ifdef __cplusplus
}
#endif
