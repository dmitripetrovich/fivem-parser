#include <windows.h>
#include <tlhelp32.h>
#include "parser.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define SCAN_INTERVAL_MS 500
#define MSG_QUEUE_CAP 256
#define DEDUP_CAP 16384
#define DEDUP_MASK (DEDUP_CAP - 1)
#define READ_BUF_SIZE (64 * 1024)
#define NUM_MARKERS 3
static const char *MARKERS[] = { "ON_MESSAGE", "FUNC::CHAT::ADD_MESSAGE", "FUNC::CHAT::ADD_MESSAGES" };
static const int MARKER_LENS[] = { 10, 23, 24 };
#define MIN_MARKER_LEN 10
#define MAX_MARKER_LEN 24
#define MAX_HOTSPOTS 256
#define FULL_SCAN_EVERY 5
#define FAST_INTERVAL_MS 200
#define MAX_REGION_SIZE (32 * 1024 * 1024)
#define SEED_TIMEOUT_MS 3000
#define PASS_DEDUP_CLEAR_MS 10000

typedef unsigned long long u64;

static HANDLE s_thread;
static volatile LONG s_running;
static CRITICAL_SECTION s_cs;
static int s_cs_init;

static ScannedMsg *s_queue;
static int s_head, s_tail;

static u64 s_dedup_hash[DEDUP_CAP];
static int s_dedup_used[DEDUP_CAP];
static int s_dedup_count;
static volatile LONG s_seeded;
static DWORD s_seed_start;
static DWORD s_pass_dedup_time;

static unsigned char *s_hot_bases[MAX_HOTSPOTS];
static int s_hot_count;

typedef struct {
        ScannedMsg msg;
        size_t addr;
} PendingMsg;

#define MAX_PENDING 256
static PendingMsg s_pending[MAX_PENDING];
static int s_pending_count;

#define PASS_DEDUP_CAP 512
#define PASS_DEDUP_MASK (PASS_DEDUP_CAP - 1)

static u64 s_pass_hash[PASS_DEDUP_CAP];
static int s_pass_used[PASS_DEDUP_CAP];
static int s_pass_count;

static void pass_dedup_clear(void) {
        memset(s_pass_used, 0, sizeof(s_pass_used));
        s_pass_count = 0;
}

static int pass_check_and_mark(const char *text, int len) {
        u64 h = 14695981039346656037ULL;
        for (int i = 0; i < len; i++) {
                h ^= (unsigned char)text[i];
                h *= 1099511628211ULL;
        }
        if (h == 0) h = 1;
        unsigned idx = (unsigned)h & PASS_DEDUP_MASK;
        for (;;) {
                if (!s_pass_used[idx]) {
                        if (s_pass_count >= PASS_DEDUP_CAP * 3 / 4) return 0;
                        s_pass_hash[idx] = h;
                        s_pass_used[idx] = 1;
                        s_pass_count++;
                        return 0;
                }
                if (s_pass_hash[idx] == h) return 1;
                idx = (idx + 1) & PASS_DEDUP_MASK;
        }
}

static u64 fnv1a(const char *data, int len, size_t addr) {
        u64 h = 14695981039346656037ULL;
        for (size_t i = 0; i < sizeof(addr); i++) {
                h ^= (unsigned char)(addr >> (i * 8));
                h *= 1099511628211ULL;
        }
        for (int i = 0; i < len; i++) {
                h ^= (unsigned char)data[i];
                h *= 1099511628211ULL;
        }
        return h;
}

static void dedup_clear(void) {
        memset(s_dedup_hash, 0, sizeof(s_dedup_hash));
        memset(s_dedup_used, 0, sizeof(s_dedup_used));
        s_dedup_count = 0;
}

static int is_seen(const char *text, int len, size_t addr) {
        u64 h = fnv1a(text, len, addr);
        if (h == 0) h = 1;
        unsigned idx = (unsigned)h & DEDUP_MASK;
        EnterCriticalSection(&s_cs);
        for (;;) {
                if (!s_dedup_used[idx]) {
                        LeaveCriticalSection(&s_cs);
                        return 0;
                }
                if (s_dedup_hash[idx] == h) {
                        LeaveCriticalSection(&s_cs);
                        return 1;
                }
                idx = (idx + 1) & DEDUP_MASK;
        }
}

