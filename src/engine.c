// Shakedown: runs a real all-core load and says which CPU cores failed, when, and how hot they were.
// This file is the engine, included by window.c (shakedown.exe) and cli.c (shakedown-cli.exe). See README.md.
//
// Parts: topology (which logical CPUs belong to which core), sensors (per-core clock, temperature, throttling),
// the Windows hardware error log (WHEA) matched to cores, the load runner, and the verdict.
//
// Sensors need admin rights (PawnIO's IntelMSR module). To avoid a prompt on every run, `shakedown install` makes an
// on-demand task that runs this same program elevated as a helper. The helper only reads sensors and writes them to a
// file; it takes no commands and starts nothing. The load itself always runs with normal rights.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <winevt.h>
#include <pdh.h>
#include <shellapi.h>
#include <cpuid.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>

#define VERSION    "0.2.0"
#define MAX_CPU    64
#define MAX_WHEA   1024
#define TASK_NAME  L"Shakedown"
#define ALIVE_NAME L"Local\\ShakedownAlive"
#define EVERY_MS   2000
// Status bits of IA32_THERM_STATUS / IA32_PACKAGE_THERM_STATUS that mean the clock is being held back right now.
#define F_THERMAL  0x0001
#define F_PROCHOT  0x0004
#define F_POWER    0x0400
#define F_CURRENT  0x1000
#define F_ANY      (F_THERMAL | F_PROCHOT | F_POWER | F_CURRENT)

// What the window shows while a run is going (and between runs). Filled by the engine every sample.
enum { PH_IDLE, PH_BEFORE, PH_START, PH_LOAD, PH_AFTER };
typedef struct { int mhz, temp, util, flags, errors, firstError; } LiveCore;
typedef struct {
    int phase, loadSeconds, wanted, busy, pkgWatts, pkgTemp, tjmax, errors, round;
    double coolant;
    ULONG64 held;
    LiveCore core[64];
} Live;
static void (*onLive)(const Live *); // set by the window
static BOOL quiet;                   // no output to stdout (the window has none)
static char engineError[200];        // why the last command could not start

static void fail(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(engineError, sizeof(engineError), format, args);
    va_end(args);
    if (!quiet) fprintf(stderr, "%s\n", engineError);
}

static void say(const char *format, ...) {
    va_list args;
    if (quiet) return;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    fflush(stdout);
}

// ---- Topology ----

typedef struct { char name[8]; int perf, firstCpu; ULONG64 mask; } Core;
static Core cores[MAX_CPU];
static int coreCount, cpuCount, pCount;
static int cpuCore[MAX_CPU];
static unsigned cpuApic[MAX_CPU];
static ULONG64 allMask;

static BOOL topology(void) {
    DWORD size = 0;
    int top = 0, e = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &size);
    BYTE *buf = malloc(size);
    if (!buf || !GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &size)) return FALSE;
    for (DWORD at = 0; at < size && coreCount < MAX_CPU;) {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX r = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + at);
        at += r->Size;
        if (r->Relationship != RelationProcessorCore || r->Processor.GroupMask[0].Group != 0) continue; // first 64 CPUs only
        Core c = {"", r->Processor.EfficiencyClass, 0, r->Processor.GroupMask[0].Mask};
        while (!(c.mask >> c.firstCpu & 1)) c.firstCpu++;
        int i = coreCount++;
        for (; i > 0 && cores[i - 1].firstCpu > c.firstCpu; i--) cores[i] = cores[i - 1];
        cores[i] = c;
        if (c.perf > top) top = c.perf;
    }
    free(buf);
    for (int i = 0; i < coreCount; i++) {
        cores[i].perf = cores[i].perf == top;
        if (cores[i].perf) sprintf(cores[i].name, "P%d", pCount++);
        else sprintf(cores[i].name, "E%d", e++);
        allMask |= cores[i].mask;
        for (int cpu = 0; cpu < MAX_CPU; cpu++)
            if (cores[i].mask >> cpu & 1) {
                cpuCore[cpu] = i;
                if (cpu + 1 > cpuCount) cpuCount = cpu + 1;
            }
    }
    // The APIC ID is how the hardware error log names a logical CPU.
    HANDLE me = GetCurrentThread();
    DWORD_PTR old = SetThreadAffinityMask(me, 1);
    for (int cpu = 0; cpu < cpuCount; cpu++) {
        unsigned a, b, c, d;
        if (!(allMask >> cpu & 1) || !SetThreadAffinityMask(me, (DWORD_PTR)1 << cpu)) continue;
        Sleep(0);
        __cpuid(0, a, b, c, d);
        if (a >= 0x0B) {
            __cpuid_count(0x0B, 0, a, b, c, d);
            cpuApic[cpu] = d;
        } else {
            __cpuid(1, a, b, c, d);
            cpuApic[cpu] = b >> 24;
        }
    }
    SetThreadAffinityMask(me, old ? old : (DWORD_PTR)allMask);
    return coreCount > 0;
}

static int coreByName(const char *s) {
    for (int i = 0; i < coreCount; i++)
        if (!_stricmp(cores[i].name, s)) return i;
    return -1;
}

// "2,4" or "P2,P4,E3" into a bit per core. A bare number is a P-core.
static ULONG64 parseCores(const char *list) {
    char copy[256], name[16], *tok;
    ULONG64 set = 0;
    strncpy(copy, list, sizeof(copy) - 1), copy[sizeof(copy) - 1] = 0;
    for (tok = strtok(copy, ", "); tok; tok = strtok(NULL, ", ")) {
        if (*tok >= '0' && *tok <= '9') sprintf(name, "P%.6s", tok);
        else sprintf(name, "%.7s", tok);
        int c = coreByName(name);
        if (c < 0) {
            fprintf(stderr, "No core called %s on this CPU.\n", tok);
            exit(2);
        }
        set |= (ULONG64)1 << c;
    }
    return set;
}

static void coreList(ULONG64 set, char *out) {
    *out = 0;
    for (int i = 0; i < coreCount; i++)
        if (set >> i & 1) sprintf(out + strlen(out), "%s%s", *out ? " " : "", cores[i].name);
    if (!*out) strcpy(out, "-");
}

// ---- Paths and settings ----

static WCHAR exeDir[MAX_PATH], iniPath[MAX_PATH], dataDir[MAX_PATH];

