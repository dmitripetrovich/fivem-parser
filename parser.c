#include "resource.h"
#include "parser.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

ChatLog *parse_log_chat(const char *path, int remove_timestamps) {
        FILE *f = fopen(path, "r");
        if (!f)
                return NULL;
        ChatLog *log = (ChatLog *)calloc(1, sizeof(ChatLog));
        if (!log) {
                fclose(f);
                return NULL;
        }
        log->capacity = 256;
        log->entries = (ChatEntry *)malloc((size_t)log->capacity * sizeof(ChatEntry));
        if (!log->entries) {
                free(log);
                fclose(f);
                return NULL;
        }
        char line[8192];
        while (fgets(line, sizeof(line), f)) {
                size_t llen = strlen(line);
                if (llen > 0 && line[llen - 1] != '\n') {
                        int ch;
                        while ((ch = fgetc(f)) != '\n' && ch != EOF);
                }
                char *chat = strstr(line, "[chat] ");
                if (!chat)
                        continue;
                chat += 7;
                if (*chat == ' ')
                        chat++;
                char raw[8192];
                snprintf(raw, sizeof(raw), "%s", chat);
                if (!rtrim_newline(raw))
                        continue;
                char plain[8192];
                snprintf(plain, sizeof(plain), "%s", raw);
                strip_color_codes(plain);
                if (!plain[0])
                        continue;
                if (log->count >= log->capacity) {
                        int new_cap = log->capacity > (INT_MAX / 2) ? INT_MAX : log->capacity * 2;
                        if (new_cap <= log->capacity)
                                break;
                        ChatEntry *tmp = (ChatEntry *)realloc(log->entries, (size_t)new_cap * sizeof(ChatEntry));
                        if (!tmp)
                                break;
                        log->entries = tmp;
                        log->capacity = new_cap;
                }
                ChatEntry *e = &log->entries[log->count];
                e->timestamp[0] = '\0';
                if (!remove_timestamps && line[0] == '[') {
                        char *end = strchr(line, ']');
                        if (end) {
                                char numstr[64] = "";
                                int nlen = (int)(end - line) - 1;
                                if (nlen > 0 && nlen < (int)sizeof(numstr)) {
                                        memcpy(numstr, line + 1, (size_t)nlen);
                                        numstr[nlen] = '\0';
                                        long ms = atol(numstr);
                                        if (ms >= 0) {
                                                long total_sec = ms / 1000;
                                                int h = (int)(total_sec / 3600);
                                                int m = (int)((total_sec % 3600) / 60);
                                                int s = (int)(total_sec % 60);
                                                snprintf(e->timestamp, sizeof(e->timestamp), "[%02d:%02d:%02d]", h, m, s);
                                        }
                                }
                        }
                }
                snprintf(e->raw, sizeof(e->raw), "%s", raw);
                snprintf(e->plain, sizeof(e->plain), "%s", plain);
                log->count++;
        }
        fclose(f);
        return log;
}

void chatlog_free(ChatLog *log) {
        if (log) {
                free(log->entries);
                free(log);
        }
}
