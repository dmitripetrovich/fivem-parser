#include "parser.h"

#include <windows.h>
#include <tlhelp32.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define SCAN_INTERVAL_MS 500
#define FAST_INTERVAL_MS 200
#define FULL_SCAN_EVERY 5
#define SEED_TIMEOUT_MS 3000
#define PASS_DEDUP_CLEAR_MS 10000
#define JOIN_TIMEOUT_MS 5000

#define MSG_QUEUE_CAP 256
#define MAX_PENDING 256
#define MAX_HOTSPOTS 256
#define SEEN_CAP 16384
#define PASS_CAP 512

#define READ_STEP (64 * 1024)
#define CTX_LEN 2048
#define MAX_MARKER_LEN 23
#define READ_BUF_SIZE (READ_STEP + MAX_MARKER_LEN + CTX_LEN)
#define MAX_REGION_SIZE (32 * 1024 * 1024)

#define FNV_SEED 14695981039346656037ULL
#define FNV_PRIME 1099511628211ULL

typedef unsigned long long u64;

typedef struct {
        u64 *keys;
        unsigned cap;
        unsigned count;
} HashSet;

static HANDLE s_thread;
static volatile LONG s_running;
static volatile LONG s_status;
static CRITICAL_SECTION s_cs;
static int s_cs_init;

static ScannedMsg *s_queue;
static int s_head, s_tail;

static ScannedMsg s_pending[MAX_PENDING];
static int s_pending_count;

static u64 s_seen_keys[2][SEEN_CAP];
static HashSet s_seen[2] = { { s_seen_keys[0], SEEN_CAP, 0 }, { s_seen_keys[1], SEEN_CAP, 0 } };
static int s_seen_cur;

static u64 s_pass_keys[PASS_CAP];
static HashSet s_pass = { s_pass_keys, PASS_CAP, 0 };

static int s_seeded;
static DWORD s_seed_start;

static unsigned char *s_hot_bases[MAX_HOTSPOTS];
static int s_hot_count;

static void set_clear(HashSet *s) {
        memset(s->keys, 0, s->cap * sizeof(u64));
        s->count = 0;
}

static int set_full(const HashSet *s) {
        return s->count >= s->cap / 4 * 3;
}

static int set_has(const HashSet *s, u64 h) {
        for (unsigned i = (unsigned)h & (s->cap - 1); s->keys[i]; i = (i + 1) & (s->cap - 1))
                if (s->keys[i] == h) return 1;
        return 0;
}

static void set_add(HashSet *s, u64 h) {
        unsigned i = (unsigned)h & (s->cap - 1);
        while (s->keys[i])
                i = (i + 1) & (s->cap - 1);
        s->keys[i] = h;
        s->count++;
}

static u64 fnv1a(const void *data, size_t len, u64 h) {
        const unsigned char *p = (const unsigned char *)data;
        for (size_t i = 0; i < len; i++) {
                h ^= p[i];
                h *= FNV_PRIME;
        }
        return h;
}

static int text_seen_this_pass(u64 h) {
        if (set_has(&s_pass, h)) return 1;
        if (!set_full(&s_pass)) set_add(&s_pass, h);
        return 0;
}

static int site_seen(u64 h) {
        HashSet *cur = &s_seen[s_seen_cur];
        if (set_has(cur, h)) return 1;
        int known = set_has(&s_seen[!s_seen_cur], h);
        if (set_full(cur)) {
                s_seen_cur = !s_seen_cur;
                cur = &s_seen[s_seen_cur];
                set_clear(cur);
        }
        set_add(cur, h);
        return known;
}

static void copy_msg(ScannedMsg *dst, const ScannedMsg *src) {
        memcpy(dst->raw, src->raw, strlen(src->raw) + 1);
}

static int enqueue(const ScannedMsg *msg) {
        int ok = 0;
        EnterCriticalSection(&s_cs);
        int next = (s_head + 1) % MSG_QUEUE_CAP;
        if (s_queue && next != s_tail) {
                copy_msg(&s_queue[s_head], msg);
                s_head = next;
                ok = 1;
        }
        LeaveCriticalSection(&s_cs);
        return ok;
}

static void flush_pending(void) {
        int sent = 0;
        while (sent < s_pending_count && enqueue(&s_pending[sent]))
                sent++;
        s_pending_count -= sent;
        for (int i = 0; sent && i < s_pending_count; i++)
                copy_msg(&s_pending[i], &s_pending[i + sent]);
}

static const unsigned char *find_bytes(const unsigned char *hay, size_t hay_len, const unsigned char *needle, size_t needle_len) {
        if (needle_len == 0 || needle_len > hay_len) return NULL;
        const unsigned char *p = hay;
        size_t remaining = hay_len;
        unsigned char first = needle[0];
        while (remaining >= needle_len) {
                const unsigned char *f = (const unsigned char *)memchr(p, first, remaining - needle_len + 1);
                if (!f) return NULL;
                if (memcmp(f, needle, needle_len) == 0)
                        return f;
                size_t skip = (size_t)(f - p) + 1;
                p += skip;
                remaining -= skip;
        }
        return NULL;
}