static void paths(void) {
    DWORD n = GetModuleFileNameW(NULL, exeDir, MAX_PATH);
    while (n && exeDir[n - 1] != L'\\') n--;
    exeDir[n] = 0;
    swprintf(iniPath, MAX_PATH, L"%lsshakedown.ini", exeDir);
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Shakedown", dataDir, MAX_PATH);
    CreateDirectoryW(dataDir, NULL);
}

static void setting(LPCWSTR section, LPCWSTR key, LPCWSTR fallback, LPWSTR out, int cap) {
    WCHAR raw[1024];
    GetPrivateProfileStringW(section, key, fallback, raw, 1024, iniPath);
    ExpandEnvironmentStringsW(raw, out, cap);
}

static BOOL isElevated(void) {
    HANDLE t;
    TOKEN_ELEVATION e = {0};
    DWORD n;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return FALSE;
    GetTokenInformation(t, TokenElevation, &e, sizeof(e), &n);
    CloseHandle(t);
    return e.TokenIsElevated != 0;
}

static int runHidden(LPWSTR cmd, DWORD waitMs) {
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return -1;
    if (WaitForSingleObject(pi.hProcess, waitMs) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

// ---- Sensors ----

typedef struct { short temp, flags; int mhz, c0; } CpuSense; // c0 in tenths of a percent
typedef struct {
    DWORD seq, tick;
    int n, tjmax, pkgTemp, pkgFlags, pkgWatts;
    CpuSense cpu[MAX_CPU];
    DWORD seq2;
} Snapshot;

enum { SRC_NONE, SRC_DIRECT, SRC_HELPER, SRC_COUNTERS };
static int source;
static const char *const sourceName[] = {"none", "direct", "helper", "Windows counters (clock only: run `shakedown install` for temperatures)"};
static int baseMhz;

typedef HRESULT(WINAPI *PioOpen)(PHANDLE);
typedef HRESULT(WINAPI *PioLoad)(HANDLE, const UCHAR *, SIZE_T);
typedef HRESULT(WINAPI *PioExec)(HANDLE, PCSTR, const ULONG64 *, SIZE_T, PULONG64, SIZE_T, PSIZE_T);
static PioExec pioExec;
static HANDLE pio;

static BOOL msrOpen(void) {
    WCHAR path[MAX_PATH], module[MAX_PATH], fallback[MAX_PATH];
    ExpandEnvironmentStringsW(L"%ProgramFiles%\\PawnIO\\PawnIOLib.dll", path, MAX_PATH);
    HMODULE lib = LoadLibraryW(path);
    if (!lib) return FALSE;
    PioOpen pOpen = (PioOpen)GetProcAddress(lib, "pawnio_open");
    PioLoad pLoad = (PioLoad)GetProcAddress(lib, "pawnio_load");
    pioExec = (PioExec)GetProcAddress(lib, "pawnio_execute");
    swprintf(fallback, MAX_PATH, L"%lsIntelMSR.bin", exeDir);
    setting(L"sensors", L"module", fallback, module, MAX_PATH);
    HANDLE f = CreateFileW(module, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (!pOpen || !pLoad || !pioExec || f == INVALID_HANDLE_VALUE) return FALSE;
    DWORD size = GetFileSize(f, NULL), got = 0;
    UCHAR *blob = malloc(size);
    BOOL ok = blob && ReadFile(f, blob, size, &got, NULL) && got == size;
    CloseHandle(f);
    ok = ok && pOpen(&pio) == S_OK && pLoad(pio, blob, size) == S_OK;
    free(blob);
    return ok;
}

static BOOL msr(DWORD reg, ULONG64 *v) {
    ULONG64 in = reg;
    SIZE_T n;
    *v = 0;
    return pioExec(pio, "ioctl_read_msr", &in, 1, v, 1, &n) == S_OK;
}

// One reading of every logical CPU. Clock comes from APERF/MPERF, so it is the clock the core really ran at while
// it was running, and c0 is the share of time it was running at all.
static void msrSample(Snapshot *s) {
    static ULONG64 lastA[MAX_CPU], lastM[MAX_CPU], lastEnergy;
    static LARGE_INTEGER lastTime, freq;
    static double joules;
    static int tjmax;
    LARGE_INTEGER now;
    ULONG64 v, a, m;
    HANDLE me = GetCurrentThread();
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        SetThreadAffinityMask(me, (DWORD_PTR)1 << cores[0].firstCpu);
        tjmax = msr(0x1A2, &v) && (v >> 16 & 0xFF) ? (int)(v >> 16 & 0xFF) : 100;
        joules = msr(0x606, &v) ? 1.0 / (double)(1u << (v >> 8 & 0x1F)) : 0;
    }
    QueryPerformanceCounter(&now);
    double dt = lastTime.QuadPart ? (double)(now.QuadPart - lastTime.QuadPart) / (double)freq.QuadPart : 0;
    s->n = cpuCount, s->tjmax = tjmax;
    for (int cpu = 0; cpu < cpuCount; cpu++) {
        CpuSense *c = &s->cpu[cpu];
        c->temp = -1, c->flags = 0, c->mhz = 0, c->c0 = 0;
        if (!(allMask >> cpu & 1) || !SetThreadAffinityMask(me, (DWORD_PTR)1 << cpu)) continue;
        Sleep(0);
        if (msr(0x19C, &v) && (v >> 31 & 1)) c->temp = (short)(tjmax - (int)(v >> 16 & 0x7F)), c->flags = (short)(v & F_ANY);
        if (msr(0xE8, &a) && msr(0xE7, &m)) {
            if (dt > 0 && m > lastM[cpu]) {
                c->mhz = (int)((double)baseMhz * (double)(a - lastA[cpu]) / (double)(m - lastM[cpu]) + 0.5);
                c->c0 = (int)((double)(m - lastM[cpu]) / (dt * baseMhz * 1e6) * 1000 + 0.5);
            }
            lastA[cpu] = a, lastM[cpu] = m;
        }
    }
    SetThreadAffinityMask(me, (DWORD_PTR)1 << cores[0].firstCpu);
    s->pkgTemp = -1, s->pkgFlags = 0, s->pkgWatts = 0;
    if (msr(0x1B1, &v) && (v >> 31 & 1)) s->pkgTemp = tjmax - (int)(v >> 16 & 0x7F), s->pkgFlags = (int)(v & F_ANY);
    if (joules > 0 && msr(0x611, &v)) {
        v &= 0xFFFFFFFF;
        if (dt > 0) s->pkgWatts = (int)((double)((v - lastEnergy) & 0xFFFFFFFF) * joules / dt + 0.5);
        lastEnergy = v;
    }
    SetThreadAffinityMask(me, (DWORD_PTR)allMask);
    lastTime = now;
    s->tick = GetTickCount();
}

static void snapshotPath(LPWSTR out) { swprintf(out, MAX_PATH, L"%ls\\sensors.dat", dataDir); }

static BOOL helperRead(Snapshot *s) {
    WCHAR path[MAX_PATH];
    DWORD got;
    snapshotPath(path);
    for (int attempt = 0; attempt < 3; attempt++) {
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) return FALSE;
        BOOL ok = ReadFile(f, s, sizeof(*s), &got, NULL) && got == sizeof(*s) && s->seq == s->seq2;
        CloseHandle(f);
        if (ok) return GetTickCount() - s->tick < 5000;
        Sleep(20);
    }
    return FALSE;
}

// The elevated helper: sensors to a file once a second, for as long as a run holds the "alive" event.
static int helperMain(void) {
    WCHAR path[MAX_PATH];
    Snapshot s = {0};
    DWORD wrote, started = GetTickCount(), lastSeen = started;
    if (!msrOpen()) return 3;
    snapshotPath(path);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 4;
    while (GetTickCount() - started < 12 * 3600 * 1000u) {
        HANDLE alive = OpenEventW(SYNCHRONIZE, FALSE, ALIVE_NAME);
        if (alive) CloseHandle(alive), lastSeen = GetTickCount();
        else if (GetTickCount() - lastSeen > 15000) break;
        msrSample(&s);
        s.seq2 = ++s.seq;
        SetFilePointer(f, 0, NULL, FILE_BEGIN);
        WriteFile(f, &s, sizeof(s), &wrote, NULL);
        Sleep(1000);
    }
    CloseHandle(f);
    DeleteFileW(path);
    return 0;
}

// Asked through schtasks: the task registry is not readable without admin rights.
static BOOL taskExists(void) {
    WCHAR cmd[96] = L"schtasks.exe /Query /TN " TASK_NAME;
    return runHidden(cmd, 10000) == 0;
}

// Windows' own counters, for when there is no admin path: the clock of each logical CPU, nothing else.
static PDH_HQUERY pdhQuery;
static PDH_HCOUNTER pdhPerf;

static void countersSample(Snapshot *s) {
    static BYTE buf[32768];
    DWORD size = sizeof(buf), count = 0;
    PPDH_FMT_COUNTERVALUE_ITEM_W items = (PPDH_FMT_COUNTERVALUE_ITEM_W)buf;
    s->n = cpuCount, s->pkgTemp = -1, s->pkgWatts = 0, s->pkgFlags = 0, s->tick = GetTickCount();
    for (int i = 0; i < MAX_CPU; i++) s->cpu[i].temp = -1, s->cpu[i].flags = 0, s->cpu[i].mhz = 0, s->cpu[i].c0 = -1;
    if (PdhCollectQueryData(pdhQuery) != ERROR_SUCCESS) return;
    if (PdhGetFormattedCounterArrayW(pdhPerf, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS) return;
    for (DWORD i = 0; i < count; i++) {
        LPCWSTR comma = wcschr(items[i].szName, L',');
        if (!comma || items[i].szName[0] != L'0' || comma[1] < L'0' || comma[1] > L'9') continue; // "0,7"; skips _Total
        int cpu = _wtoi(comma + 1);
        if (cpu < MAX_CPU) s->cpu[cpu].mhz = (int)(items[i].FmtValue.doubleValue * baseMhz / 100 + 0.5);
    }
}

static HANDLE aliveEvent;

static void sensorsOpen(void) {
    DWORD n = sizeof(DWORD), mhz = 0;
    Snapshot s;
    RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"~MHz", RRF_RT_REG_DWORD, NULL, &mhz, &n);
    baseMhz = (int)mhz;
    if (isElevated() && msrOpen()) {
        source = SRC_DIRECT;
        msrSample(&s);
        return;
    }
    if (taskExists()) {
        WCHAR cmd[128] = L"schtasks.exe /Run /TN " TASK_NAME;
        aliveEvent = CreateEventW(NULL, TRUE, FALSE, ALIVE_NAME);
        if (helperRead(&s) || runHidden(cmd, 10000) == 0) {
            for (int i = 0; i < 40 && source == SRC_NONE; i++) {
                if (helperRead(&s)) source = SRC_HELPER;
                else Sleep(250);
            }
        }
        if (source == SRC_HELPER) return;
    }
    if (PdhOpenQueryW(NULL, 0, &pdhQuery) == ERROR_SUCCESS &&
        PdhAddEnglishCounterW(pdhQuery, L"\\Processor Information(*)\\% Processor Performance", 0, &pdhPerf) == ERROR_SUCCESS) {
        PdhCollectQueryData(pdhQuery);
        source = SRC_COUNTERS;
    }
}

static void sensorsRead(Snapshot *s) {
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < MAX_CPU; i++) s->cpu[i].temp = -1, s->cpu[i].c0 = -1;
    s->pkgTemp = -1;
    if (source == SRC_DIRECT) msrSample(s);
    else if (source == SRC_HELPER) helperRead(s);
    else if (source == SRC_COUNTERS) countersSample(s);
}

