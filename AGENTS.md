# AGENTS.md

Guide for coding agents working on this repo. Humans: see README.md.

## Rules

- One console program, Win32 C, system DLLs only. Nothing resident, nothing installed except the optional on-demand helper task.
- It never changes a hardware setting. Reads only: MSRs through PawnIO's signed IntelMSR module, the event log, performance counters.
- The elevated helper reads sensors and writes a file. It must never take commands, run a program or read a path from a place a normal process can write, because that would be a way around UAC. The load always runs unelevated.
- Docs: plain, short. Never use em dashes or en dashes in any file.

## Map

| Path | Purpose |
|---|---|
| `src/shakedown.c` | Everything: topology, sensors, error log, the run, the verdict, commands |
| `shakedown.example.ini` | Config template |
| `build.sh` | zig cross-build to `build/shakedown.exe` |

## How it works

- **Topology:** `GetLogicalProcessorInformationEx` gives cores and their efficiency class (highest class = P-cores, named P0.. in CPU order; the rest E0..). CPUID leaf 0B on each logical CPU gives its APIC ID, which is how WHEA names a CPU.
- **Errors:** `EvtQuery` on the System log for provider `Microsoft-Windows-WHEA-Logger` since the run began; `ApicId`, `MCABank`, `MciStat` are read from the rendered XML. Low 16 bits of `MciStat`: 0005 internal parity, 001x TLB, 01xx cache.
- **Sensors:** three sources, best first. Direct (already elevated), helper (file `%LOCALAPPDATA%\Shakedown\sensors.dat`, a `Snapshot` struct rewritten once a second, valid when `seq == seq2` and the tick is under 5 s old), Windows counters (`% Processor Performance`, clock only). MSRs: 19C core temperature and status bits, 1B1 package, 1A2 Tj max, E7/E8 MPERF/APERF, 611 and 606 package energy.
- **Is the helper installed:** asked with `schtasks /Query`. The task's registry key cannot be read without admin rights.
- **Helper lifetime:** the run holds the named event `Local\ShakedownAlive`; the helper exits 15 s after it can no longer open it, or after 12 h. The task runs `conhost.exe --headless shakedown.exe --helper` so no window appears.
- **Load:** started suspended, put in a job (kill on close, affinity limit when cores are held out), then resumed. Its output goes to `runs\<name>.load.txt`. A newer file matching `[load] crashfile` means it crashed: the job is ended 2 s later.
- **Verdict:** see README, "What counts as a pass". A sample counts as "under load" when the load is alive and its cores average 70% busy or more.

## Check

Build with `-Wall -Wextra` (in `build.sh`) and keep it free of warnings. Test without stressing a machine by pointing `[load] command` at a short `cmd.exe /c "ping ..."` line: exit codes, the crash file path, held-out cores and the score text can all be exercised that way.