static int next_marker(const unsigned char *buf, size_t len, size_t *pos, size_t *start, int *mlen) {
        while (*pos < len) {
                const unsigned char *tail = find_bytes(buf + *pos, len - *pos, (const unsigned char *)"_MESSAGE", 8);
                if (!tail) return 0;
                size_t at = (size_t)(tail - buf);
                *pos = at + 8;
                if (at >= 15 && memcmp(tail - 15, "FUNC::CHAT::ADD", 15) == 0) {
                        *start = at - 15;
                        *mlen = 23;
                        return 1;
                }
                if (at >= 2 && memcmp(tail - 2, "ON", 2) == 0) {
                        *start = at - 2;
                        *mlen = 10;
                        return 1;
                }
        }
        return 0;
}

static int decode_hex4(const char *p, const char *end, unsigned *out) {
        if (p + 4 > end) return 0;
        unsigned cp = 0;
        for (int k = 0; k < 4; k++) {
                char c = p[k];
                cp <<= 4;
                if (c >= '0' && c <= '9') cp |= (unsigned)(c - '0');
                else if (c >= 'a' && c <= 'f') cp |= (unsigned)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') cp |= (unsigned)(c - 'A' + 10);
                else return 0;
        }
        *out = cp;
        return 1;
}

static int encode_utf8(unsigned cp, char *out, int max) {
        if (cp < 0x80) {
                if (max < 1) return 0;
                out[0] = (char)cp;
                return 1;
        }
        if (cp < 0x800) {
                if (max < 2) return 0;
                out[0] = (char)(0xC0 | (cp >> 6));
                out[1] = (char)(0x80 | (cp & 0x3F));
                return 2;
        }
        if (cp < 0x10000) {
                if (max < 3) return 0;
                out[0] = (char)(0xE0 | (cp >> 12));
                out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[2] = (char)(0x80 | (cp & 0x3F));
                return 3;
        }
        if (cp < 0x110000) {
                if (max < 4) return 0;
                out[0] = (char)(0xF0 | (cp >> 18));
                out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[3] = (char)(0x80 | (cp & 0x3F));
                return 4;
        }
        return 0;
}

static const char *extract_json_string(const char *p, const char *end, char *out, int out_sz) {
        if (p >= end || *p != '"') return NULL;
        p++;
        int j = 0;
        while (p < end && *p != '"') {
                if (*p == '\\' && p + 1 < end) {
                        p++;
                        switch (*p) {
                        case '"': case '\\': case '/':
                                if (j < out_sz - 1) out[j++] = *p;
                                break;
                        case 'n': if (j < out_sz - 1) out[j++] = '\n'; break;
                        case 't': if (j < out_sz - 1) out[j++] = '\t'; break;
                        case 'r': if (j < out_sz - 1) out[j++] = '\r'; break;
                        case 'b': if (j < out_sz - 1) out[j++] = '\b'; break;
                        case 'f': if (j < out_sz - 1) out[j++] = '\f'; break;
                        case 'u': {
                                unsigned cp = 0;
                                if (!decode_hex4(p + 1, end, &cp)) break;
                                p += 4;
                                if (cp >= 0xD800 && cp <= 0xDBFF && p + 2 < end && p[1] == '\\' && p[2] == 'u') {
                                        unsigned lo = 0;
                                        if (decode_hex4(p + 3, end, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                                p += 6;
                                        }
                                }
                                int n = encode_utf8(cp, out + j, out_sz - 1 - j);
                                j += n;
                                break;
                        }
                        default:
                                if (j < out_sz - 1) out[j++] = *p;
                                break;
                        }
                } else {
                        if (j < out_sz - 1) out[j++] = *p;
                }
                p++;
        }
        out[j] = '\0';
        if (p >= end) return NULL;
        return p + 1;
}

static int parse_chat_json(const char *data, int data_len, char *raw, size_t raw_sz, char *plain, size_t plain_sz) {
        const char *end = data + data_len;
        const char *args = (const char *)find_bytes((const unsigned char *)data, data_len, (const unsigned char *)"\"args\"", 6);
        if (!args) return 0;
        args += 6;
        while (args < end && *args != '[') args++;
        if (args >= end) return 0;
        args++;
        while (args < end && (*args == ' ' || *args == '\t' || *args == '\n' || *args == '\r'))
                args++;
        if (args >= end || *args != '"') return 0;
        static char arg0[4096];
        static char arg1[4096];
        arg0[0] = '\0';
        arg1[0] = '\0';
        const char *next = extract_json_string(args, end, arg0, sizeof(arg0));
        if (!next) return 0;
        int has_two = 0;
        while (next < end && (*next == ' ' || *next == ','))
                next++;
        if (next < end && *next == '"') {
                if (!extract_json_string(next, end, arg1, sizeof(arg1))) return 0;
                has_two = 1;
        }
        if (has_two && arg0[0])
                snprintf(raw, raw_sz, "%s: %s", arg0, arg1);
        else if (has_two)
                snprintf(raw, raw_sz, "%s", arg1);
        else
                snprintf(raw, raw_sz, "%s", arg0);
        snprintf(plain, plain_sz, "%s", raw);
        strip_color_codes(plain);
        for (int i = 0; plain[i]; i++)
                if ((plain[i] >= 'A' && plain[i] <= 'Z') || (plain[i] >= 'a' && plain[i] <= 'z') || (plain[i] >= '0' && plain[i] <= '9'))
                        return 1;
        return 0;
}

static DWORD find_fivem_pid(void) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;
        PROCESSENTRY32 pe;
        pe.dwSize = sizeof(pe);
        DWORD pid = 0;
        if (Process32First(snap, &pe)) {
                do {
                        if (strstr(pe.szExeFile, "GTAProcess")) {
                                pid = pe.th32ProcessID;
                                break;
                        }
                } while (Process32Next(snap, &pe));
        }
        CloseHandle(snap);
        return pid;
}