// How busy each logical CPU was since the last call, 0..100. Needs no rights.
static void utilRead(int *util) {
    static SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION last[MAX_CPU], now[MAX_CPU];
    static NTSTATUS(NTAPI * query)(ULONG, PVOID, ULONG, PULONG);
    ULONG got;
    if (!query) query = (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
    if (!query || query(8, now, sizeof(now[0]) * cpuCount, &got) != 0) return;
    for (int i = 0; i < cpuCount; i++) {
        LONGLONG total = (now[i].KernelTime.QuadPart + now[i].UserTime.QuadPart) - (last[i].KernelTime.QuadPart + last[i].UserTime.QuadPart);
        LONGLONG idle = now[i].IdleTime.QuadPart - last[i].IdleTime.QuadPart;
        util[i] = total > 0 ? (int)((total - idle) * 100 / total) : 0;
        last[i] = now[i];
    }
}

// A temperature some other program publishes in the registry (Blackout's water temperature, for one).
static double coolantRead(void) {
    static WCHAR key[256], value[64];
    static int divide = -1;
    DWORD v, n = sizeof(v);
    if (divide < 0) {
        setting(L"coolant", L"key", L"Software\\Blackout", key, 256);
        setting(L"coolant", L"value", L"Coolant", value, 64);
        divide = GetPrivateProfileIntW(L"coolant", L"divide", 10, iniPath);
        if (divide < 1) divide = 1;
    }
    if (RegGetValueW(HKEY_CURRENT_USER, key, value, RRF_RT_REG_DWORD, NULL, &v, &n) != ERROR_SUCCESS || !v) return -1;
    return (double)v / divide;
}

// ---- Hardware error log ----

typedef struct { ULONG64 time; int id, cpu, core, bank; ULONG64 status; } Whea; // time as FILETIME, UTC
static Whea whea[MAX_WHEA];
static int wheaCount;

static ULONG64 nowFiletime(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (ULONG64)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
}

static const char *wheaKind(const Whea *w) {
    unsigned code = (unsigned)(w->status & 0xFFFF);
    if (w->core < 0) return "other";
    if (code == 0x0005) return "parity";
    if ((code & 0xFFF0) == 0x0010) return "TLB";
    if ((code & 0xFF00) == 0x0100) return "cache";
    if (code & 0x0800) return "bus";
    return "internal";
}

static ULONG64 xmlNumber(LPCWSTR xml, LPCWSTR name) {
    LPCWSTR at = wcsstr(xml, name);
    if (!at || !(at = wcschr(at, L'>'))) return (ULONG64)-1;
    return wcstoull(at + 1, NULL, 0);
}

// Adds every WHEA-Logger record since `since` that has not been seen yet.
static void wheaPoll(ULONG64 since) {
    static WCHAR xml[16384];
    WCHAR query[256];
    SYSTEMTIME st;
    FILETIME ft = {(DWORD)since, (DWORD)(since >> 32)};
    EVT_HANDLE batch[16];
    DWORD got, used, props;
    int index = 0;
    FileTimeToSystemTime(&ft, &st);
    swprintf(query, 256, L"*[System[Provider[@Name='Microsoft-Windows-WHEA-Logger'] and TimeCreated[@SystemTime>='%04d-%02d-%02dT%02d:%02d:%02d.000Z']]]",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    EVT_HANDLE q = EvtQuery(NULL, L"System", query, EvtQueryChannelPath | EvtQueryForwardDirection);
    if (!q) return;
    while (EvtNext(q, 16, batch, 1000, 0, &got)) {
        for (DWORD i = 0; i < got; i++, index++) {
            if (index >= wheaCount && wheaCount < MAX_WHEA && EvtRender(NULL, batch[i], EvtRenderEventXml, sizeof(xml), xml, &used, &props)) {
                Whea *w = &whea[wheaCount++];
                LPCWSTR when = wcsstr(xml, L"SystemTime=");
                ULONG64 apic = xmlNumber(xml, L"'ApicId'");
                memset(&st, 0, sizeof(st));
                if (when) swscanf(when + 12, L"%hu-%hu-%huT%hu:%hu:%hu", &st.wYear, &st.wMonth, &st.wDay, &st.wHour, &st.wMinute, &st.wSecond);
                SystemTimeToFileTime(&st, &ft);
                w->time = (ULONG64)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
                w->id = (int)xmlNumber(xml, L"<EventID");
                w->bank = (int)xmlNumber(xml, L"'MCABank'");
                w->status = xmlNumber(xml, L"'MciStat'");
                w->cpu = w->core = -1;
                for (int cpu = 0; cpu < cpuCount && apic != (ULONG64)-1; cpu++)
                    if ((allMask >> cpu & 1) && cpuApic[cpu] == (unsigned)apic) w->cpu = cpu, w->core = cpuCore[cpu];
            }
            EvtClose(batch[i]);
        }
    }
    EvtClose(q);
}

// ---- One run ----

typedef struct {
    int minutes, soak, maxRuns, round;
    ULONG64 exclude;
    char label[48], note[160];
} Options;

typedef struct {
    int n, mhzMin, tempMax, flags, errors, firstError; // firstError: seconds into the load, -1 = none
    double mhzSum, utilSum;
    int utilN;
} CoreStat;

typedef struct {
    BOOL pass;
    char reason[96], score[32];
    int seconds, wanted, errors, pkgTempMax, loaded;
    double coolantStart, coolantMax, wattSum;
    ULONG64 failed; // cores named by the error log
    CoreStat core[MAX_CPU];
} Result;

static volatile LONG stopRequested;
static BOOL WINAPI onCtrl(DWORD type) {
    InterlockedExchange(&stopRequested, 1);
    return type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT;
}

static BOOL crashFileNewer(LPCWSTR pattern, ULONG64 since) {
    WIN32_FIND_DATAW fd;
    WCHAR dir[MAX_PATH], *star, *slash;
    BOOL found = FALSE;
    if (!pattern[0]) return FALSE;
    // One wildcard may sit in a folder name (Maxon puts a hash there), so walk the parent first.
    wcsncpy(dir, pattern, MAX_PATH - 1), dir[MAX_PATH - 1] = 0;
    star = wcschr(dir, L'*');
    slash = star ? wcschr(star, L'\\') : NULL;
    if (!slash) {
        HANDLE h = FindFirstFileW(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) return FALSE;
        do found |= ((ULONG64)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime) > since;
        while (!found && FindNextFileW(h, &fd));
        FindClose(h);
        return found;
    }
    WCHAR rest[MAX_PATH], parent[MAX_PATH], next[MAX_PATH];
    wcscpy(rest, slash);
    *slash = 0;
    HANDLE h = FindFirstFileW(dir, &fd);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    wcscpy(parent, dir);
    WCHAR *cut = wcsrchr(parent, L'\\');
    if (cut) cut[1] = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        swprintf(next, MAX_PATH, L"%ls%ls%ls", parent, fd.cFileName, rest);
        found = crashFileNewer(next, since);
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

static void stamp(char *out, int cap) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    snprintf(out, cap, "%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

static void clock8(char *out) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    sprintf(out, "%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
}

static void both(FILE *f, const char *line) {
    if (!quiet) fputs(line, stdout), fflush(stdout);
    if (f) fputs(line, f), fflush(f);
}

static void verdict(const Options *o, const Result *r, const char *name, FILE *out);

static BOOL runOnce(const Options *o, Result *r, char *name) {
    WCHAR command[2048], raw[2048], dir[MAX_PATH], crash[MAX_PATH], prefix[64], path[MAX_PATH], seconds[16], *hole;
    char line[4096], time8[16], when[32], label[64];
    int util[MAX_CPU] = {0}, limit = GetPrivateProfileIntW(L"limits", L"coolant", 45, iniPath), elapsed = 0, loadTick = 0, post = 0;
    ULONG64 started = nowFiletime() - 10000000, loadStarted = 0, include = allMask;
    PROCESS_INFORMATION pi = {0};
    STARTUPINFOW si = {sizeof(si)};
    SECURITY_ATTRIBUTES inherit = {sizeof(inherit), NULL, TRUE};
    Snapshot s;
    DWORD code = 0, t0 = GetTickCount();
    BOOL running = FALSE, crashed = FALSE, aborted = FALSE, begun = FALSE;
    FILE *csv, *ledger;
    // The load and anything it starts live in one job: they end together, and cannot leave the cores they are given.
    HANDLE job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimit = {0};

    memset(r, 0, sizeof(*r));
    r->coolantStart = -1, r->coolantMax = -1, r->pkgTempMax = -1;
    r->wanted = o->minutes * 60;
    for (int i = 0; i < coreCount; i++) {
        r->core[i].firstError = -1, r->core[i].tempMax = -1;
        if (o->exclude >> i & 1) include &= ~cores[i].mask;
    }
    setting(L"load", L"command", L"", raw, 2048);
    setting(L"load", L"dir", L"", dir, MAX_PATH);
    setting(L"load", L"crashfile", L"", crash, MAX_PATH);
    setting(L"load", L"score", L"", prefix, 64);
    if (!raw[0]) {
        fail("No load is set. Put the program to run in shakedown.ini under [load] command=.");
        return FALSE;
    }
    swprintf(seconds, 16, L"%d", o->minutes ? o->minutes * 60 : 1);
    if ((hole = wcsstr(raw, L"{seconds}"))) {
        *hole = 0;
        swprintf(command, 2048, L"%ls%ls%ls", raw, seconds, hole + 9);
    } else {
        wcscpy(command, raw);
    }

    stamp(when, sizeof(when));
    snprintf(label, sizeof(label), "%s", o->label[0] ? o->label : "run");
    snprintf(name, 96, "%s-%s", when, label);
    swprintf(path, MAX_PATH, L"%lsruns", exeDir);
    CreateDirectoryW(path, NULL);
    swprintf(path, MAX_PATH, L"%lsruns\\%hs.csv", exeDir, name);
    csv = _wfopen(path, L"w");
    swprintf(path, MAX_PATH, L"%lsruns\\%hs.load.txt", exeDir, name);
    HANDLE loadOut = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, 0, NULL);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, NULL);

    // Heat-soak: wait for the water to reach the asked temperature before starting.
    if (o->soak > 0) {
        double c = coolantRead();
        if (c < 0) {
            fail("--soak needs a water temperature (see [coolant] in shakedown.ini).");
            return FALSE;
        }
        for (int waited = 0; c < o->soak && waited < 3600 && !stopRequested; waited += 5, c = coolantRead()) {
            if (waited % 60 == 0) say("# soak: water %.1f C, waiting for %d C\n", c, o->soak);
            Sleep(5000);
        }
        if (c < o->soak) {
            fail("Water did not reach %d C (now %.1f C). Not started.", o->soak, c);
            return FALSE;
        }
    }

    int n = snprintf(line, sizeof(line), "time,elapsed,phase,util,coolant,pkg_w,pkg_c,errors");
    for (int i = 0; i < coreCount; i++) n += snprintf(line + n, sizeof(line) - n, ",%s_mhz,%s_c,%s_util", cores[i].name, cores[i].name, cores[i].name);
    snprintf(line + n, sizeof(line) - n, "\n");
    both(csv, line);
    utilRead(util);
    sensorsRead(&s);

    for (;;) {
        // Between samples: keep the load on its cores, and see whether it has ended or left a crash report.
        for (int tick = 0; tick < EVERY_MS / 500; tick++) {
            Sleep(500);
            if (!running) continue;
            DWORD_PTR have, sys;
            if (o->exclude && GetProcessAffinityMask(pi.hProcess, &have, &sys) && have != (DWORD_PTR)include) SetProcessAffinityMask(pi.hProcess, (DWORD_PTR)include);
            if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
                GetExitCodeProcess(pi.hProcess, &code);
                running = FALSE;
            } else if (crashFileNewer(crash, loadStarted)) {
                Sleep(2000);
                TerminateJobObject(job, 1), TerminateProcess(pi.hProcess, 1);
                crashed = TRUE, running = FALSE;
            } else if (stopRequested) {
                TerminateJobObject(job, 1), TerminateProcess(pi.hProcess, 1);
                aborted = TRUE, running = FALSE;
                snprintf(r->reason, sizeof(r->reason), "stopped by hand");
            }
            if (!running) r->seconds = (int)((GetTickCount() - (DWORD)loadTick) / 1000);
        }
        elapsed = (int)((GetTickCount() - t0) / 1000);
        utilRead(util);
        sensorsRead(&s);
        wheaPoll(started);
        double coolant = coolantRead();
        if (coolant > r->coolantMax) r->coolantMax = coolant;

        // Start the load after a few idle samples.
        if (!begun && elapsed >= 8) {
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdInput = nul, si.hStdOutput = loadOut, si.hStdError = loadOut;
            // Only the load's own output file goes to it, not this program's output.
            SetHandleInformation(GetStdHandle(STD_OUTPUT_HANDLE), HANDLE_FLAG_INHERIT, 0);
            SetHandleInformation(GetStdHandle(STD_ERROR_HANDLE), HANDLE_FLAG_INHERIT, 0);
            SetHandleInformation(GetStdHandle(STD_INPUT_HANDLE), HANDLE_FLAG_INHERIT, 0);
            if (!CreateProcessW(NULL, command, NULL, NULL, TRUE, CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED, NULL, dir[0] ? dir : NULL, &si, &pi)) {
                fail("Could not start the load (error %lu): %ls", GetLastError(), command);
                return FALSE;
            }
            jobLimit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | (o->exclude ? JOB_OBJECT_LIMIT_AFFINITY : 0);
            jobLimit.BasicLimitInformation.Affinity = (ULONG_PTR)include;
            if (job && SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jobLimit, sizeof(jobLimit))) AssignProcessToJobObject(job, pi.hProcess);
            if (o->exclude) SetProcessAffinityMask(pi.hProcess, (DWORD_PTR)include);
            ResumeThread(pi.hThread);
            begun = running = TRUE;
            loadTick = (int)GetTickCount(), loadStarted = nowFiletime();
            r->coolantStart = coolant;
        }

        int busy = 0, cpus = 0;
        for (int cpu = 0; cpu < cpuCount; cpu++)
            if (include >> cpu & 1) busy += util[cpu], cpus++;
        busy = cpus ? busy / cpus : 0;
        BOOL loaded = running && busy >= 70;
        clock8(time8);
        n = snprintf(line, sizeof(line), "%s,%d,%s,%d,", time8, elapsed, !begun ? "before" : running ? (loaded ? "load" : "start") : "after", busy);
        n += coolant < 0 ? snprintf(line + n, sizeof(line) - n, ",") : snprintf(line + n, sizeof(line) - n, "%.1f,", coolant);
        n += snprintf(line + n, sizeof(line) - n, "%d,", s.pkgWatts);
        n += s.pkgTemp < 0 ? snprintf(line + n, sizeof(line) - n, ",") : snprintf(line + n, sizeof(line) - n, "%d,", s.pkgTemp);
        n += snprintf(line + n, sizeof(line) - n, "%d", wheaCount);
        if (loaded) {
            r->loaded++, r->wattSum += s.pkgWatts;
            if (s.pkgTemp > r->pkgTempMax) r->pkgTempMax = s.pkgTemp;
        }
        for (int i = 0; i < coreCount; i++) {
            CoreStat *st = &r->core[i];
            int mhz = 0, temp = -1, flags = 0, u = 0, threads = 0;
            for (int cpu = 0; cpu < cpuCount; cpu++) {
                if (!(cores[i].mask >> cpu & 1)) continue;
                // A thread that barely ran has no meaningful clock.
                if (s.cpu[cpu].mhz > mhz && (s.cpu[cpu].c0 < 0 || s.cpu[cpu].c0 >= 200)) mhz = s.cpu[cpu].mhz;
                if (s.cpu[cpu].temp > temp) temp = s.cpu[cpu].temp;
                flags |= s.cpu[cpu].flags;
                u += util[cpu], threads++;
            }
            u = threads ? u / threads : 0;
            if (running) st->utilSum += u, st->utilN++;
            if (loaded && !(o->exclude >> i & 1)) {
                if (mhz) {
                    st->n++, st->mhzSum += mhz;
                    if (!st->mhzMin || mhz < st->mhzMin) st->mhzMin = mhz;
                }
                if (temp > st->tempMax) st->tempMax = temp;
                st->flags |= flags | s.pkgFlags;
            }
            n += snprintf(line + n, sizeof(line) - n, ",%d,", mhz);
            n += temp < 0 ? snprintf(line + n, sizeof(line) - n, ",%d", u) : snprintf(line + n, sizeof(line) - n, "%d,%d", temp, u);
        }
        snprintf(line + n, sizeof(line) - n, "\n");
        both(csv, line);
        if (onLive) {
            static Live live;
            memset(&live, 0, sizeof(live));
            live.phase = !begun ? PH_BEFORE : running ? (loaded ? PH_LOAD : PH_START) : PH_AFTER;
            live.loadSeconds = !begun ? 0 : running ? (int)((GetTickCount() - (DWORD)loadTick) / 1000) : r->seconds;
            live.wanted = r->wanted, live.busy = busy, live.pkgWatts = s.pkgWatts, live.pkgTemp = s.pkgTemp, live.tjmax = s.tjmax;
            live.coolant = coolant, live.held = o->exclude, live.round = o->round;
            for (int i = 0; i < coreCount; i++) {
                LiveCore *lc = &live.core[i];
                lc->temp = -1, lc->firstError = -1;
                for (int cpu = 0; cpu < cpuCount; cpu++) {
                    if (!(cores[i].mask >> cpu & 1)) continue;
                    if (s.cpu[cpu].mhz > lc->mhz && (s.cpu[cpu].c0 < 0 || s.cpu[cpu].c0 >= 200)) lc->mhz = s.cpu[cpu].mhz;
                    if (s.cpu[cpu].temp > lc->temp) lc->temp = s.cpu[cpu].temp;
                    lc->flags |= s.cpu[cpu].flags;
                    lc->util += util[cpu];
                }
                int threads = 0;
                for (ULONG64 m = cores[i].mask; m; m &= m - 1) threads++;
                lc->util = threads ? lc->util / threads : 0;
            }
            for (int i = 0; i < wheaCount && begun; i++) {
                if (whea[i].time + 10000000 < loadStarted) continue;
                live.errors++;
                if (whea[i].core < 0) continue;
                LiveCore *lc = &live.core[whea[i].core];
                int at = whea[i].time > loadStarted ? (int)((whea[i].time - loadStarted) / 10000000) : 0;
                if (lc->firstError < 0 || at < lc->firstError) lc->firstError = at;
                lc->errors++;
            }
            onLive(&live);
        }

        if (running && limit > 0 && coolant >= limit) {
            TerminateJobObject(job, 1), TerminateProcess(pi.hProcess, 1);
            aborted = TRUE, running = FALSE;
            r->seconds = (int)((GetTickCount() - (DWORD)loadTick) / 1000);
            snprintf(r->reason, sizeof(r->reason), "stopped: water reached %.1f C (limit %d)", coolant, limit);
        }
        if (begun && !running && ++post >= 4) break;
    }
    wheaPoll(started);
    if (csv) fclose(csv);
    CloseHandle(loadOut);
    CloseHandle(nul);
    if (pi.hProcess) CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);

    // Errors logged from the moment the load started, by core.
    for (int i = 0; i < wheaCount; i++) {
        const Whea *w = &whea[i];
        if (w->time + 10000000 < loadStarted) continue;
        r->errors++;
        if (w->core < 0) continue;
        CoreStat *st = &r->core[w->core];
        int at = w->time > loadStarted ? (int)((w->time - loadStarted) / 10000000) : 0;
        if (st->firstError < 0 || at < st->firstError) st->firstError = at;
        st->errors++;
        r->failed |= (ULONG64)1 << w->core;
    }
    // The score, if the load prints one.
    if (prefix[0]) {
        swprintf(path, MAX_PATH, L"%lsruns\\%hs.load.txt", exeDir, name);
        FILE *f = _wfopen(path, L"rb");
        char text[8192], want[64], *at, *last = NULL;
        if (f) {
            size_t got = fread(text, 1, sizeof(text) - 1, f);
            text[got] = 0;
            fclose(f);
            snprintf(want, sizeof(want), "%ls", prefix);
            for (at = text; (at = strstr(at, want)); at += strlen(want)) last = at + strlen(want);
            if (last) {
                while (*last == ' ' || *last == ':' || *last == '=') last++;
                int k = 0;
                while (k < 30 && ((last[k] >= '0' && last[k] <= '9') || last[k] == '.' || last[k] == ',')) r->score[k] = last[k], k++;
            }
        }
    }
    BOOL throttled = FALSE;
    for (int i = 0; i < coreCount; i++) throttled |= (r->core[i].flags & F_ANY) != 0;
    if (crashed) snprintf(r->reason, sizeof(r->reason), "the load crashed after %d:%02d", r->seconds / 60, r->seconds % 60);
    else if (aborted) ;
    else if (code) snprintf(r->reason, sizeof(r->reason), "the load ended with code %lu after %d:%02d", code, r->seconds / 60, r->seconds % 60);
    else if (r->wanted && r->seconds < r->wanted) snprintf(r->reason, sizeof(r->reason), "the load ended early (%d:%02d of %d:00)", r->seconds / 60, r->seconds % 60, o->minutes);
    else if (r->errors) snprintf(r->reason, sizeof(r->reason), "the load finished but %d hardware error%s logged", r->errors, r->errors == 1 ? " was" : "s were");
    else if (throttled) snprintf(r->reason, sizeof(r->reason), "the load finished but the CPU throttled");
    else if (!r->loaded) snprintf(r->reason, sizeof(r->reason), "the load never kept the cores busy, so nothing was tested");
    r->pass = !r->reason[0];

    swprintf(path, MAX_PATH, L"%lsruns\\%hs.txt", exeDir, name);
    FILE *report = _wfopen(path, L"w");
    if (!quiet) verdict(o, r, name, stdout);
    if (report) verdict(o, r, name, report), fclose(report);

    swprintf(path, MAX_PATH, L"%lsledger.csv", exeDir);
    BOOL fresh = GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES;
    if ((ledger = _wfopen(path, L"a"))) {
        char excluded[256], failed[256];
        int hottest = -1;
        coreList(o->exclude, excluded), coreList(r->failed, failed);
        for (int i = 0; i < coreCount; i++)
            if (r->core[i].tempMax >= 0 && (hottest < 0 || r->core[i].tempMax > r->core[hottest].tempMax)) hottest = i;
        if (fresh) fprintf(ledger, "run,note,minutes,excluded,result,reason,seconds,score,errors,failed_cores,hottest_core,hottest_c,water_start,water_max,package_w\n");
        fprintf(ledger, "%s,\"%s\",%d,%s,%s,\"%s\",%d,%s,%d,%s,%s,", name, o->note, o->minutes, excluded, r->pass ? "PASS" : "FAIL", r->reason, r->seconds, r->score, r->errors, failed,
                hottest < 0 ? "" : cores[hottest].name);
        if (hottest >= 0) fprintf(ledger, "%d", r->core[hottest].tempMax);
        fprintf(ledger, ",%.1f,%.1f,%d\n", r->coolantStart, r->coolantMax, r->loaded ? (int)(r->wattSum / r->loaded) : 0);
        fclose(ledger);
    }
    return TRUE;
}