static void mark_seen(const char *text, int len, size_t addr) {
        u64 h = fnv1a(text, len, addr);
        if (h == 0) h = 1;
        unsigned idx = (unsigned)h & DEDUP_MASK;
        EnterCriticalSection(&s_cs);
        for (;;) {
                if (!s_dedup_used[idx]) {
                        if (s_dedup_count >= DEDUP_CAP * 3 / 4) {
                                dedup_clear();
                                idx = (unsigned)h & DEDUP_MASK;
                        }
                        s_dedup_hash[idx] = h;
                        s_dedup_used[idx] = 1;
                        s_dedup_count++;
                        LeaveCriticalSection(&s_cs);
                        return;
                }
                if (s_dedup_hash[idx] == h) {
                        LeaveCriticalSection(&s_cs);
                        return;
                }
                idx = (idx + 1) & DEDUP_MASK;
        }
}

static int enqueue(const ScannedMsg *msg) {
        EnterCriticalSection(&s_cs);
        if (!s_queue) {
                LeaveCriticalSection(&s_cs);
                return 0;
        }
        int next = (s_head + 1) % MSG_QUEUE_CAP;
        if (next != s_tail) {
                s_queue[s_head] = *msg;
                s_head = next;
                LeaveCriticalSection(&s_cs);
                return 1;
        }
        LeaveCriticalSection(&s_cs);
        return 0;
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

static const unsigned char *find_any_marker(const unsigned char *hay, size_t hay_len, int *match_len) {
        const unsigned char *earliest = NULL;
        *match_len = 0;
        for (int i = 0; i < NUM_MARKERS; i++) {
                if (hay_len < (size_t)MARKER_LENS[i]) continue;
                const unsigned char *f = find_bytes(hay, hay_len, (const unsigned char *)MARKERS[i], MARKER_LENS[i]);
                if (f && (!earliest || f < earliest)) {
                        earliest = f;
                        *match_len = MARKER_LENS[i];
                }
        }
        return earliest;
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

static int parse_chat_json(const char *data, int data_len, ScannedMsg *msg) {
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
        char arg0[4096] = "";
        const char *next = extract_json_string(args, end, arg0, sizeof(arg0));
        if (!next) return 0;
        char arg1[4096] = "";
        int has_two = 0;
        while (next < end && (*next == ' ' || *next == ','))
                next++;
        if (next < end && *next == '"') {
                if (extract_json_string(next, end, arg1, sizeof(arg1)))
                        has_two = 1;
        }
        if (has_two && arg0[0])
                snprintf(msg->raw, sizeof(msg->raw), "%s: %s", arg0, arg1);
        else if (has_two)
                snprintf(msg->raw, sizeof(msg->raw), "%s", arg1);
        else
                snprintf(msg->raw, sizeof(msg->raw), "%s", arg0);
        snprintf(msg->plain, sizeof(msg->plain), "%s", msg->raw);
        strip_color_codes(msg->plain);
        for (int i = 0; msg->plain[i]; i++)
                if ((msg->plain[i] >= 'A' && msg->plain[i] <= 'Z') || (msg->plain[i] >= 'a' && msg->plain[i] <= 'z') || (msg->plain[i] >= '0' && msg->plain[i] <= '9'))
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
        if (s_hot_count < MAX_HOTSPOTS) {
                s_hot_bases[s_hot_count] = (unsigned char *)base;
                s_hot_count++;
        }
}

static int scan_one_region(HANDLE proc, unsigned char *base, size_t region_size, unsigned char *buf) {
        int found = 0;
        size_t offset = 0;
        while (offset < region_size && is_running()) {
                size_t to_read = region_size - offset;
                if (to_read > READ_BUF_SIZE) to_read = READ_BUF_SIZE;
                SIZE_T bytes_read = 0;
                if (!ReadProcessMemory(proc, base + offset, buf, to_read, &bytes_read) ||
                    bytes_read == 0)
                        break;
                const unsigned char *p = buf;
                size_t remaining = bytes_read;
                int mlen;
                while (remaining >= MIN_MARKER_LEN) {
                        const unsigned char *f = find_any_marker(p, remaining, &mlen);
                        if (!f) break;
                        found = 1;
                        size_t match_off = (size_t)(f - buf);
                        size_t ctx_end = match_off + mlen + 2048;
                        if (ctx_end > bytes_read) ctx_end = bytes_read;
                        const char *region = (const char *)(buf + match_off);
                        int region_len = (int)(ctx_end - match_off);
                        ScannedMsg msg;
                        if (parse_chat_json(region, region_len, &msg)) {
                                int plen = (int)strlen(msg.plain);
                                size_t marker_addr = (size_t)(base + offset) + (size_t)(f - buf);
                                if (!is_seen(msg.plain, plen, marker_addr)) {
                                        mark_seen(msg.plain, plen, marker_addr);
                                        int seeded = InterlockedCompareExchange(&s_seeded, 1, 1);
                                        if (!seeded && GetTickCount() - s_seed_start > SEED_TIMEOUT_MS) {
                                                InterlockedExchange(&s_seeded, 1);
                                                seeded = 1;
                                        }
                                        int text_dup = pass_check_and_mark(msg.plain, plen);
                                        if (seeded && !text_dup && s_pending_count < MAX_PENDING) {
                                                s_pending[s_pending_count].msg = msg;
                                                s_pending[s_pending_count].addr = marker_addr;
                                                s_pending_count++;
                                        }
                                }
                        }
                        size_t advance = (size_t)(f - p) + mlen;
                        p += advance;
                        remaining -= advance;
                }
                if (to_read == READ_BUF_SIZE && offset + to_read < region_size)
                        offset += to_read - MAX_MARKER_LEN;
                else
                        offset += to_read;
        }
        return found;
}

static void scan_process(HANDLE proc, unsigned char *buf) {
        MEMORY_BASIC_INFORMATION mbi;
        unsigned char *addr = NULL;
        while (VirtualQueryEx(proc, addr, &mbi, sizeof(mbi)) && is_running()) {
                addr = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
                if (mbi.State != MEM_COMMIT) continue;
                if (mbi.Type == MEM_IMAGE) continue;
                if (mbi.RegionSize > MAX_REGION_SIZE) continue;
                DWORD prot = mbi.Protect & 0xFF;
                if (!(prot == PAGE_READWRITE || prot == PAGE_READONLY || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_READ))
                        continue;
                if (scan_one_region(proc, (unsigned char *)mbi.BaseAddress, mbi.RegionSize, buf))
                        record_hotspot(mbi.BaseAddress);
        }
}

static void scan_cached(HANDLE proc, unsigned char *buf) {
        if (s_hot_count == 0) return;
        for (int i = 0; i < s_hot_count && is_running(); i++) {
                MEMORY_BASIC_INFORMATION mbi;
                if (!VirtualQueryEx(proc, s_hot_bases[i], &mbi, sizeof(mbi)))
                        continue;
                if (mbi.State != MEM_COMMIT)
                        continue;
                size_t sz = mbi.RegionSize > MAX_REGION_SIZE ? MAX_REGION_SIZE : mbi.RegionSize;
                scan_one_region(proc, (unsigned char *)mbi.BaseAddress, sz, buf);
        }
}

static int pending_cmp(const void *a, const void *b) {
        size_t aa = ((const PendingMsg *)a)->addr;
        size_t bb = ((const PendingMsg *)b)->addr;
        return (aa > bb) - (aa < bb);
}

static void flush_pending(void) {
        if (s_pending_count == 0) return;
        qsort(s_pending, s_pending_count, sizeof(PendingMsg), pending_cmp);
        for (int i = 0; i < s_pending_count; i++)
                enqueue(&s_pending[i].msg);
        s_pending_count = 0;
}

static DWORD WINAPI scanner_thread(LPVOID param) {
        (void)param;
        unsigned char *buf = (unsigned char *)malloc(READ_BUF_SIZE + MAX_MARKER_LEN);
        if (!buf) return 1;
        DWORD cached_pid = 0;
        HANDLE cached_proc = NULL;
        int pass = 0;
        while (is_running()) {
                DWORD pid = 0;
                if (cached_proc) {
                        DWORD exit_code = 0;
                        if (GetExitCodeProcess(cached_proc, &exit_code) && exit_code == STILL_ACTIVE) {
                                pid = cached_pid;
                        } else {
                                CloseHandle(cached_proc);
                                cached_proc = NULL;
                                cached_pid = 0;
                        }
                }
                if (!cached_proc) {
                        pid = find_fivem_pid();
                        if (pid) {
                                cached_proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
                                if (cached_proc)
                                        cached_pid = pid;
                        }
                }
                if (cached_proc) {
                        if (pass > 1 && GetTickCount() - s_pass_dedup_time > PASS_DEDUP_CLEAR_MS) {
                                pass_dedup_clear();
                                s_pass_dedup_time = GetTickCount();
                        }
                        s_pending_count = 0;
                        if (pass == 0 || s_hot_count == 0 || pass % FULL_SCAN_EVERY == 0)
                                scan_process(cached_proc, buf);
                        else
                                scan_cached(cached_proc, buf);
                        flush_pending();
                        if (!InterlockedCompareExchange(&s_seeded, 1, 1)) {
                                InterlockedExchange(&s_seeded, 1);
                                pass_dedup_clear();
                                s_pass_dedup_time = GetTickCount();
                        }
                        pass++;
                }
                int interval = (s_hot_count > 0 && pass > 1) ? FAST_INTERVAL_MS : SCAN_INTERVAL_MS;
                for (int i = 0; i < interval / 100 && is_running(); i++)
                        Sleep(100);
        }
        if (cached_proc)
                CloseHandle(cached_proc);
        free(buf);
        return 0;
}

int scanner_start(void) {
        if (!s_cs_init) {
                InitializeCriticalSection(&s_cs);
                s_cs_init = 1;
        }
        if (s_thread) {
                InterlockedExchange(&s_running, 0);
                WaitForSingleObject(s_thread, 5000);
                CloseHandle(s_thread);
                s_thread = NULL;
        }
        free(s_queue);
        s_queue = (ScannedMsg *)calloc(MSG_QUEUE_CAP, sizeof(ScannedMsg));
        if (!s_queue) return 0;
        s_head = s_tail = 0;
        dedup_clear();
        pass_dedup_clear();
        s_hot_count = 0;
        s_seed_start = GetTickCount();
        s_pass_dedup_time = s_seed_start;
        InterlockedExchange(&s_seeded, 0);
        InterlockedExchange(&s_running, 1);
        s_thread = CreateThread(NULL, 0, scanner_thread, NULL, 0, NULL);
        if (!s_thread) {
                InterlockedExchange(&s_running, 0);
                free(s_queue);
                s_queue = NULL;
                return 0;
        }
        return 1;
}

void scanner_stop(void) {
        if (!s_thread) return;
        InterlockedExchange(&s_running, 0);
        WaitForSingleObject(s_thread, 5000);
        CloseHandle(s_thread);
        s_thread = NULL;
        free(s_queue);
        s_queue = NULL;
}

int scanner_is_running(void) {
        return is_running();
}

int scanner_poll(ScannedMsg *out, int max_count) {
        if (!s_cs_init) return 0;
        int count = 0;
        EnterCriticalSection(&s_cs);
        while (count < max_count && s_queue && s_tail != s_head) {
                out[count++] = s_queue[s_tail];
                s_tail = (s_tail + 1) % MSG_QUEUE_CAP;
        }
        LeaveCriticalSection(&s_cs);
        return count;
}
