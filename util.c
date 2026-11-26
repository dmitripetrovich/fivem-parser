#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_TILDE_TOKEN 32

static int hex_value(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
}

int color_code_at(const char *s, ColorCode *kind, unsigned *value) {
        *kind = CODE_NONE;
        *value = 0;
        if (s[0] == '^') {
                char nx = s[1];
                if (nx == '#') {
                        unsigned rgb = 0;
                        int h = 0;
                        while (h < 6 && hex_value(s[2 + h]) >= 0) {
                                rgb = (rgb << 4) | (unsigned)hex_value(s[2 + h]);
                                h++;
                        }
                        if (h == 6) {
                                *kind = CODE_RGB;
                                *value = rgb;
                                return 8;
                        }
                }
                if (nx >= '0' && nx <= '9') {
                        *kind = CODE_PALETTE;
                        *value = (unsigned)(nx - '0');
                        return 2;
                }
                if (nx == 'r' || nx == '~') {
                        *kind = CODE_RESET;
                        return 2;
                }
                if (nx == '*' || nx == '_') {
                        *kind = CODE_IGNORED;
                        return 2;
                }
        }
        if (s[0] == '~') {
                int k = 1;
                while (k <= MAX_TILDE_TOKEN && (isalnum((unsigned char)s[k]) || s[k] == '_'))
                        k++;
                if (k > 1 && s[k] == '~') {
                        *kind = CODE_IGNORED;
                        return k + 1;
                }
        }
        return 0;
}

void strip_color_codes(char *s) {
        int j = 0;
        for (int i = 0; s[i];) {
                ColorCode kind;
                unsigned value;
                int n = color_code_at(s + i, &kind, &value);
                if (n) {
                        i += n;
                        continue;
                }
                s[j++] = s[i++];
        }
        s[j] = '\0';
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
