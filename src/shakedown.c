// Shakedown: runs a real all-core load and says which CPU cores failed, when, and how hot they were.
// One console program, no installer, nothing resident. See README.md.
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

#define VERSION    "0.1.0"
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

static BOOL taskExists(void) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Schedule\\TaskCache\\Tree\\" TASK_NAME, 0, KEY_READ, &key) != ERROR_SUCCESS) return FALSE;
    RegCloseKey(key);
    return TRUE;
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
    int minutes, soak, maxRuns;
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
    fputs(line, stdout);
    fflush(stdout);
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
        fprintf(stderr, "No load is set. Put the program to run in shakedown.ini under [load] command=.\n");
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
            fprintf(stderr, "--soak needs a water temperature (see [coolant] in shakedown.ini).\n");
            return FALSE;
        }
        for (int waited = 0; c < o->soak && waited < 3600 && !stopRequested; waited += 5, c = coolantRead()) {
            if (waited % 60 == 0) printf("# soak: water %.1f C, waiting for %d C\n", c, o->soak), fflush(stdout);
            Sleep(5000);
        }
        if (c < o->soak) {
            fprintf(stderr, "Water did not reach %d C (now %.1f C). Not started.\n", o->soak, c);
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
                fprintf(stderr, "Could not start the load (error %lu): %ls\n", GetLastError(), command);
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
    verdict(o, r, name, stdout);
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

// ---- Commands ----

static int cmdRank(Options *o) {
    Result r;
    char name[96], list[256], history[1024] = "";
    int max = o->maxRuns ? o->maxRuns : 6;
    for (int run = 1; run <= max && !stopRequested; run++) {
        coreList(o->exclude, list);
        printf("# rank: run %d of at most %d, held out: %s\n", run, max, list);
        snprintf(o->label, sizeof(o->label), "rank%d", run);
        wheaCount = 0;
        if (!runOnce(o, &r, name)) return 2;
        ULONG64 fresh = r.failed & ~o->exclude;
        char failed[256];
        coreList(fresh, failed);
        snprintf(history + strlen(history), sizeof(history) - strlen(history), "#   run %d: held out %-18s %s after %d:%02d, failing: %s\n", run, list, r.pass ? "passed" : "failed", r.seconds / 60,
                 r.seconds % 60, failed);
        if (r.pass) {
            printf("\n# Ranking (weakest first is the order the cores were held out):\n%s# With %s held out the load passes.\n", history, list);
            return 0;
        }
        if (!fresh) {
            printf("\n# Ranking so far:\n%s# The last run failed without naming a new core, so the list stops here.\n", history);
            return 1;
        }
        o->exclude |= fresh;
        Sleep(20000);
    }
    printf("\n# Ranking so far:\n%s# Stopped after %d runs.\n", history, max);
    return 1;
}

static int cmdSensors(void) {
    Snapshot s;
    int util[MAX_CPU] = {0};
    utilRead(util);
    sensorsRead(&s);
    Sleep(1500);
    utilRead(util);
    sensorsRead(&s);
    double water = coolantRead();
    printf("Sensors: %s\n", sourceName[source]);
    if (s.pkgTemp >= 0) printf("Package: %d C, %d W, limit %d C\n", s.pkgTemp, s.pkgWatts, s.tjmax);
    if (water >= 0) printf("Water: %.1f C\n", water);
    printf("\nCore   CPUs     APIC   MHz    Temp   Busy\n");
    for (int i = 0; i < coreCount; i++) {
        int mhz = 0, temp = -1, u = 0, threads = 0, first = -1, last = -1;
        for (int cpu = 0; cpu < cpuCount; cpu++) {
            if (!(cores[i].mask >> cpu & 1)) continue;
            if (first < 0) first = cpu;
            last = cpu;
            if (s.cpu[cpu].mhz > mhz) mhz = s.cpu[cpu].mhz;
            if (s.cpu[cpu].temp > temp) temp = s.cpu[cpu].temp;
            u += util[cpu], threads++;
        }
        char cpus[16], t[16] = "-";
        if (first == last) sprintf(cpus, "%d", first);
        else sprintf(cpus, "%d-%d", first, last);
        if (temp >= 0) sprintf(t, "%d C", temp);
        printf("%-6s %-8s %-6u %-6d %-6s %d%%\n", cores[i].name, cpus, cpuApic[first], mhz, t, threads ? u / threads : 0);
    }
    return 0;
}

static int cmdWhea(int minutes) {
    wheaPoll(nowFiletime() - (ULONG64)(minutes ? minutes : 60) * 600000000ull);
    int perCore[MAX_CPU] = {0}, other = 0;
    for (int i = 0; i < wheaCount; i++) {
        const Whea *w = &whea[i];
        SYSTEMTIME st;
        FILETIME utc = {(DWORD)w->time, (DWORD)(w->time >> 32)}, local;
        FileTimeToLocalFileTime(&utc, &local);
        FileTimeToSystemTime(&local, &st);
        if (w->core >= 0) perCore[w->core]++;
        else other++;
        if (wheaCount - i <= 40)
            printf("%04d-%02d-%02d %02d:%02d:%02d  event %-3d %-4s %-8s bank %d  status %016llX\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, w->id,
                   w->core < 0 ? "-" : cores[w->core].name, wheaKind(w), w->bank, w->core < 0 ? 0 : w->status);
    }
    printf("\n%d record%s in the last %d minutes.", wheaCount, wheaCount == 1 ? "" : "s", minutes ? minutes : 60);
    for (int i = 0; i < coreCount; i++)
        if (perCore[i]) printf("  %s: %d", cores[i].name, perCore[i]);
    if (other) printf("  not a CPU core: %d", other);
    printf("\n");
    return 0;
}

static int cmdWatch(int minutes) {
    Snapshot s;
    int util[MAX_CPU] = {0};
    ULONG64 started = nowFiletime();
    char time8[16];
    DWORD t0 = GetTickCount();
    utilRead(util);
    printf("time,water,pkg_w,pkg_c,errors,hottest,hottest_c,p_mhz_max\n");
    while (!stopRequested && (!minutes || GetTickCount() - t0 < (DWORD)minutes * 60000)) {
        Sleep(EVERY_MS);
        utilRead(util);
        sensorsRead(&s);
        wheaPoll(started);
        int hot = -1, mhz = 0;
        for (int cpu = 0; cpu < cpuCount; cpu++) {
            if (s.cpu[cpu].temp >= 0 && (hot < 0 || s.cpu[cpu].temp > s.cpu[hot].temp)) hot = cpu;
            if (s.cpu[cpu].mhz > mhz) mhz = s.cpu[cpu].mhz;
        }
        clock8(time8);
        printf("%s,%.1f,%d,%d,%d,%s,%d,%d\n", time8, coolantRead(), s.pkgWatts, s.pkgTemp, wheaCount, hot < 0 ? "" : cores[cpuCore[hot]].name, hot < 0 ? -1 : s.cpu[hot].temp, mhz);
        fflush(stdout);
    }
    return 0;
}

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
            fprintf(stderr, "Admin rights were refused.\n");
            return 1;
        }
        WaitForSingleObject(sei.hProcess, 30000);
        CloseHandle(sei.hProcess);
        printf(taskExists() != remove ? (remove ? "Helper removed.\n" : "Helper installed. Temperatures and throttling are now read without a prompt.\n") : "That did not work.\n");
        return taskExists() != remove ? 0 : 1;
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
             L"<Actions Context=\"Author\"><Exec><Command>conhost.exe</Command><Arguments>--headless \"%ls\" --helper</Arguments></Exec></Actions></Task>",
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