static int is_running(void) {
        return InterlockedCompareExchange(&s_running, 1, 1) != 0;
}

static void record_hotspot(void *base) {
        for (int i = 0; i < s_hot_count; i++)
                if (s_hot_bases[i] == (unsigned char *)base) return;
        if (s_hot_count < MAX_HOTSPOTS)
                s_hot_bases[s_hot_count++] = (unsigned char *)base;
}

static int region_readable(const MEMORY_BASIC_INFORMATION *mbi) {
        if (mbi->State != MEM_COMMIT || mbi->Type == MEM_IMAGE) return 0;
        if (mbi->Protect & PAGE_GUARD) return 0;
        DWORD prot = mbi->Protect & 0xFF;
        return prot == PAGE_READWRITE || prot == PAGE_READONLY || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_READ;
}

static void handle_message(const char *ctx, int ctx_len, size_t addr, int *found) {
        static ScannedMsg msg;
        static char plain[sizeof(msg.raw)];
        if (!parse_chat_json(ctx, ctx_len, msg.raw, sizeof(msg.raw), plain, sizeof(plain))) return;
        *found = 1;
        u64 text_hash = fnv1a(plain, strlen(plain), FNV_SEED);
        u64 site_hash = fnv1a(&addr, sizeof(addr), text_hash);
        if (!text_hash) text_hash = 1;
        if (!site_hash) site_hash = 1;
        if (site_seen(site_hash)) return;
        if (!s_seeded && GetTickCount() - s_seed_start > SEED_TIMEOUT_MS)
                s_seeded = 1;
        int text_dup = text_seen_this_pass(text_hash);
        if (s_seeded && !text_dup && s_pending_count < MAX_PENDING)
                copy_msg(&s_pending[s_pending_count++], &msg);
}

static int scan_one_region(HANDLE proc, unsigned char *base, size_t region_size, unsigned char *buf) {
        int found = 0;
        for (size_t offset = 0; offset < region_size && is_running(); offset += READ_STEP) {
                size_t want = region_size - offset;
                int last = want <= READ_BUF_SIZE;
                if (!last) want = READ_BUF_SIZE;
                SIZE_T got = 0;
                if (!ReadProcessMemory(proc, base + offset, buf, want, &got) || got == 0)
                        break;
                /* a marker past READ_STEP is left to the next chunk, where its whole context is read */
                size_t own = (last || got < READ_STEP) ? got : READ_STEP;
                size_t pos = 0, start;
                int mlen;
                while (next_marker(buf, got, &pos, &start, &mlen) && start < own) {
                        size_t ctx_end = start + mlen + CTX_LEN;
                        if (ctx_end > got) ctx_end = got;
                        handle_message((const char *)(buf + start), (int)(ctx_end - start), (size_t)(base + offset) + start, &found);
                }
                if (last) break;
        }
        return found;
}

static void scan_process(HANDLE proc, unsigned char *buf) {
        MEMORY_BASIC_INFORMATION mbi;
        unsigned char *addr = NULL;
        while (VirtualQueryEx(proc, addr, &mbi, sizeof(mbi)) && is_running()) {
                addr = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
                if (!region_readable(&mbi) || mbi.RegionSize > MAX_REGION_SIZE) continue;
                if (scan_one_region(proc, (unsigned char *)mbi.BaseAddress, mbi.RegionSize, buf))
                        record_hotspot(mbi.BaseAddress);
        }
}