static void coreLine(FILE *out, const char *name, const CoreStat *st, BOOL excluded) {
    char clock[32] = "-", temp[16] = "-", errors[64] = "-";
    if (excluded) strcpy(clock, "held out");
    else if (st->n) snprintf(clock, sizeof(clock), "%d / %d", (int)(st->mhzSum / st->n), st->mhzMin);
    if (st->tempMax >= 0) snprintf(temp, sizeof(temp), "%d C", st->tempMax);
    if (st->errors) snprintf(errors, sizeof(errors), "%d, first at %d:%02d", st->errors, st->firstError / 60, st->firstError % 60);
    fprintf(out, "  %-7s %-15s %-7s %3d%%   %s%s\n", name, clock, temp, st->utilN ? (int)(st->utilSum / st->utilN) : 0, errors, st->flags & F_ANY ? "   THROTTLED" : "");
}

static void verdict(const Options *o, const Result *r, const char *name, FILE *out) {
    char list[256];
    fprintf(out, "\n# %s  %s\n", r->pass ? "PASS" : "FAIL", name);
    if (o->note[0]) fprintf(out, "# Settings under test: %s\n", o->note);
    if (!r->pass) fprintf(out, "# Why: %s\n", r->reason);
    fprintf(out, "# Load: %d:%02d", r->seconds / 60, r->seconds % 60);
    if (r->wanted) fprintf(out, " of %d:00 asked", o->minutes);
    if (r->score[0]) fprintf(out, ", score %s", r->score);
    if (o->exclude) coreList(o->exclude, list), fprintf(out, ", held out: %s", list);
    fprintf(out, "\n");
    if (r->coolantStart >= 0) fprintf(out, "# Water: %.1f C at start, %.1f C at most\n", r->coolantStart, r->coolantMax);
    if (r->pkgTempMax >= 0) fprintf(out, "# Package: %d W on average, %d C at most\n", r->loaded ? (int)(r->wattSum / r->loaded) : 0, r->pkgTempMax);
    fprintf(out, "# Sensors: %s\n#\n", sourceName[source]);
    fprintf(out, "  Core    MHz avg / min   Hottest  Busy   Errors\n");
    // Performance cores one by one. Efficiency cores as one line unless one of them stands out.
    CoreStat e = {0};
    int eCount = 0;
    e.tempMax = -1, e.firstError = -1;
    for (int i = 0; i < coreCount; i++) {
        const CoreStat *st = &r->core[i];
        BOOL held = (o->exclude >> i & 1) != 0;
        if (cores[i].perf || st->errors || (st->flags & F_ANY) || held) {
            coreLine(out, cores[i].name, st, held);
            continue;
        }
        eCount++;
        e.n += st->n, e.mhzSum += st->mhzSum, e.utilSum += st->utilSum, e.utilN += st->utilN;
        if (st->mhzMin && (!e.mhzMin || st->mhzMin < e.mhzMin)) e.mhzMin = st->mhzMin;
        if (st->tempMax > e.tempMax) e.tempMax = st->tempMax;
    }
    if (eCount) {
        snprintf(list, sizeof(list), "E x%d", eCount);
        coreLine(out, list, &e, FALSE);
    }
    if (r->errors) {
        fprintf(out, "#\n# Errors in order:");
        int shown = 0;
        for (int i = 0; i < wheaCount && shown < 12; i++) {
            const Whea *w = &whea[i];
            SYSTEMTIME st;
            FILETIME utc = {(DWORD)w->time, (DWORD)(w->time >> 32)}, local;
            if (!r->core[w->core < 0 ? 0 : w->core].errors && w->core >= 0) continue;
            FileTimeToLocalFileTime(&utc, &local);
            FileTimeToSystemTime(&local, &st);
            fprintf(out, "%s %02d:%02d:%02d %s %s", shown++ ? "," : "", st.wHour, st.wMinute, st.wSecond, w->core < 0 ? "?" : cores[w->core].name, wheaKind(w));
        }
        fprintf(out, "\n");
    }
    fflush(out);
}

