#include "scanner.h"
#include "util.h"
#include <tlhelp32.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define SCAN_INTERVAL_MS 1500
#define MSG_QUEUE_CAP 256
#define DEDUP_CAP 2048
#define READ_BUF_SIZE (64 * 1024)
#define MARKER "ON_MESSAGE"
#define MARKER_LEN 10

static HANDLE s_thread;
static volatile LONG s_running;
static CRITICAL_SECTION s_cs;
static int s_cs_init;

static ScannedMsg s_queue[MSG_QUEUE_CAP];
static int s_head, s_tail;

static unsigned s_seen[DEDUP_CAP];
static int s_seen_count;
static int s_seen_idx;
static volatile LONG s_seeded;

static unsigned fnv1a(const char *data, int len) {
        unsigned h = 2166136261u;
        for (int i = 0; i < len; i++) {
                h ^= (unsigned char)data[i];
                h *= 16777619u;
        }
        return h;
}

static int already_seen(const char *text, int len) {
        unsigned h = fnv1a(text, len);
        EnterCriticalSection(&s_cs);
        for (int i = 0; i < s_seen_count && i < DEDUP_CAP; i++) {
                if (s_seen[i] == h) {
                        LeaveCriticalSection(&s_cs);
                        return 1;
                }
        }
        s_seen[s_seen_idx % DEDUP_CAP] = h;
        s_seen_idx++;
        if (s_seen_count < DEDUP_CAP) s_seen_count++;
        LeaveCriticalSection(&s_cs);
        return 0;
}

static void enqueue(const ScannedMsg *msg) {
        EnterCriticalSection(&s_cs);
        int next = (s_head + 1) % MSG_QUEUE_CAP;
        if (next != s_tail) {
                s_queue[s_head] = *msg;
                s_head = next;
        }
        LeaveCriticalSection(&s_cs);
}

static const unsigned char *find_bytes(const unsigned char *hay, size_t hay_len, const unsigned char *needle, size_t needle_len) {
        if (needle_len > hay_len) return NULL;
        size_t limit = hay_len - needle_len;
        for (size_t i = 0; i <= limit; i++) {
                if (hay[i] == needle[0] && memcmp(hay + i, needle, needle_len) == 0)
                        return hay + i;
        }
        return NULL;
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
                        case 'u':
                                if (p + 4 < end) p += 4;
                                break;
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
        const char *args = NULL;
        for (const char *p = data; p + 6 < end; p++) {
                if (memcmp(p, "\"args\"", 6) == 0) {
                        args = p + 6;
                        break;
                }
        }
        if (!args) return 0;
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
        return msg->plain[0] != '\0';
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

static void scan_process(HANDLE proc) {
        unsigned char *buf = (unsigned char *)malloc(READ_BUF_SIZE + MARKER_LEN);
        if (!buf) return;
        MEMORY_BASIC_INFORMATION mbi;
        unsigned char *addr = NULL;
        while (VirtualQueryEx(proc, addr, &mbi, sizeof(mbi)) && is_running()) {
                addr = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
                if (mbi.State != MEM_COMMIT) continue;
                if (mbi.Type == MEM_IMAGE) continue;
                DWORD prot = mbi.Protect & 0xFF;
                if (!(prot == PAGE_READWRITE || prot == PAGE_READONLY || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_READ))
                        continue;
                size_t region_size = mbi.RegionSize;
                unsigned char *base = (unsigned char *)mbi.BaseAddress;
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
                        while (remaining >= MARKER_LEN) {
                                const unsigned char *found = find_bytes(p, remaining, (const unsigned char *)MARKER, MARKER_LEN);
                                if (!found) break;
                                size_t match_off = (size_t)(found - buf);
                                size_t ctx_start = (match_off > 512) ? match_off - 512 : 0;
                                size_t ctx_end = match_off + MARKER_LEN + 2048;
                                if (ctx_end > bytes_read) ctx_end = bytes_read;
                                const char *region = (const char *)(buf + ctx_start);
                                int region_len = (int)(ctx_end - ctx_start);
                                ScannedMsg msg;
                                if (parse_chat_json(region, region_len, &msg)) {
                                        if (!already_seen(msg.plain, (int)strlen(msg.plain))) {
                                                if (InterlockedCompareExchange(&s_seeded, 1, 1))
                                                        enqueue(&msg);
                                        }
                                }
                                size_t advance = (size_t)(found - p) + MARKER_LEN;
                                p += advance;
                                remaining -= advance;
                        }
                        if (to_read == READ_BUF_SIZE && offset + to_read < region_size)
                                offset += to_read - MARKER_LEN;
                        else
                                offset += to_read;
                }
        }
        free(buf);
}

static DWORD WINAPI scanner_thread(LPVOID param) {
        (void)param;
        while (is_running()) {
                DWORD pid = find_fivem_pid();
                if (pid) {
                        HANDLE proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
                        if (proc) {
                                scan_process(proc);
                                CloseHandle(proc);
                                if (!InterlockedCompareExchange(&s_seeded, 1, 1))
                                        InterlockedExchange(&s_seeded, 1);
                        }
                }
                for (int i = 0; i < SCAN_INTERVAL_MS / 100 && is_running(); i++)
                        Sleep(100);
        }
        return 0;
}

int scanner_start(void) {
        if (is_running()) return 1;
        if (!s_cs_init) {
                InitializeCriticalSection(&s_cs);
                s_cs_init = 1;
        }
        s_head = s_tail = 0;
        s_seen_count = s_seen_idx = 0;
        memset(s_seen, 0, sizeof(s_seen));
        InterlockedExchange(&s_seeded, 0);
        InterlockedExchange(&s_running, 1);
        s_thread = CreateThread(NULL, 0, scanner_thread, NULL, 0, NULL);
        if (!s_thread) {
                InterlockedExchange(&s_running, 0);
                return 0;
        }
        return 1;
}

void scanner_stop(void) {
        if (!is_running()) return;
        InterlockedExchange(&s_running, 0);
        WaitForSingleObject(s_thread, 3000);
        CloseHandle(s_thread);
        s_thread = NULL;
}

int scanner_is_running(void) {
        return is_running();
}

int scanner_poll(ScannedMsg *out, int max_count) {
        if (!s_cs_init) return 0;
        int count = 0;
        EnterCriticalSection(&s_cs);
        while (count < max_count && s_tail != s_head) {
                out[count++] = s_queue[s_tail];
                s_tail = (s_tail + 1) % MSG_QUEUE_CAP;
        }
        LeaveCriticalSection(&s_cs);
        return count;
}
