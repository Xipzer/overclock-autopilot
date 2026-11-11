// shakedown-cli.exe: the same engine from a terminal. See README.md.
#include "engine.c"

static int cmdRank(Options *o) {
    Result r;
    char name[96], history[1024], list[256];
    int code = rankLoop(o, &r, name, history, sizeof(history));
    if (code == 2) return 2;
    coreList(o->exclude, list);
    printf("\n# Ranking (the cores were held out weakest first):\n%s", history);
    if (!code) printf("# With %s held out the load passes.\n", list);
    else printf("# Stopped without a passing run.\n");
    return code;
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

static int usage(void) {
    printf("Shakedown " VERSION ": run a real all-core load and see which CPU cores fail.\n\n"
           "  shakedown-cli run   [--minutes N] [--exclude 2,4] [--soak C] [--note \"text\"] [--label name]\n"
           "                  One run of the load in shakedown.ini. --minutes 0 (default) is a single pass.\n"
           "  shakedown-cli rank  [--minutes N] [--exclude 2,4] [--max-runs 6]\n"
           "                  Repeat, holding out each core the error log names, until the load passes.\n"
           "  shakedown-cli sensors          Cores, clocks, temperatures right now.\n"
           "  shakedown-cli whea  [--minutes N]   Hardware errors Windows logged, by core.\n"
           "  shakedown-cli watch [--minutes N]   Log sensors and errors without running a load.\n"
           "  shakedown-cli install | uninstall   The helper that reads temperatures without an admin prompt.\n\n"
           "Each run writes runs\\<time>-<label>.csv and .txt and adds a line to ledger.csv, beside the program.\n"
           "shakedown.exe is the same thing with a window.\n");
    return 0;
}

int main(int argc, char **argv) {
    Options o = {0};
    const char *cmd = argc > 1 ? argv[1] : "help";
    setvbuf(stdout, NULL, _IONBF, 0);
    paths();
    if (!strcmp(cmd, "--helper")) return helperEntry();
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