// ---- Ranking ----

// Runs the load again and again, each time also holding out the cores the error log named, until it passes.
// history gets one line per run. Returns 0 when the last run passed.
static int rankLoop(Options *o, Result *r, char *name, char *history, int cap) {
    char list[256], failed[256];
    int max = o->maxRuns ? o->maxRuns : 6;
    history[0] = 0;
    for (int run = 1; run <= max && !stopRequested; run++) {
        coreList(o->exclude, list);
        say("# rank: run %d of at most %d, held out: %s\n", run, max, list);
        snprintf(o->label, sizeof(o->label), "rank%d", run);
        o->round = run;
        wheaCount = 0;
        if (!runOnce(o, r, name)) return 2;
        ULONG64 fresh = r->failed & ~o->exclude;
        coreList(fresh, failed);
        snprintf(history + strlen(history), cap - strlen(history), "Run %d, held out %s: %s after %d:%02d%s%s\n", run, list, r->pass ? "passed" : "failed", r->seconds / 60, r->seconds % 60,
                 fresh ? ", errors on " : "", fresh ? failed : "");
        if (r->pass) return 0;
        if (!fresh || stopRequested) return 1;
        o->exclude |= fresh;
        for (int wait = 0; wait < 40 && !stopRequested; wait++) Sleep(500);
    }
    return 1;
}

