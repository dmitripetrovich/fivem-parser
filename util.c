#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void strip_color_codes(char *s) {
        int j = 0;
        for (int i = 0; s[i]; i++) {
                if (s[i] == '^') {
                        char nx = s[i + 1];
                        if (nx == '#') {
                                int k = i + 2, h = 0;
                                while (s[k] && h < 6 && ((s[k] >= '0' && s[k] <= '9') || (s[k] >= 'a' && s[k] <= 'f') || (s[k] >= 'A' && s[k] <= 'F'))) {
                                        k++; h++;
                                }
                                if (h == 6) {
                                        i = k - 1;
                                        continue;
                                }
                        }
                        if ((nx >= '0' && nx <= '9') || nx == '*' || nx == '_' || nx == 'r' || nx == '~') {
                                i++;
                                continue;
                        }
                }
                if (s[i] == '~') {
                        int k = i + 1;
                        while (s[k] && s[k] != '~')
                                k++;
                        if (s[k] == '~') {
                                i = k;
                                continue;
                        }
                }
                s[j++] = s[i];
        }
        s[j] = '\0';
}

int rtrim_newline(char *s) {
        int n = (int)strlen(s);
        while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
                s[--n] = '\0';
        return n;
}

void get_logs_dir(char *out, int out_size) {
        char local[MAX_PATH];
        if (!GetEnvironmentVariableA("LOCALAPPDATA", local, MAX_PATH)) {
                out[0] = '\0';
                return;
        }
        snprintf(out, (size_t)out_size, "%s\\FiveM\\FiveM.app\\logs", local);
}

int find_latest_log(char *out, int out_size) {
        char dir[MAX_PATH * 2];
        get_logs_dir(dir, sizeof(dir));
        if (!dir[0])
                return 0;
        char pattern[MAX_PATH + 64];
        snprintf(pattern, sizeof(pattern), "%.260s\\CitizenFX_log_*.log", dir);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE)
                return 0;
        char best[MAX_PATH] = "";
        FILETIME best_time = {0, 0};
        int first = 1;
        do {
                if (first || CompareFileTime(&fd.ftLastWriteTime, &best_time) > 0) {
                        first = 0;
                        snprintf(best, sizeof(best), "%s", fd.cFileName);
                        best_time = fd.ftLastWriteTime;
                }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        snprintf(out, (size_t)out_size, "%s\\%s", dir, best);
        return 1;
}

const char *stristr(const char *h, const char *n) {
        if (!*n) return h;
        for (; *h; h++) {
                const char *a = h, *b = n;
                while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
                        a++; b++;
                }
                if (!*b) return h;
        }
        return NULL;
}
