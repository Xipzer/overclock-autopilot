# AGENTS.md

Guide for coding agents working on this repo. Humans: see README.md.

## Rules

- Win32 C, system DLLs only. Nothing resident, nothing installed except the optional on-demand helper task. No autostart: the tray icon exists from when the user starts it until they exit it.
- It never changes a hardware setting. Reads only: MSRs through PawnIO's signed IntelMSR module, the event log, performance counters.
- The elevated helper reads sensors and writes a file. It must never take commands, run a program or read a path from a place a normal process can write, because that would be a way around UAC. The load always runs unelevated.
- Docs: plain, short. Never use em dashes or en dashes in any file.

## Map

| Path | Purpose |
|---|---|
| `src/engine.c` | Topology, sensors, error log, the run, the verdict, ranking, the helper. `#include`d by the two programs |
| `src/window.c` | `shakedown.exe`: the tray icon and popup panel (Direct2D on the CPU, same drawing set and docking as Blackout's panel) |
| `src/cli.c` | `shakedown-cli.exe`: the same engine from a terminal |
| `tools/make_icon.py` | Draws `shakedown.ico` (Pillow) |
| `docs/panel*.png` | README images, made with `shakedown.exe --shot` |
| `shakedown.example.ini` | Config template |
| `build.sh` | zig cross-build to `build/` |

## How it works

- **Topology:** `GetLogicalProcessorInformationEx` gives cores and their efficiency class (highest class = P-cores, named P0.. in CPU order; the rest E0..). CPUID leaf 0B on each logical CPU gives its APIC ID, which is how WHEA names a CPU.
- **Errors:** `EvtQuery` on the System log for provider `Microsoft-Windows-WHEA-Logger` since the run began; `ApicId`, `MCABank`, `MciStat` are read from the rendered XML. Low 16 bits of `MciStat`: 0005 internal parity, 001x TLB, 01xx cache.
- **Sensors:** three sources, best first. Direct (already elevated), helper (file `%LOCALAPPDATA%\Shakedown\sensors.dat`, a `Snapshot` struct rewritten once a second, valid when `seq == seq2` and the tick is under 5 s old), Windows counters (`% Processor Performance`, clock only). MSRs: 19C core temperature and status bits, 1B1 package, 1A2 Tj max, E7/E8 MPERF/APERF, 611 and 606 package energy.
- **Is the helper installed:** asked with `schtasks /Query`. The task's registry key cannot be read without admin rights.
- **Helper lifetime:** the run holds the named event `Local\ShakedownAlive`; the helper exits 15 s after it can no longer open it, or after 12 h. The task runs `conhost.exe --headless shakedown.exe --helper` so no window appears.
- **Load:** started suspended, put in a job (kill on close, affinity limit when cores are held out), then resumed. Its output goes to `runs\<name>.load.txt`. A newer file matching `[load] crashfile` means it crashed: the job is ended 2 s later.
- **Verdict:** see README, "What counts as a pass". A sample counts as "under load" when the load is alive and its cores average 70% busy or more.

## The panel

- `shakedown.exe` is a tray icon plus a popup panel, built like Blackout's: a layered `WS_POPUP` docked to the taskbar's own rectangle and the screen edge, one rounded corner, the taskbar's colour (median of one pixel row), DWM transitions off. It closes on deactivate or Esc unless pinned; a tray click within 300 ms of closing does not reopen it. The panel window is made once and shown or hidden.
- A hidden main window owns the tray icon and receives `WM_LIVE` and `WM_DONE` from the worker. Starting the exe a second time opens the panel of the running one.
- One worker thread owns the engine (sensors, error log, runs). Each sample it copies a `Live` struct under a lock and posts `WM_LIVE`. The window thread only draws and never touches hardware.
- **Idle cost:** the worker samples every 2 s only while the panel is open (`watching`) or a run is going. With the panel closed and no run it calls `sensorsClose()` and waits without a timeout, so the helper exits and nothing is read. Keep it that way.
- Drawing: an `ID2D1DCRenderTarget` (software, premultiplied alpha) into a DIB, handed over with `UpdateLayeredWindow`. A frame is drawn per sample or mouse change, nothing in between. Never while the panel is hidden.
- 372 wide, height from the core counts (`height()`). Top to bottom: header (state, pin, exit), four readings, P-cores as rows, E-cores as a grid, the test card, the result card (or the last two ledger lines when there is no result yet). On a small or heavily scaled screen the whole panel is scaled down to fit.
- Colour: `heat()` maps a temperature to blue, green, amber, red (red from 85% of the way to the thermal limit) for bars, efficiency-core cells and the hottest reading. Start buttons are blue, Stop is red, a clean error count is green. The hint line under the buttons says what the thing under the mouse does; add a line there for any new control.
- Clicking a core toggles it in `heldOut`; after a ranking the cores that had to be held out stay marked.
- Exit stops a running load (it asks first), then the worker, then the process. The helper notices within 15 s and exits.
- `shakedown.exe --shot <idle|running|fail|pass|rank|nohelper|first> [light] [pinned] [x2] out=<file.bmp>` draws one state with made-up data: no window, no hardware. The bitmap has premultiplied alpha. Review these after any change to the picture; `docs/panel*.png` are made from them.

## Check

Build with `-Wall -Wextra` (in `build.sh`) and keep it free of warnings. Test without stressing a machine by pointing `[load] command` at a short `cmd.exe /c "ping ..."` line: exit codes, the crash file path, held-out cores and the score text can all be exercised that way.