// ---- The helper task ----

// The helper task: on demand only, highest privileges, runs `shakedown --helper` without a window.
static int cmdInstall(BOOL remove) {
    WCHAR exe[MAX_PATH], user[256], name[128], xml[4096], file[MAX_PATH], cmd[MAX_PATH + 64];
    DWORD w;
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!isElevated()) {
        SHELLEXECUTEINFOW sei = {sizeof(sei)};
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"runas", sei.lpFile = exe, sei.lpParameters = remove ? L"uninstall" : L"install", sei.nShow = SW_HIDE;
        if (!ShellExecuteExW(&sei)) {
            fail("Admin rights were refused.");
            return 1;
        }
        WaitForSingleObject(sei.hProcess, 30000);
        CloseHandle(sei.hProcess);
        BOOL done = taskExists() != remove;
        say(done ? (remove ? "Helper removed.\n" : "Helper installed. Temperatures and throttling are now read without a prompt.\n") : "That did not work.\n");
        return done ? 0 : 1;
    }
    if (remove) {
        wcscpy(cmd, L"schtasks.exe /Delete /TN " TASK_NAME L" /F");
        return runHidden(cmd, 15000);
    }
    GetEnvironmentVariableW(L"USERDOMAIN", user, 128);
    GetEnvironmentVariableW(L"USERNAME", name, 128);
    wcscat(user, L"\\");
    wcscat(user, name);
    swprintf(xml, 4096,
             L"\xFEFF<?xml version=\"1.0\" encoding=\"UTF-16\"?>"
             L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\"><Triggers />"
             L"<Principals><Principal id=\"Author\"><UserId>%ls</UserId><LogonType>InteractiveToken</LogonType>"
             L"<RunLevel>HighestAvailable</RunLevel></Principal></Principals>"
             L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><AllowStartOnDemand>true</AllowStartOnDemand>"
             L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
             L"<ExecutionTimeLimit>PT12H</ExecutionTimeLimit><Priority>5</Priority></Settings>"
#ifdef SHAKEDOWN_WINDOW
             L"<Actions Context=\"Author\"><Exec><Command>\"%ls\"</Command><Arguments>--helper</Arguments></Exec></Actions></Task>",
#else
             // A console program started by a task would flash a window; conhost --headless runs it without one.
             L"<Actions Context=\"Author\"><Exec><Command>conhost.exe</Command><Arguments>--headless \"%ls\" --helper</Arguments></Exec></Actions></Task>",
#endif
             user, exe);
    GetTempPathW(MAX_PATH, file);
    wcscat(file, L"shakedown-task.xml");
    HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 1;
    WriteFile(f, xml, (DWORD)(wcslen(xml) * sizeof(WCHAR)), &w, NULL);
    CloseHandle(f);
    swprintf(cmd, MAX_PATH + 64, L"schtasks.exe /Create /TN " TASK_NAME L" /XML \"%ls\" /F", file);
    int code = runHidden(cmd, 15000);
    DeleteFileW(file);
    return code;
}

static int helperEntry(void) {
    DWORD n = sizeof(DWORD), mhz = 0;
    if (!topology()) return 2;
    RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"~MHz", RRF_RT_REG_DWORD, NULL, &mhz, &n);
    baseMhz = (int)mhz;
    return helperMain();
}
