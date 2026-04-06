#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

void strip_color_codes(char *s);
int rtrim_newline(char *s);
void get_logs_dir(char *out, int out_size);
int find_latest_log(char *out, int out_size);
const char *stristr(const char *h, const char *n);

#ifdef __cplusplus
}
#endif
