# Shakedown

Runs a real all-core load on your PC and tells you which CPU cores failed, when, and how hot they were.

For overclockers who want to know which cores hold a setting and which do not, without guessing from a crash.

## What it does

- Starts a load you choose (Cinebench by default) and watches it.
- Reads the Windows hardware error log while it runs and matches every error to a core.
- Logs each core's real clock, temperature and whether it throttled.
- Closes the load by itself when it crashes, so no error dialog waits for a click.
- Ends with a verdict: pass or fail, why, and a line per core.
- Keeps every run in a ledger, so you can see which setting bought what.

Nothing is installed and nothing keeps running. It changes no setting on your PC.

## Finding the weak cores

`shakedown rank` runs the load, takes the cores the error log named, and runs it again with those cores held out. It repeats until the load passes. The order in which cores drop out is the ranking, weakest first, and you have it before you change anything in the BIOS.

A held-out core is kept idle with a job affinity limit, which the load cannot undo.

## Use

1. Put `shakedown.exe` in a folder. Copy `shakedown.example.ini` there as `shakedown.ini` and set the path to your load.
2. Optional, for temperatures and throttling: install [PawnIO](https://pawnio.eu), put `IntelMSR.bin` (from [PawnIO.Modules](https://github.com/namazso/PawnIO.Modules/releases)) beside the exe, and run `shakedown install` once. It asks for admin rights one time.
3. Run it from a terminal:

```
shakedown run                      one pass of the load
shakedown run --minutes 10         a 10 minute run
shakedown run --exclude 2,4        the same with P-cores 2 and 4 held out
shakedown run --soak 40            wait until the water is 40 C, then start
shakedown rank                     find the failing cores, weakest first
shakedown sensors                  cores, clocks and temperatures right now
shakedown whea --minutes 120       hardware errors Windows logged, by core
shakedown watch                    log sensors and errors with no load
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

| | Without the helper | With `shakedown install` |
|---|---|---|
| Errors by core | Yes | Yes |
| Core clock | From Windows counters | From the CPU (APERF/MPERF) |
| Core and package temperature | No | Yes |
| Throttling, package power | No | Yes |

The helper is an on-demand task that runs `shakedown.exe` with admin rights while a run is going. It only reads sensors and writes them to a file. It takes no commands and starts nothing, and the load itself always runs with normal rights. `shakedown uninstall` removes it.

Intel CPUs only for temperatures. Error matching works on any CPU that reports an APIC ID in its error records.

## Limits

- It cannot change clocks or voltages. You change those in the BIOS and run it again.
- A core that is held out makes no heat, so the other cores run a little cooler than in a full run. Confirm a ranking with a full run.
- If the PC freezes, the run ends with it. The log on disk is complete up to the last 2 seconds; pipe the output to another machine if you want it to survive.
- An error with no core named (or a crash with nothing logged) stops `rank`, because there is nothing to hold out.

## Build

Needs [zig](https://ziglang.org).

```sh
./build.sh            # build/shakedown.exe
```

## License

[PolyForm Noncommercial 1.0.0](LICENSE.md). Free to use, change and share. Not for sale.

## Credits

Sensor access through [PawnIO](https://pawnio.eu). The per-core idea follows [CoreCycler](https://github.com/sp00n/corecycler), which tests one core at a time; Shakedown tests them together and removes the ones that fail.