static void scan_cached(HANDLE proc, unsigned char *buf) {
        for (int i = 0; i < s_hot_count && is_running(); i++) {
                MEMORY_BASIC_INFORMATION mbi;
                if (!VirtualQueryEx(proc, s_hot_bases[i], &mbi, sizeof(mbi)) || !region_readable(&mbi))
                        continue;
                size_t sz = mbi.RegionSize > MAX_REGION_SIZE ? MAX_REGION_SIZE : mbi.RegionSize;
                scan_one_region(proc, (unsigned char *)mbi.BaseAddress, sz, buf);
        }
}

static DWORD WINAPI scanner_thread(LPVOID param) {
        (void)param;
        unsigned char *buf = (unsigned char *)malloc(READ_BUF_SIZE);
        if (!buf) {
                InterlockedExchange(&s_running, 0);
                return 1;
        }
        HANDLE proc = NULL;
        DWORD pass_clear_time = 0;
        int pass = 0;
        while (is_running()) {
                if (proc) {
                        DWORD exit_code = 0;
                        if (!GetExitCodeProcess(proc, &exit_code) || exit_code != STILL_ACTIVE) {
                                CloseHandle(proc);
                                proc = NULL;
                        }
                }
                if (!proc) {
                        DWORD pid = find_fivem_pid();
                        if (pid)
                                proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
                        InterlockedExchange(&s_status, proc ? SCANNER_ATTACHED : pid ? SCANNER_DENIED : SCANNER_SEARCHING);
                        if (proc) {
                                set_clear(&s_seen[0]);
                                set_clear(&s_seen[1]);
                                set_clear(&s_pass);
                                s_hot_count = 0;
                                s_seeded = 0;
                                s_seed_start = pass_clear_time = GetTickCount();
                                pass = 0;
                        }
                }
                if (proc) {
                        if (pass > 1 && GetTickCount() - pass_clear_time > PASS_DEDUP_CLEAR_MS) {
                                set_clear(&s_pass);
                                pass_clear_time = GetTickCount();
                        }
                        if (pass % FULL_SCAN_EVERY == 0 || s_hot_count == 0) {
                                s_hot_count = 0;
                                scan_process(proc, buf);
                        } else {
                                scan_cached(proc, buf);
                        }
                        flush_pending();
                        if (!s_seeded) {
                                s_seeded = 1;
                                set_clear(&s_pass);
                                pass_clear_time = GetTickCount();
                        }
                        pass++;
                }
                int interval = (s_hot_count > 0 && pass > 1) ? FAST_INTERVAL_MS : SCAN_INTERVAL_MS;
                for (int i = 0; i < interval / 100 && is_running(); i++)
                        Sleep(100);
        }
        if (proc)
                CloseHandle(proc);
        free(buf);
        return 0;
}

static int join_thread(void) {
        if (!s_thread) return 1;
        InterlockedExchange(&s_running, 0);
        if (WaitForSingleObject(s_thread, JOIN_TIMEOUT_MS) != WAIT_OBJECT_0) return 0;
        CloseHandle(s_thread);
        s_thread = NULL;
        return 1;
}

int scanner_start(void) {
        if (!s_cs_init) {
                InitializeCriticalSection(&s_cs);
                s_cs_init = 1;
        }
        if (!join_thread()) return 0;
        ScannedMsg *queue = (ScannedMsg *)calloc(MSG_QUEUE_CAP, sizeof(ScannedMsg));
        if (!queue) return 0;
        EnterCriticalSection(&s_cs);
        free(s_queue);
        s_queue = queue;
        s_head = s_tail = 0;
        LeaveCriticalSection(&s_cs);
        s_pending_count = 0;
        InterlockedExchange(&s_status, SCANNER_SEARCHING);
        InterlockedExchange(&s_running, 1);
        s_thread = CreateThread(NULL, 0, scanner_thread, NULL, 0, NULL);
        if (!s_thread) {
                InterlockedExchange(&s_running, 0);
                InterlockedExchange(&s_status, SCANNER_IDLE);
                return 0;
        }
        return 1;
}

void scanner_stop(void) {
        if (join_thread())
                InterlockedExchange(&s_status, SCANNER_IDLE);
}

int scanner_is_running(void) {
        return is_running();
}

ScannerStatus scanner_status(void) {
        return (ScannerStatus)InterlockedCompareExchange(&s_status, 0, 0);
}

int scanner_poll(ScannedMsg *out, int max_count) {
        if (!s_cs_init) return 0;
        int count = 0;
        EnterCriticalSection(&s_cs);
        while (count < max_count && s_queue && s_tail != s_head) {
                copy_msg(&out[count++], &s_queue[s_tail]);
                s_tail = (s_tail + 1) % MSG_QUEUE_CAP;
        }
        LeaveCriticalSection(&s_cs);
        return count;
}