static int usage(void) {
    printf("Shakedown " VERSION ": run a real all-core load and see which CPU cores fail.\n\n"
           "  shakedown run   [--minutes N] [--exclude 2,4] [--soak C] [--note \"text\"] [--label name]\n"
           "                  One run of the load in shakedown.ini. --minutes 0 (default) is a single pass.\n"
           "  shakedown rank  [--minutes N] [--exclude 2,4] [--max-runs 6]\n"
           "                  Repeat, holding out each core the error log names, until the load passes.\n"
           "  shakedown sensors          Cores, clocks, temperatures right now.\n"
           "  shakedown whea  [--minutes N]   Hardware errors Windows logged, by core.\n"
           "  shakedown watch [--minutes N]   Log sensors and errors without running a load.\n"
           "  shakedown install | uninstall   The helper that reads temperatures without an admin prompt.\n\n"
           "Each run writes runs\\<time>-<label>.csv and .txt and adds a line to ledger.csv, beside shakedown.exe.\n");
    return 0;
}

int main(int argc, char **argv) {
    Options o = {0};
    const char *cmd = argc > 1 ? argv[1] : "help";
    setvbuf(stdout, NULL, _IONBF, 0);
    paths();
    if (!strcmp(cmd, "--helper")) {
        if (!topology()) return 2;
        DWORD n = sizeof(DWORD), mhz = 0;
        RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"~MHz", RRF_RT_REG_DWORD, NULL, &mhz, &n);
        baseMhz = (int)mhz;
        return helperMain();
    }
    if (!strcmp(cmd, "install")) return cmdInstall(FALSE);
    if (!strcmp(cmd, "uninstall")) return cmdInstall(TRUE);
    if (strcmp(cmd, "run") && strcmp(cmd, "rank") && strcmp(cmd, "sensors") && strcmp(cmd, "whea") && strcmp(cmd, "watch")) return usage();
    if (!topology()) {
        fprintf(stderr, "Could not read the CPU layout.\n");
        return 2;
    }
    for (int i = 2; i < argc; i++) {
        const char *value = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(argv[i], "--minutes")) o.minutes = atoi(value), i++;
        else if (!strcmp(argv[i], "--exclude")) o.exclude = parseCores(value), i++;
        else if (!strcmp(argv[i], "--soak")) o.soak = atoi(value), i++;
        else if (!strcmp(argv[i], "--max-runs")) o.maxRuns = atoi(value), i++;
        else if (!strcmp(argv[i], "--note")) snprintf(o.note, sizeof(o.note), "%s", value), i++;
        else if (!strcmp(argv[i], "--label")) snprintf(o.label, sizeof(o.label), "%s", value), i++;
        else {
            fprintf(stderr, "Unknown option %s\n", argv[i]);
            return 2;
        }
    }
    for (char *c = o.label; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '-')) *c = '-';
    for (char *c = o.note; *c; c++)
        if (*c == '"' || *c == '\n') *c = ' ';
    if (!strcmp(cmd, "whea")) return cmdWhea(o.minutes);
    SetConsoleCtrlHandler(onCtrl, TRUE);
    sensorsOpen();
    int code;
    if (!strcmp(cmd, "sensors")) code = cmdSensors();
    else if (!strcmp(cmd, "watch")) code = cmdWatch(o.minutes);
    else if (!strcmp(cmd, "rank")) code = cmdRank(&o);
    else {
        Result r;
        char name[96];
        code = !runOnce(&o, &r, name) ? 2 : r.pass ? 0 : 1;
    }
    if (aliveEvent) CloseHandle(aliveEvent);
    return code;
}
