# OC Autopilot

The generic core of OC Autopilot: a modular engine for automatic overclocking, undervolting and stability testing on
Windows. It holds the parts every platform needs (the test engine, the safety rails, the run ledger) and a plugin
interface, so platform editions can add their own CPU, RAM, GPU and BIOS backends.

> **Status: early.** What works today is the stability tester (formerly called Shakedown; its programs are still
> `shakedown.exe` and `shakedown-cli.exe`): it runs a real all-core load and tells you which CPU cores failed, when,
> and how hot they were. It never changes a setting. The automatic tuning loop and the plugin interface are designed,
> not built. See [What works today and what is planned](#what-works-today-and-what-is-planned).

<p>
<img src="docs/panel.png" width="300" alt="The panel during a run">
<img src="docs/panel-result.png" width="300" alt="The panel after a failed run">
</p>

## Which repo do I want?

| Repo | What it is | Use it if |
|---|---|---|
| [overclock-autopilot](https://github.com/Xipzer/overclock-autopilot) (this one) | Generic core: test engine, safety rails, run ledger, plugin interface | You want the stability tester, or you are writing a backend for a new platform |
| [raptor-lake-overclock-autopilot](https://github.com/Xipzer/raptor-lake-overclock-autopilot) | Edition for Intel 13th and 14th gen: CPU and DDR5 tuning, ASRock BIOS automation | You have a Raptor Lake PC and want CPU ratios, voltage floors and memory timings tuned |
| [blackwell-overclock-autopilot](https://github.com/Xipzer/blackwell-overclock-autopilot) | Edition for NVIDIA RTX 50 cards: GPU tuning | You want your RTX 50 card overclocked or undervolted |
| [raptor-lake-field-notes](https://github.com/Xipzer/raptor-lake-field-notes) | Findings from a full manual Raptor Lake CPU and RAM campaign | You want the data and lessons behind the Raptor Lake edition |
| [blackwell-field-notes](https://github.com/Xipzer/blackwell-field-notes) | Findings from RTX 50 tuning | You want the data and lessons behind the Blackwell edition |
| [blackout-tray](https://github.com/Xipzer/blackout-tray) | Tray app for fans, RGB and screens | You want quiet fans and lights off, not tuning |

## How the pieces fit

```
             platform editions
   +-------------------+   +--------------------+
   | Raptor Lake       |   | Blackwell          |
   | CPU, RAM backends |   | GPU backend        |
   | ASRock BIOS writer|   | driver writer      |
   +---------+---------+   +----------+---------+
             |   plugin interface      |
   +---------+-------------------------+---------+
   | core (this repo)                             |
   | test engine, sensors, WHEA, guard, ledger,   |
   | outcome classes, step and stop rules, state  |
   +----------------------------------------------+
```

The core decides how a change is tested and judged. A backend says what can be changed, how to write it, how to read
it back and what a failure looks like on its hardware. The interface is in [docs/PLUGINS.md](docs/PLUGINS.md).

## What works today and what is planned

| Part | State |
|---|---|
| `run`: one pass or a timed run of a load, with a verdict per core | Works |
| `rank`: find the failing cores, weakest first | Works |
| `sensors`, `watch`: clocks, temperatures, throttling, package power (Intel), GPU power (NVIDIA) | Works |
| `whea`: hardware errors Windows logged, by core | Works |
| Run ledger (`ledger.csv`, a CSV and verdict per run) | Works |
| GPU heat for testing a CPU on a shared water loop | Works |
| Plugin interface (CPU, RAM, GPU, BIOS writer backends) | Designed: [docs/PLUGINS.md](docs/PLUGINS.md) |
| Temperature guard on every load, JSON result per run, memory tests (y-cruncher, TestMem5), DIMM temperatures | Planned |
| Doctor (read-only checks before tuning), baseline benchmark, thermal sanity check | Planned |
| BIOS read and write with read-back, failsafe, the automatic tuning loop, final report | Planned |

The full plan, with every lesson from the manual campaign and the milestones, is in [docs/ROADMAP.md](docs/ROADMAP.md).
Platform-specific parts of that plan (Raptor Lake ladders, ASRock question names, RTX 50 tuning) belong in the editions.

## What the tester does

- Starts a load you choose (Cinebench by default) and watches it.
- Reads the Windows hardware error log while it runs and matches every error to a core.
- Logs each core's real clock, temperature and whether it throttled.
- Closes the load by itself when it crashes, so no error dialog waits for a click.
- Ends with a verdict: pass or fail, why, and a line per core.
- Keeps every run in a ledger, so you can see which setting bought what.

It can start with Windows (`shakedown.exe --tray` puts only the icon in the tray) or by hand. While its panel is closed
and no run is going it does nothing at all. It changes no setting on your PC.

## Finding the weak cores

**Rank** (`shakedown-cli rank`) runs the load, takes the cores the error log named, and runs it again with those cores
held out. It repeats until the load passes. The order in which cores drop out is the ranking, weakest first, and you
have it before you change anything in the BIOS.

A held-out core is kept idle with a job affinity limit, which the load cannot undo.

## Use

1. Put `shakedown.exe` and `shakedown-cli.exe` in a folder. Copy `shakedown.example.ini` there as `shakedown.ini` and set the path to your load.
2. Optional, for temperatures and throttling: install [PawnIO](https://pawnio.eu) and put `IntelMSR.bin` (from [PawnIO.Modules](https://github.com/namazso/PawnIO.Modules/releases)) beside the programs. The panel then offers **Install the helper**, which asks for admin rights one time.
3. Start `shakedown.exe`. Its panel opens above the tray, and its icon stays in the tray until you exit.

In the panel:

- **1 pass**, **10 min** and **Rank** start a run. **Stop** ends it.
- Hover anything and the line under the buttons says what it does.
- Every core has a line (or a cell, for efficiency cores) with its clock, temperature and load. A core turns red the moment Windows logs an error for it.
- Click a core to hold it out of the next run.
- Type what you are testing (for example `P0 P1 59x, rest 58x`) and it is kept with the result.
- The panel closes when you click elsewhere, like the system tray menus. The **pin** keeps it open. Click the tray icon to bring it back.
- A run keeps going with the panel closed. The tray icon shows progress on hover and tells you when the run ends.
- **GPU heat** loads the graphics card to warm a shared water loop, for testing the CPU under hot water. It is a switch: on until you turn it off. It asks first if the card is already busy, and switches itself off at the water limit.
- The **x** closes the panel. To exit, right-click the tray icon.

The same from a terminal, for scripts:

```
shakedown-cli run                      one pass of the load
shakedown-cli run --minutes 10         a 10 minute run
shakedown-cli run --exclude 2,4        the same with P-cores 2 and 4 held out
shakedown-cli run --soak 40            wait until the water is 40 C, then start
shakedown-cli rank --minutes 10        find the failing cores, weakest first
shakedown-cli sensors                  cores, clocks and temperatures right now
shakedown-cli whea --minutes 120       hardware errors Windows logged, by core
shakedown-cli watch                    log sensors and errors with no load
shakedown-cli install                  the helper, without the window
```

`--note "P0 P1 59x, rest 58x"` writes what you are testing into the ledger.

Each run writes `runs\<time>-<label>.csv` (a line every 2 seconds), `.txt` (the verdict) and one line in `ledger.csv`.

## What counts as a pass

All of these:

- the load finished, for the whole time asked;
- Windows logged no hardware error during it;
- no core throttled (thermal, PROCHOT, power or current limit);
- the load really kept the cores busy.

## Sensors

| | Without the helper | With the helper |
|---|---|---|
| Errors by core | Yes | Yes |
| Core clock | From Windows counters | From the CPU (APERF/MPERF) |
| Core and package temperature | No | Yes |
| Throttling, package power | No | Yes |

The helper is an on-demand task that runs the tester with admin rights while the panel is open or a run is going. It
only reads sensors and writes them to a file. It takes no commands and starts nothing, and the load itself always runs
with normal rights. `shakedown-cli uninstall` removes it.

Intel CPUs only for temperatures. Error matching works on any CPU that reports an APIC ID in its error records.

## Limits

- It cannot change clocks or voltages yet. You change those in the BIOS and run it again. Automatic changes come with the backends.
- A core that is held out makes no heat, so the other cores run a little cooler than in a full run. Confirm a ranking with a full run.
- GPU power and temperature are read on NVIDIA cards only. The GPU heat itself works on any Direct3D 11 card.
- If the PC freezes, the run ends with it. The log on disk is complete up to the last 2 seconds; pipe the output to another machine if you want it to survive.
- An error with no core named (or a crash with nothing logged) stops a ranking, because there is nothing to hold out.

## Build

Needs [zig](https://ziglang.org) and Python 3 with Pillow (for the icon).

```sh
./build.sh            # build/shakedown.exe and build/shakedown-cli.exe
```

## License

[PolyForm Noncommercial 1.0.0](LICENSE.md). Free to use, change and share. Not for sale.

## Credits

Sensor access through [PawnIO](https://pawnio.eu). The per-core idea follows [CoreCycler](https://github.com/sp00n/corecycler),
which tests one core at a time; this tester runs them together and removes the ones that fail.
