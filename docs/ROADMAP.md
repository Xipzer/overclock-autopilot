# OC Autopilot roadmap

Status: draft for the owner's review, 17 November 2026. Nothing here is built yet unless the coverage matrix says "today".

This repo is the generic core. The platform-specific parts of this plan (Raptor Lake ladders, ASRock board profiles,
GPU tuning) move into the platform editions as backends; the interface between them is in [PLUGINS.md](PLUGINS.md).

Sources: the public field notes (raptor-lake-field-notes: README, cpu/, ram/, fafo-log.md, tools.md, hardware.md), the draft
harness in that repo (`harness/`, Python, untracked), the campaign scripts (ramsweep, finalrun, memtrack, tm5run, memhold,
heat-run, cpuconfirm, the elevated helper and the failsafe), the campaign journal and board map, and this repo
(`src/engine.c`, `src/cli.c`, `src/window.c`, `src/heat.c`, and the stash "wip: live voltage via OC mailbox").

## 1. Product definition

OC Autopilot takes a Windows PC from its current BIOS profile to the fastest CPU and DDR5 setting that is verified stable for
the owner's real workloads, at the temperatures those workloads reach, inside voltage and temperature limits the owner sets
before anything runs. It does this one change at a time: it writes the BIOS, reboots, reads back what the BIOS really
applied, runs the right test for that change, classifies the outcome honestly, and keeps a value only on evidence. It
recovers on its own from crash loops and failed training, and tells the owner exactly what to do when only hands can help
(no POST, hard freeze). The end product is a written report: the final set, the value that failed next to every kept value,
the margins (voltage above the last 30-minute failure, DIMM temperature tested against the highest real-use reading, hours
of each test), a before/after benchmark against the profile the owner started from, the borderline items, and the BIOS
fields that undo each one.

Realistic end goal: not "the highest number that booted", but "the highest setting that passed long, hot tests twice, with
the margin stated". On the reference machine that was 57x / 46x / ring 50 / 1.320 V and DDR5-8400 with 20 tuned timings,
71.2 ns against 74.1 ns, from a profile that was not stable at all.

Out of scope for now: GPU tuning (see section 10), AMD, non-AMI firmware, DDR4.

## 2. Where the campaign stands (the starting point for this tool)

| Area | State on the reference machine |
|---|---|
| CPU | Final: 57x all P-cores, E-cores 46x, ring 50x, fixed 1.320 V, LLC 1. Two 30-minute Cinebench runs with GPU heat clean; 1.310 V failed (2 corrected errors on P2). 58x all-core fails at the 1.360 V ceiling; per-core 58 mixes fail at 30 minutes; ring 51 needs 1.360 V and was not kept |
| RAM | Final and validated (at-rest 45 min, y-cruncher 60 min, TestMem5 60 min at up to 62 C on the hotter stick). tREFI 32767 (49151 and 65535 fail TestMem5 near 60 C), tCL 38 at DIMM 1.50/1.48 V |
| Open, waiting on the DIMM water block | tREFI 49151 then 65535; tCL 36 with more voltage; then the combined CPU and RAM sign-off; tRFC below 660 if time allows |
| Open, never run | Positive controls for the at-rest test; a true cold start (power off for minutes); MRC Fast Boot decision; the SA step test; the adaptive voltage + LLC 3/4 daily alternative |
| Second machine | Same board, SP 110, 2x48 GB DDR5-6600, 24/7 server. Repasted. Never tested at heat-soaked water. Tuning is next |

These open items are the first real-hardware acceptance tests of the tool (milestones M11 to M14). The water-block re-runs
are a RAM resume on a known machine; the second machine is the first "new machine" run.

## 3. Coverage matrix

Priority: P0 = needed for the first end-to-end campaign on the reference board; P1 = needed for a full campaign like ours;
P2 = later or optional. "Today" means what `shakedown.exe` / `shakedown-cli.exe` do now.

### 3.1 RAM timings

| Surface | What we learned | Today | Tool must | Pri |
|---|---|---|---|---|
| Frequency, gear | 8600 ruled out (timings are fixed in ns, little gain). Lower speed with tighter timings gives the same ns with less bandwidth | nothing | Record speed and gear; offer a frequency step only as an explicit, owner-chosen group; explain why it is usually not worth it | P2 |
| MRC Fast Boot | Off during tuning so every boot retrains. Owner decision for daily left open | nothing | Doctor flags it; force off during the campaign; final validation runs in the mode the owner picks for daily use | P1 |
| Command Rate | 1N no train at 8400 | nothing | One alt step, risky (training) | P2 |
| tCL | Even in Gear 2. 38 marginal at 1.45 V (passed once, broke the base), needs DIMM 1.50/1.48 V. 36 trained and then corrupted boot files | nothing | Even-only rule; pair with tCWL; on a late FAIL try the alt with one DIMM voltage step (within limits); never retry a value that corrupted data until the owner changes cooling or voltage | P0 |
| tCWL | Down to 34 at tCL 38. Moves tWTR | nothing | Step alone; re-read tWTR after | P0 |
| tRCD | 49 kept, 48 crash-looped at startup twice (separate retries) | nothing | One clock at a time near a known failure; risky gate | P0 |
| tRCDW | 52 to 32 all passed | nothing | Own group after tRCD | P1 |
| tRP | 46 kept, tRP 48 passed alone while the pair failed: tRCD was the weak one | nothing | Split linked pairs after a pair fails | P0 |
| tRAS | Not binding below tRCD + tRTP (read-back tRTP). 64 kept | nothing | Compute the floor from read-back values only; stop the ladder at the floor, below it is no gain | P0 |
| tRC | Derived (tRAS + tRP) | nothing | Show, never set | P2 |
| tRTP | Separate from tWR. Entered value reads back one lower on this BIOS. 12 is the DRAM minimum | nothing | Per-board read-back offset rule from the board profile; chip minimum from the memory profile | P0 |
| tWR | Down to 12, does not move tRTP | nothing | Plain ladder | P0 |
| tRRD_L / tRRD_S / tFAW | tRRD_L 11 kept, 10 crash-looped twice. tRRD_S 8 and tFAW 32 are minimums | nothing | Ladder for tRRD_L; mark tRRD_S, tFAW N/A with reason | P1 |
| tRFC / tRFC2 | 922 to 660; 650 failed warm, 630 no train | nothing | Linked pair; coarse then fine steps; ns shown next to clocks | P0 |
| tRFCpb | 796 to 420 (100 ns), biggest single latency gain; 400 blue-screened at startup | nothing | Own group early; ratio to tRFC shown | P0 |
| tREFI | Heat-bound. 65535 passed every short run and 60-min y-cruncher, failed TestMem5 at about 60 C. Looser tRFC did not fix it; a shorter interval did | nothing | Staged ladder; only TestMem5 at real-use DIMM temperature decides; never fix a tREFI fault by loosening tRFC | P0 |
| tWTR_S / tWTR_L | Follows tWRRD and tCWL (tWTR = tWRRD - tCWL - 6 on this board). Direct writes come back clamped | nothing | Never set; derived row with the board formula; read back after tWRRD or tCWL changes | P0 |
| tWRRD_dg / tWRRD_sg | 46 / 64 kept; 44 no train, 62 failed at once | nothing | _dg before _sg (more gain) | P0 |
| tRDWR_dg / tRDWR_sg | 19/19; 18 no train as a pair and alone | nothing | Pair first, then each alone | P1 |
| tRDRD_sg / tWRWR_sg | 15 kept; tRDRD_sg 14 gave no POST | nothing | Default: leave trained. Optional one-clock steps behind the presence gate and an explicit "I accept no-POST risk" | P1 |
| tRDRD_dg / tWRWR_dg / _dr / _dd / RTL / IO-L | Minimums or not used on 1 DPC single rank; training owns RTL/IO-L | nothing | N/A rows with reasons in the report | P2 |
| tCKE | Power-down timing, no gain with C-states off | nothing | N/A row with reason | P2 |
| Whole-page drift | tWTR moved for 11 hours unseen; ASRock Timing Optimization may rewrite Auto timings | nothing | Diff every live value against the baseline export after every boot; report any field that moved without a step | P0 |

### 3.2 RAM voltages and temperature

| Surface | What we learned | Today | Tool must | Pri |
|---|---|---|---|---|
| TX (ASRock VDD_IMC) | Auto followed XMP to 1.45 V. 1.30 V passed; 1.35 V kept for training margin after the no-POST | nothing | Voltage pass first: step down to the daily range; raise one step only for a new training failure | P0 |
| VDD2 (ASRock VDD_CPU) | 1.42 V failed in 3 minutes, 1.45 V kept | nothing | Same, stop at first fail | P0 |
| VCCSA | Not moved; SA bug freezes some 14th gen chips above a chip-specific value | nothing | Optional guided SA test: 10 to 20 mV steps, short VT3 each, stop at first freeze, presence gate | P2 |
| DIMM VDD / VDDQ | 1.50/1.48 V needed for tCL 38; cost 1 to 1.5 C. BIOS shows no live value | nothing | Only as the alt of a voltage-limited timing; verify delivery by PMIC read-back (research) or a guided HWiNFO check | P1 |
| VDD_MRC | Unknown mapping | nothing | Leave Auto; doctor note | P2 |
| DIMM temperature | SPD hub sensors read 0 after a cold boot. Sticks 6 to 9 C apart; the hotter one sets the limit. Steady peaks, not cold starts | nothing (separate dimmtemp.c in the field notes) | Built-in reader with its own SPD hub init (no HWiNFO); per-stick peak and steady value on every test; refuse RAM tests with no reading | P0 |
| Real-use DIMM temperature | The fault needed about 60 C; gaming barely loads RAM, local AI from RAM is the hot case | nothing | A "real-use capture" phase logs DIMM and core temps while the owner runs their heaviest normal workload; RAM long tests must reach that peak | P0 |

### 3.3 CPU

| Surface | What we learned | Today | Tool must | Pri |
|---|---|---|---|---|
| WHEA mapping to cores | 120 days of event 19 gave a free per-core weakness map (P2, P4 worst) | `whea` command, per-core in runs | Doctor reads full history at start; baseline map in the report | P0 |
| P-core all-core ratio | 59 fails at 1.36 V, 58 is a silicon limit (more voltage only delays: 2:11, 4:00, 6:48) | runs and judges the load; the user changes the BIOS | Descend from the current ratio at the ceiling voltage; stop a ladder when +10 mV steps only delay the crash | P0 |
| Per-core ratios | Rank found P2, P4, then P6/P3/P5. Mixes passed 10 min and failed 30 | `rank` (job object hold-out) | Build mixes from the rank; 30-minute runs for mixes; drop a mix after a failing core outside the rank (P0 surprised us) | P1 |
| E-core ratio | 46 passes, 47 hard-froze | nothing | One bin at a time; presence gate (freeze class) | P1 |
| Ring ratio | 51 passes memory tests, fails Cinebench with GPU heat at 1.340/1.350 V; passes at 1.360 V with no measurable latency gain | nothing | CPU test required for ring; report latency gain against noise before keeping a value that costs voltage | P1 |
| Vcore fixed + LLC | Fixed at the VRM on ASRock (mailbox shows adaptive, target 0). LLC 1 least bad in fixed mode. LLC may reset after a Vcore edit | nothing | BIOS question only; LLC in the watch list; never "Auto" for Vcore (adaptive, far above ceiling) | P0 |
| AC/DC load line | Neutral for delivered voltage in fixed mode | nothing | Doctor note; no step | P2 |
| Voltage floor search | 10-min floor was 30 mV optimistic. 10-min passes were luck twice. Repaste moved the floor 20 mV | stash: `sweep` (10-min steps, stop at first fail) | 10 mV steps down; 10-min runs only far from the floor; 30-min runs near it; two 30-min passes to keep; report margin to the 30-min failure | P0 |
| Adaptive + LLC 3/4 daily alternative | Research only (lower idle voltage, less wear) | nothing | Later phase with its own validation | P2 |
| Tj Max / power limits | 115 C set by owner; script stop at 100 C | nothing | Never write Tj Max; our own guard stops loads | P0 |

### 3.4 Thermal and platform

| Surface | What we learned | Today | Tool must | Pri |
|---|---|---|---|---|
| Per-core spread | 10 C steady, then 19 C: the sign that sent us to open the block | live temps | Record per-core offsets at baseline; flag a change in spread | P1 |
| C per 100 W above water/air | 12.0 to 12.6 flat; 16 before and 13 after the repaste | nothing | Compute hottest-core-above-coolant per 100 W per run; trend it; repaste hint only on drift at equal load | P1 |
| Coolant / GPU heat | CPU sits after the GPU in the loop; GPU heat adds 3 to 4 C at the same pump reading | GPU heat switch, coolant limit, `--soak` | Ask the cooling layout; use GPU heat only for CPU tests on shared loops | P0 |
| Boot / training detection | Fallback signature tRFC 395, tCL 29, tWR 54; WHEA id 1 on fallback boots | nothing | Post-boot classifier (section 4.10) | P0 |
| 0x9F shutdown hangs | Fixed 323 to 353 s "slow boots" were driver hangs at shutdown, not training | nothing | Read bugcheck 1001 before judging any slow boot; release GPU processes before restart | P0 |
| Crash and hang recovery | Startup failsafe fixes crash loops and fallbacks, not no-POST or freeze | nothing | Failsafe v2 in C; presence gate; step cards | P0 |
| Data integrity | Unstable RAM corrupted boot files in RAM; disks were fine | nothing | chkdsk /scan, sfc /scannow (and WSL fs check if present) after every unclean stop, before the next step | P0 |

## 4. The automated workflow

Each phase has an exit criterion. The tool never moves on without it, and says in plain words what is missing.

### 4.0 Phase list

| # | Phase | Exit criterion |
|---|---|---|
| 0 | Doctor and plan | Report read by the owner; limits and cooling layout entered; no blocking WARN (for example microcode older than 0x12B) |
| 1 | Baseline capture | BIOS export saved as `baseline.txt`; WHEA history map; original profile benchmarked; original profile stability known (one pass + 10 min hot) |
| 2 | Safety setup | Restore list written; failsafe tested with no change and seen to restore; BIOS profile saved to a slot by the owner (confirmed by a checkbox); Clear CMOS location confirmed |
| 3 | Thermal sanity | Per-core offsets and C per 100 W recorded; no drift flag, or the owner accepted it |
| 4 | Real-use capture | Peak DIMM, core and coolant temperatures under the owner's heaviest normal workload recorded, or a default chosen |
| 5 | CPU ratio | Highest all-core (or per-core mix) ratio that passes 30 min hot at the ceiling voltage |
| 6 | Interim voltage | Interim Vcore found (10-min search) and set 20 mV above it, to cut heat during RAM work |
| 7 | RAM | Every timing and voltage in the memory profile ends KEPT, FINAL (value kept and value that failed) or N/A with a reason; RAM checkpoint passed |
| 8 | CPU extras | E-core and ring results, each with the CPU test |
| 9 | Final voltage floor | Lowest Vcore with two clean 30-min hot runs; margin to the 30-min failure stated |
| 10 | Final validation | All long tests clean on the final set, at real-use temperatures, plus hot reboots and one cold start |
| 11 | After benchmark | Same suite as phase 1, same water start temperature |
| 12 | Report and hand-back | Report written; restore list empty; owner saved TUNED and SAFE BIOS profiles |

### 4.1 Phase 0: doctor and plan

- Read-only probes (port of `harness/rlh/doctor/probes.py`): CPU model and microcode, board and BIOS (SMBIOS), DIMM part
  numbers, speed and configured voltage (SMBIOS type 17, no serials), VBS/HVCI, crash dump mode, `bcdedit`, unclean
  shutdowns and bugchecks, WHEA history by APIC ID and kind, a SCEWIN export if SCEWIN is present, the OC mailbox
  read-out (from the stash, read only: detects "fixed at the VRM"), DIMM temperatures, iGPU driver package ranking.
- Quirk evaluation from data files (the harness `quirks/*.toml` schema): detected, applies, checked-not-present, could not check.
- The wizard asks only what probes cannot see: cooling type (air, AIO, custom loop shared with the GPU), where the coolant
  sensor is, voltage ceilings, how much time, whether someone can reach the power button during "risky" windows.
- Output: a plan the owner reads, with the groups in order, which ones need presence, and the predicted outcome of each
  risky step (from the memory profile), written before any test.

### 4.2 Phase 1: baseline capture and benchmark

- SCEWIN export to `baseline.txt` (this is the "golden" script; never edited).
- Benchmark suite on the original profile: latency meter three times (best kept), y-cruncher bench 2.5b, Cinebench
  multi and single once each, water at or below a start temperature before each, WHEA recorded at boot and during.
- Stability check of the original: one pass and 10 minutes of the CPU test hot. A failure here is a finding, not a blocker:
  "your current profile is not stable" is often the most useful line in the report.
- Lesson applied: benchmark the profile you started from. Ours could not finish Cinebench and logged errors at boot.

### 4.3 Phase 2: safety setup

Each item is shown with what it changes and how it is undone, done with one UAC prompt, and recorded in the restore list.

| Item | Campaign value |
|---|---|
| Crash dumps | Small dumps on, automatic restart on (see open question Q6) |
| Windows Recovery | `recoveryenabled No`, `bootstatuspolicy IgnoreAllFailures` |
| Failsafe task | SYSTEM at startup, v2 logic |
| Resume task | Elevated at logon, campaign only |
| Autologon with immediate lock | Optional; without it the owner logs in after each reboot ("semi-attended") |
| Background services | Optional pause list (cloud sync, model servers), restored at the end |
| BIOS admin password | Needed for SCEWIN import on ASRock Z590+; stored encrypted (section 5.3) |

Then a recovery drill: arm the failsafe with no change, reboot twice, confirm it restored and logged. Our first failsafe
had a script error that only this drill found.

### 4.4 Phase 3: thermal sanity

- 100-second all-core load at fixed settings. Record per-core offset from the core average, hottest core above coolant
  (or ambient for air) per 100 W of package power, package power.
- Store as the thermal baseline. Every later CPU run adds a point. A rise at equal load and equal GPU load, or a spread
  change, raises "contact may have changed". Not a hot run alone (our scare was power and inlet temperature, not the TIM).
- Repaste hint: shown only on drift. After a repaste the owner marks a planned power-off; the tool re-runs this phase and
  reports before and after. If cooling changed a lot, offer to re-run the voltage floor (cooler silicon moved ours 20 mV).

### 4.5 Phase 4: real-use capture

- `watch` mode for 30 to 60 minutes while the owner runs their hottest normal workload (a game with the GPU loaded, a large
  model in system RAM). Logs DIMM per stick, cores, coolant, GPU power.
- Output: the target temperatures for later tests. RAM long tests must reach the hotter stick's real-use peak (plus a
  margin of 2 C by default); CPU tests must reach the real-use water plateau. If the tests cannot reach it, say so.

### 4.6 Phase 5: CPU ratio

1. Free map: WHEA history by core from phase 0.
2. All-core at the current ratio, ceiling voltage, hot. If it fails, `rank` holds out failing cores until it passes. The
   order is the weakness ranking.
3. Descend all-core one bin at a time until a 10-minute hot run passes, then confirm 30 minutes.
4. If the board has per-core ratios: try one bin up on the strongest ranked cores (never on any core that failed at that
   bin), 30-minute runs, because 10-minute passes of mixes were luck twice. A new core failing (as P0 did) drops the mix.
5. Stop rule for voltage: if each +10 mV step only moves the crash later (2:11, 4:00, 6:48), the ratio is a silicon limit
   at this ceiling. Do not keep climbing.
6. One change per run: ratio and voltage never change together.

Exit: the ratio set that passed 30 minutes hot at or below the ceiling.

### 4.7 Phase 6: interim voltage

- 10 mV steps down, 10-minute hot runs, back to back with no cooldown, until the first non-pass.
- Set 20 mV above the last pass. This cuts heat for the RAM work. The real floor comes in phase 9.

### 4.8 Phase 7: RAM

Order (plan v2, from the memory profile; each group stops at its first failure, alt groups try alternatives):

1. Voltages to daily-safe values: TX down (1.40, 1.35, 1.30), VDD2 down (1.42, 1.40).
2. Turnarounds through tWRRD: tWRRD_dg, tWRRD_sg, tRDWR_dg, tRDWR_sg.
3. tRFCpb (coarse, then fine).
4. tRFC / tRFC2 (linked), coarse then fine.
5. tWR, tRTP (with the read-back offset), then tRAS down to the floor computed from read-back values.
6. tCWL (CL - 2 first).
7. tRCD / tRP pair, then each alone, one clock at a time near a known failure; tRCDW.
8. tRRD_L.
9. tCL (with the DIMM voltage alt), presence gate.
10. tREFI in stages (40960, 49152, 57344, 65535 and up), short runs only to find candidates.
11. Optional, presence and explicit acceptance: tRDRD_sg / tWRWR_sg one clock, Command Rate, SA test.

Per step:

1. Build the full set: fresh export + every kept value + this step. Write it. Re-export and verify every stored value.
2. Arm failsafe, write the step card, journal "SET ... If nothing follows, the PC did not come back", close GPU-heavy
   processes, reboot.
3. After boot: classify (section 4.10). Diff the whole timing page against baseline.
4. Search test: latency/bandwidth, y-cruncher FFTv4 + N63 + VT3 (7 to 8 minutes), WHEA, DIMM temps. Two passes in the
   same boot in the search phase while the base is young; one 8-minute run for the last probes once a long checkpoint
   has passed (the campaign's late rule).
5. Keep, or fail and restore: restore, reboot, confirm by export, and only then the next step.

Checkpoints (long runs): after each block of about 5 kept changes, and before tREFI work: at-rest test 45 minutes,
y-cruncher 60 minutes, TestMem5 60 minutes at the real-use DIMM temperature. No GPU heat, CPU load kept cool.

Heat-bound faults: a failure that appears only in long runs on warm sticks goes to the heat-sensitive family first (tREFI,
then tCL with its voltage), never to tRFC by default. Test with TestMem5 at real-use temperature. Use the at-rest test to
point at refresh.

### 4.9 Phases 8 to 12

- **CPU extras (8):** E-core ratio and ring, one bin each, each step two memory runs then 10 minutes of the CPU test with GPU
  heat (shared loop); keep only with the CPU test. Ring: show the latency gain against run-to-run noise; if a ring step
  needs more voltage and gains less than the noise, recommend not keeping it.
- **Final voltage floor (9):** from the interim value down in 10 mV steps, 30 minutes each, hot. Lowest pass gets a second
  30-minute run. Report: kept value, the value that failed at 30 minutes, the margin, and the option one step up.
- **Final validation (10):** at-rest 45 min, y-cruncher 60 min, TestMem5 60 min (at real-use DIMM temperature, CPU cool,
  no GPU heat); CPU test 30 min with GPU heat; five hot reboots (boot time each, short VT3 after each); one true cold start
  (power off for minutes, owner action). Hardware errors counted over the whole validation, not only inside test windows.
  A failure gives back one rung of the step-back ladder and repeats only that test.
- **After benchmark (11):** same suite as phase 1.
- **Report (12):** section 5.6. Undo the restore list. Cards for the owner to save TUNED and SAFE BIOS profiles (UEFI
  profiles are not reachable through SCEWIN).

### 4.10 Outcome classes

| Outcome | How the tool knows | Who recovers |
|---|---|---|
| PASS | Values live as written, tests clean, WHEA 0 | |
| FAIL | Values live, a test found an error, or new corrected errors, or the load crashed (a silent crash is a fail) | tool |
| CRASH | Kernel-Power 41 / 6008 / 1001 after the step was set (look-back starts at the step's SET time, not before) | tool or failsafe |
| NO TRAIN | Values the step did not touch changed, or the board fallback signature is live, or WHEA id 1 on this boot | tool |
| NOT APPLIED | Only our value changed (after the board's read-back offsets), normal boot | nothing |
| UNSTABLE BOOT | Trained, then boot files failed checksums, recovery screens | person; tool runs integrity checks after |
| NO POST | Long boot gap plus Boot Failure Guard fallback memory speed, or the owner answers "I had to power-cycle" | person |
| FREEZE | Unclean shutdown with no bugcheck during a test, "running" marker still set | person |
| NOT A RESULT | Tool error, guard stop, test did not start, coverage too low | tool re-runs; never counted against the value |
| SKIPPED | A rule, gate or doctor implication refused the step | |

### 4.11 Stop and step-back rules (built in, not advice)

1. One change per step. Declared linked pairs (tRFC/tRFC2) count as one.
2. A failed step: restore, reboot, confirm by export, integrity checks after any unclean stop, then the next step.
3. A failed validation test gives back one rung of the ladder and repeats only that test. Passed tests are not re-run after
   loosening, except the CPU test after any CPU field change.
4. Two ladders: memory (newest kept refresh change first, then the value kept on the thinnest evidence, ..., plain XMP)
   and CPU (E-cores back, then Vcore +10 mV to the ceiling). A CPU-caused fault never pulls back memory and the reverse.
5. Who caused it: high package power and core temperature, no stop code, a WHEA record on a core, or a pass of in-cache
   y-cruncher on the same set point at the CPU; an error that needs memory traffic points at RAM/IMC even if y-cruncher
   names a core thread.
6. A 10-minute pass can be luck. Near the floor every step runs 30 minutes; keeping needs two.
7. Controls: after three failures in a row, or several late failures that look alike, run the kept set unchanged. If the
   control fails, the base is marginal: earlier failures become "unproven", step back the newest kept change, re-run the
   control, then retry the unproven steps once.
8. Test at real-use temperature: RAM long tests must reach the real-use DIMM peak; CPU tests start at or reach the
   real-use water plateau, back to back with no cooldown.
9. Temperature guard on every load: stop at 100 C on the hottest core (two readings in a row) or 104 C once; coolant limit
   (default 45 C); DIMM hard stop (default 65 C). A guard stop is NOT A RESULT.
10. Hard stops: never above the owner's voltage limits; never "Auto" for a CPU voltage; never write a value the profile marks
    as data-corrupting until the owner changes cooling or voltage; stop and wait for the owner when every rung is used.
11. Planned power-offs: a "maintenance" button marks the next boot so a repaste is not a crash.
12. Presence gate: steps of the no-POST or freeze class (tRDRD_sg/tWRWR_sg, tRCD/tRP near a known failure, E-cores up,
    Command Rate, SA, anything that crash-looped before) wait until the owner presses "I am at the PC".
13. A slow boot is not evidence of anything until the bugcheck record and the fallback signature are checked.
14. Never edit a verdict. Downgrades (no POST, freeze) only through `mark`, with a note.

## 5. Lessons turned into behaviour

| # | Lesson | Tool behaviour or check | Where |
|---|---|---|---|
| 1 | One pass is not a pass | Two passes to keep in search; long checkpoints | 7, 9 |
| 2 | Ten minutes is not thirty | 30-minute runs near the voltage floor; 2 x 30 to keep | 5, 9 |
| 3 | A silent crash is a fail | Load exit without a result, crash file, or reboot during a run = FAIL | all tests |
| 4 | Test hot; shared loop means GPU load | Soak to real-use plateau; GPU heat on CPU tests for shared loops | 5, 8, 9 |
| 5 | Read effective settings after every boot, including untouched ones | Full-page diff against baseline every boot | 7 |
| 6 | Tooling is part of the experiment | Parser fixtures from real logs; tool errors are NOT A RESULT; sim tests for every incident below | all |
| 7 | Order: voltages, tertiaries, refresh, heat-sensitive, then risky | Memory profile defines the order; checker rejects plans that break it without a reason | 0, 7 |
| 8 | Fused favoured cores and SP do not predict the weak core | Rank every core; never infer from fused ratios | 5 |
| 9 | A startup failsafe cannot fix no-POST or freeze | Presence gate; step cards | 7, 8 |
| 10 | Predictions are a guide | Predictions written before risky steps, compared after, shown in the report | 0, 12 |
| 11 | A rule from one observation is a guess | Board profile rules carry evidence and confidence; low-confidence rules only warn | profiles |
| 12 | CPU-side change needs the CPU test | Groups with `cpu = true` add the CPU test | 8 |
| 13 | Diff everything against baseline on audit | `audit` command: every live value vs baseline, every journal claim vs read-back | any |
| 14 | Test RAM at real-use temperature with a test that heats sticks | Real-use capture phase; TestMem5 at that peak | 4, 7, 10 |
| 15 | Keep the CPU out of a RAM test | No GPU heat in memory tests; load confined to cooler cores, ramped up only under a temperature threshold | 7, 10 |
| 16 | Fix the thing that is heat-sensitive | Heat-bound failure goes to tREFI first, not tRFC | 7 |
| 17 | A fixed-length delay is a timeout | Read 1001 bugcheck before judging a slow boot | post-boot |
| 18 | Short runs to search, long runs at checkpoints | Checkpoint every ~5 kept changes | 7 |
| 19 | Write the full set, read it back before reboot | Writer builds from fresh export + all kept values; refuses partial sets; re-export verify | BIOS |
| 20 | Liquid metal alloys, it does not dry | Repaste hint text; never "it is old" without the date | 3 |
| 21 | A 10-minute pass can be luck | Mixes and floor steps run 30 min | 5, 9 |
| 22 | Cooling buys voltage before clocks | After cooling change, re-run floor, not ratio | 3, 9 |
| 23 | Voltage that only delays the crash is not a fix | Stop a voltage alt when crash time grows but never passes at the ceiling | 5, 8 |
| 24 | Benchmark the profile you started from | Phase 1 is mandatory | 1 |
| 25 | Shared HID device: flush before you ask | Any HID sensor reader flushes input before each request, reopens on mismatch; stale coolant (no change for 5 min under load) disables water gates | sensors |
| 26 | y-cruncher.exe is a launcher | Put loads in a job object (children inherit the affinity limit); verify per-core load after 20 s matches the mask; abort if not | tests |
| 27 | Cinebench resets its own affinity | Job object affinity, never `start /affinity` (today: done) | tests |
| 28 | Confine TestMem5 once, at launch | Mask set at creation; never moved mid-run | tests |
| 29 | TM5 0.13.1 writes only "Testing completed" | Parser fixtures for that wording | tests |
| 30 | TM5 "error N occurred in the program" | Counts as FAIL on a set under test | tests |
| 31 | TM5 coverage drops right after another test | Wait for free memory; read "Testing N MB x 32 (P%)"; restart if under 75% | tests |
| 32 | "Stop on Error: Enabled" matched an error regex | Parsers match result lines, not settings echoes | tests |
| 33 | Score was at the end of output | Read the tail (today: done) | tests |
| 34 | Ledger lookup broke on a comma in the run name | Each run writes a JSON result file; the orchestrator never greps the CSV | tests |
| 35 | Stale SCEWIN script silently skipped (prompt holds live value) | Build every import from a fresh export; count "unmatched question" as a write failure | BIOS |
| 36 | Partial write filled from golden reset everything to XMP | Writer API takes the full wanted set only; diff of wanted vs stored before reboot | BIOS |
| 37 | Reader raced the helper's result file | Result files end with a completion line; reader waits for it | BIOS |
| 38 | tRTP reads back entered minus 1 | Per-board read-back offsets in the board profile | BIOS |
| 39 | tWTR derived from tWRRD and tCWL | Derived settings never stepped; formula check on read-back | 7 |
| 40 | Failsafe needs two starts; dies if Windows dies earlier | Failsafe counts starts; card says so; presence gate for steps that crashed at boot before | 2, 7 |
| 41 | Failsafe on fallback timings did nothing | Failsafe v2 takes a fresh export and writes numbers by name | 2 |
| 42 | Failsafe and sweep both fired: extra reboot | Disarm when the orchestrator restores | 2 |
| 43 | Next step written from a failing session (tCL 36 disaster) | Restore and confirm before anything new | 7 |
| 44 | Crash check looked before the step was set | Look-back starts at the SET time | post-boot |
| 45 | Validation restarted 60 minutes blind after a reset | "running" marker; at next start report the crash, run integrity checks, step back | 10 |
| 46 | Boot Failure Guard did not load defaults; NVRAM kept settings | After a rough boot, export and look; detect fail-safe memory speed | post-boot |
| 47 | WHEA id 1 on every fallback boot | id 1 = training record, excluded from CPU error counts, used as NO TRAIN evidence | post-boot |
| 48 | 0x9F shutdown hang from a model server on the GPU | Before a scripted restart: list GPU processes (NVML), close the owner's listed ones, wait; journal what was closed | reboot |
| 49 | Dumps on: NMI bugcheck sat frozen an hour | Small dumps + auto restart; boot-gap watchdog note in the card | 2 |
| 50 | DIMM sensors read 0 after cold boot | Own SPD hub init; never record 0 as a temperature | sensors |
| 51 | Cold-start fan reading overstated the fan | Report steady peaks only (rolling window), never the first minutes | sensors |
| 52 | Hard reset under a 459 W "memory" test | Memory tests refuse GPU heat; guard verified at low load first (`guard-check`) | 7 |
| 53 | E-cores-only y-cruncher ran on 32 threads at 430 W | Verify per-core load against the mask, abort on mismatch | tests |
| 54 | Chat interruption killed a run; broken mirror pipe killed a test | Orchestrator runs detached (task), mirrors are side copies that cannot end a test | arch |
| 55 | Stop rule matched "water" in a result line | Structured results, no free-text matching for control flow | arch |
| 56 | Journal ended in zero bytes after reboot | Flush and FlushFileBuffers every line; atomic state writes | arch |
| 57 | Step card said "Vcore back to Auto" | Cards name exact numbers; "Auto" banned for CPU voltage fields | cards |
| 58 | HWiNFO took the Super I/O, fans surged | Voltage checks with HWiNFO are a scheduled, announced step; not during a decisive run | 7 |
| 59 | iGPU fell back to an old board-specific package; HWiNFO hung | Doctor lists driver packages for the iGPU and flags an older board-specific one outranking a newer one; instructions only, never removes | 0 |
| 60 | WSL page cache held 23 GB | Before memory tests check available RAM; offer `drop_caches` when WSL is running | 7, 10 |
| 61 | Undo the sweep settings at the end | Restore list; final phase refuses to finish until it is empty | 12 |
| 62 | Repaste photos were from the other machine | Thermal claims only from this machine's logged data, with timestamps | 3 |
| 63 | ASRock "Fixed" Vcore lives at the VRM | Doctor reads the mailbox; disables live mailbox voltage on such boards | 0 |
| 64 | DDR5 on-die ECC never reaches WHEA | Never treat WHEA 0 as RAM proof; RAM verdicts from tests only | 7 |
| 65 | Coverage audit found untested values | Report lists every setting as FINAL, KEPT, N/A with a reason; no "TO TEST" at finish | 12 |
| 66 | Pre-register readings before interpreting | Positive controls and risky steps store the expected reading before they run | 7, 10 |
| 67 | Hot restart vs cold start differ | Validation includes hot reboots and one owner-run cold start | 10 |

## 6. Architecture

### 6.1 What stays Win32 C

Everything that touches hardware, runs elevated or runs at boot stays C, system DLLs only, built with zig as today:

| Part | Files (new or changed) |
|---|---|
| Engine: topology, sensors, WHEA, run, verdict, rank | Split `src/engine.c` into `topo.c`, `sensors.c`, `whea.c`, `run.c`, `ledger.c` (compiled separately instead of `#include`) |
| Guard | `src/guard.c`: core, coolant and DIMM limits, per-core load vs mask check; used by `runOnce` |
| Memory tools | `src/dimm.c` (from field notes `dimmtemp.c`, with SPD hub init), `src/memlat.c`, `src/hold.c` (from `memhold.c`, with the inject self-test) |
| Test runners | `src/t_ycruncher.c`, `src/t_tm5.c`, `src/t_cinebench.c` (from today's `[load]`), each returning a `TestResult` |
| Panel and wizard | `src/window.c` (wizard pages added; same tray panel) |
| GPU heat | `src/heat.c` unchanged |
| Sensor helper | unchanged; still read-only, never takes commands |
| BIOS backends | `src/bios.c` (interface), `src/scewin.c` (port of `rlh/scewin.py` and `rlh/backends.py`), `src/manual.c`, `src/sim.c` |
| Orchestrator | `src/autopilot.c` (state machine, port of `rlh/engine.py`), `src/plan.c` (groups, rules, port of `rlh/rules.py`), `src/postboot.c` (classifier), `src/state.c`, `src/journal.c`, `src/report.c` |
| Doctor | `src/doctor.c` (probes), `src/quirks.c` (TOML-subset reader and evaluation) |
| Failsafe | `src/failsafe.c`, run as `ocap-cli failsafe` by a SYSTEM task (replaces the PowerShell script) |

Why the orchestrator in C and not the Python harness: one toolchain, no interpreter to ship, the "system DLLs only" rule
holds, and the failsafe shares the SCEWIN code with the orchestrator. The harness stays in the field notes as the design
reference; its tests become scenario files (below). Alternative in Q5.

Data files, not code: `profiles/boards/*.toml`, `profiles/memory/*.toml`, `profiles/cpu/*.toml`, `quirks/*.toml` (the
harness schema, reused as is).

### 6.2 Process and privilege model

| Process | Runs as | Does |
|---|---|---|
| `shakedown.exe` (panel, wizard) | user, unelevated | Shows state, asks questions, sends a small fixed set of requests |
| `shakedown-cli.exe autopilot` (runner) | elevated scheduled task at logon, campaign only | Owns state, writes BIOS, reboots, starts tests |
| Sensor helper | elevated, on demand | Reads sensors into a file. Unchanged |
| Failsafe | SYSTEM at startup, campaign only | Restores last good set if a step's boot repeats without its test starting |
| Loads (Cinebench, y-cruncher, memlat) | user rights | The runner starts them with a normal-user token (`SaferComputeTokenFromLevel`), as today's rule requires |
| TestMem5, `hold` | elevated if needed for page locking | Started by the runner with a fixed path and fixed config only |

Rules:

- Privileged inputs (plan, state, last good set, password blob) live in `%ProgramData%\OCAutopilot\` with an ACL of
  Administrators and SYSTEM full control, Users read. Nothing elevated reads a path a normal process can write.
- The panel talks to the runner over a named pipe with fixed verbs: `pause`, `resume`, `present` (I am at the PC for N
  minutes), `approve <step id>`, `mark <outcome> <note>`, `abort` (restore last good, stop). No verb carries a value. A
  user-level process can at worst choose among steps already in the plan and inside the limits.
- Limits are compiled in per CPU family, and can be made stricter freely. Making one looser needs the wizard's typed
  acceptance (the harness's `i_accept_the_risk` plus a reason), is stored in the admin-only config, and is journaled.
- One UAC prompt to start a campaign (installs the runner task, failsafe task, safety items) and one to finish it.

### 6.3 BIOS writes

- Backend interface (`bios.c`): `export(path)`, `read() -> map`, `apply(full_wanted_set) -> stored map`, `verify(map)`,
  `arm_failsafe(last_good)`, `disarm()`, `reboot(reason)`. `apply` refuses a partial set.
- SCEWIN backend (AMI only): user supplies `SCEWIN_64.exe` and its drivers; the tool checks the AMI Authenticode
  signature before use and never bundles it (Q1). Export always works on the reference board; import on ASRock Z590+
  needs a BIOS admin password and `/cpwd`. The password is stored with DPAPI (machine scope) in the admin-only folder and
  masked in all output.
- Every write: fresh export, copy anything that drifted from baseline back, set kept values plus the step, import, wait for
  the completion line, re-export, compare (with the board's read-back offsets), then reboot. "Unmatched question" or a
  mismatch is a write failure and nothing reboots. The harmless "TcgStorageDynamicSetupVar" warning is whitelisted per
  board profile.
- Manual backend: prints a card with exact BIOS paths and numbers; the owner enters them; after boot the tool reads back by
  SCEWIN export if available, else by measured effects (core clocks from APERF/MPERF, E-core clock) and asks for the rest.
- Board profiles map logical names to question names (never tokens; tokens change between BIOS versions), with index for
  duplicates, value units, Auto value, read-back offsets, derived settings and their formulas, fallback signature, the
  memory profile question, and which fields need OC Mode.
- Live CPU backends (optional, later): the campaign drove A-Tuning by clicks with screen capture to search Vcore without
  reboots. The stash adds mailbox writes. Neither is on the critical path (section 6.7).

### 6.4 State that survives reboots

- `state.json`: phase, group, step index, kept set, last good set, pending step, ladders used, controls, counters. Written
  to a temp file, flushed, then `MoveFileEx(REPLACE_EXISTING | WRITE_THROUGH)`.
- `journal.md` (for people) and `events.jsonl` (for code): every line flushed with `FlushFileBuffers`. The line "SET X.
  If nothing follows, the PC did not come back from this boot" is written before every reboot.
- `running.json`: what test is running since when, on which settings hash. Present at startup means the last run ended
  in a crash or freeze.
- `maintenance` marker: the next unclean stop is planned (repaste).
- `runs\*.json`: one result per test, read by the orchestrator.
- At every start the runner does post-boot analysis before anything else: boot gap (last shutdown event to boot), event
  41/6008/1001 since the SET time with the bugcheck code, WHEA records by id, failsafe `restored` flag, live export diff,
  memory speed, then classifies.

### 6.5 Failsafe

- C port of failsafe v2: on the first boot of an armed step it records the step; on a second boot of the same step without
  the runner having started the test, it takes a fresh export, writes the last good numbers by question name, leaves a
  `restored` flag and restarts once.
- Known limits, shown to the owner in plain words: it cannot help with a no-POST, a hard freeze, or a crash before it has
  run twice (about 18 s into each boot). That is what the presence gate and the step cards are for.
- Tested with a no-change drill in phase 2, and in CI by the simulator.

### 6.6 Guiding a non-expert

- Wizard pages: 1 what this does and the risks; 2 doctor result in plain words; 3 limits (pre-filled with safe defaults,
  explained); 4 cooling layout; 5 save your BIOS profile and find Clear CMOS (with a board-specific picture reference);
  6 safety setup (one UAC); 7 baseline; 8 the campaign view (current step, time left, what may happen next); 9 report.
- Verdicts in plain words, one line each: "tRFC 650: failed after 9 minutes at 58 C. Kept 660."
- Step card before every risky reboot, on screen and saved; shown as a QR code the owner can scan with a phone, so the fix
  is readable when the PC has no picture. The card has: what was set, what you may see, how long to wait, and the exact
  BIOS fields with numbers that undo it.
- No-POST guide (always in the card): wait 12 minutes (training retries); hold power 5 s and power on, up to five times
  (Boot Failure Guard); if still black, Clear CMOS and load the saved profile. Then start Windows and the tool continues.
- Presence: risky groups wait with a clear "press when you can reach the power button for the next 15 minutes".
- Optional notify hook: a user-configured command gets important journal lines (off by default, nothing uploaded).

### 6.7 The stash "wip: live voltage via OC mailbox"

What it is: `src/oc.c` reads and writes MSR 0x150 through PawnIO, `oc set <mV>` changes a fixed target live, and `sweep`
steps the voltage down with 10-minute runs until the first failure.

Evaluation:

- On the reference board it cannot work: ASRock Fixed Mode sets Vcore at the VRM, the mailbox reports adaptive target 0,
  and `ocSet` only changes a voltage that is already fixed in the mailbox. Undervolt Protection and VBS can also block it.
- The `sweep` logic encodes the wrong rule (10-minute floor, stop at first fail). It must not ship as is.
- What is worth keeping: the read path (`ocRead`, `vidMv`) as a doctor probe that detects "fixed at the VRM", UVP, and the
  ratio fields; and the write path behind a capability test, for boards where the override voltage goes through the CPU.

Recommendation: merge the read-only part into `doctor.c` in M5. Keep the write path parked until a board is found where a
write-then-measure test (requested VID and measured clock and package power move as expected) passes. If it does, a live
backend lets phase 6 and the coarse part of phase 9 run without reboots; the final 30-minute confirmations still run on
the BIOS value after a reboot.

## 7. Portability

| Part | Specific to the reference build | General |
|---|---|---|
| Method, outcome classes, ladders, stop rules | | yes |
| Shakedown engine (WHEA by APIC, job object, guard) | Intel MSRs for temps | WHEA mapping works on any CPU with APIC IDs |
| GPU heat | NVIDIA power read | D3D11 load works on any card |
| SCEWIN backend | ASRock password and `/cpwd`, the Tcg warning | AMI boards |
| Question names, read-back offsets, tWTR constant (6), fallback signature (tRFC 395), BFG behaviour, LLC levels, VDD_IMC = TX, VDD_CPU = VDD2, Fixed Vcore at VRM | ASRock Z790, BIOS 10.01 | |
| Ladders, chip minimums, heat behaviour of tREFI and tCL | SK hynix 24 Gb M-die, 2x24 GB, 1 DPC, CUDIMM in bypass | DDR5 timing rules (tCL even in Gear 2, tRAS floor, tWTR via tWRRD on Intel) |
| Voltage limits, Vmin shift, SA bug, microcode | Raptor Lake | |
| Coolant reading | Pump sensor via a third-party tray app in the registry | Any number source (registry, file, command) |

How to generalise:

- **Board profiles** (`profiles/boards/<vendor>-<chipset>-<model>.toml`): question names, offsets, derived formulas,
  fallback signature, recovery features. A profile can extend a family profile (`asrock-z790.toml`). A board with no
  profile can still run the manual backend, and the doctor offers a "learn" mode: the owner changes one field in the BIOS,
  the tool diffs two exports and records the question name.
- **Memory IC profiles** (`profiles/memory/<vendor>-<density>-<die>.toml`): starting ladders, chip minimums, temperature
  behaviour, predictions with sources, from the field notes research. Identified from SPD part numbers where possible,
  else chosen by the owner.
- **CPU family profiles**: voltage limits, guard limits, core naming, what the WHEA kinds mean.

Support order:

1. ASRock Z790 (reference and second machine), Raptor Lake, 24 Gb M-die; then 2x48 GB DDR5-6600 (second machine, so the
   tool runs on a kit we did not tune by hand).
2. Other ASRock Z790/B760 AMI boards (same family profile, new question maps).
3. ASUS, MSI, Gigabyte Z790 in manual mode with SCEWIN read-back; SCEWIN write after one confirmed board each (ASUS needs
   Publish HII Resources, unverified).
4. 16 Gb and 32 Gb Hynix, Samsung and Micron memory profiles.
5. Arrow Lake (Z890) as a new CPU family. AMD later (different error reporting, different firmware tools).

## 8. Milestones

Each is small enough to ship and test alone. "HW" means it can only be accepted on real hardware; the rest are accepted in
CI (zig build plus the simulator and fixtures on a Windows runner).

| # | Milestone | Main changes | Acceptance tests | HW |
|---|---|---|---|---|
| M0 | Groundwork | Split `engine.c` into units; CI build with `-Wall -Wextra`; `tests/` runner exe; decide binary names (Q2) | Build clean; existing CLI behaviour identical against a `cmd /c ping` load | no |
| M1 | Run results and guard | `runs\*.json` result per run; `guard.c` (100 C twice or 104 once, coolant, DIMM); per-core load vs mask check after 20 s; `guard-check` 5-minute self-test | Fake load that spawns a child: affinity holds; fake sensor feed trips each limit; guard stop reported as NOT A RESULT | guard-check: HW |
| M2 | Memory sensors and meters | `dimm.c` with SPD hub init, `memlat.c`, `hold.c` with inject self-test, `sensors` shows DIMMs | Inject self-test finds the flipped bit; DIMM reading non-zero after a cold boot | HW |
| M3 | Test runners | y-cruncher (worker affinity, result-line parser, in-cache variant), TM5 (launch-time mask, "Testing completed", program error = FAIL, coverage check), Cinebench unchanged | Parser fixtures from real logs (redacted) for every pitfall in rows 29 to 34 | first real run: HW |
| M4 | Journal, state, report skeleton | `state.c`, `journal.c`, `events.jsonl`, running marker, maintenance marker, `report.c` | Kill the process mid-write in a loop: state never corrupt; running marker found on next start | no |
| M5 | Doctor | `doctor.c` probes, `quirks.c`, mailbox read from the stash, iGPU driver check, WHEA history map | Fake facts file gives the same report as the harness doctor on its example facts file | probe run: HW |
| M6 | Baseline and benchmark | `bench` command (memlat x3, y-cruncher 2.5b, Cinebench multi and single, water gate) | Repeat the 15 November suite on the reference machine within run-to-run noise | HW |
| M7 | Thermal sanity and real-use capture | C per 100 W, offsets, drift flag, `watch` peaks per stick | Replayed ledger from 12 to 15 November flags 14 November and clears 15 November | partly HW |
| M8 | SCEWIN read path | `scewin.c` parser (names, index, live-value prompts), `names`, board profile for the reference board, full-page diff | Fake exports from the harness tests; a real export diffed against the 15 November set shows zero unexpected fields | read: HW |
| M9 | Simulator and post-boot classifier | `sim.c` backend and system; `postboot.c`; scenario files for every incident in section 5 (stale script, partial write, tRTP offset, fallback, 0x9F, BFG, id 1, look-back) | All scenarios pass in CI | no |
| M10 | SCEWIN write path and safety machinery | Full-set writer with verify, DPAPI password, failsafe in C, resume task, restore list, GPU release before restart | Sim: all write scenarios. HW: one harmless write and read-back; the no-change failsafe drill restores and logs | HW |
| M11 | Orchestrator, RAM | `autopilot.c`, `plan.c`, memory profile for 24 Gb M-die; groups, alt groups, controls, ladders, presence gate, step cards with QR | Sim replays the whole campaign (plan v1 marginal tCL 38 base, controls A to C, plan v2, no-POST, freeze, tCL 36 corruption) and reaches the same kept set. HW: the DIMM water block re-runs (tREFI 49151, 65535; tCL 36) on the reference machine | HW |
| M12 | Orchestrator, CPU | Ratio search with rank, per-core mixes, E-core and ring with the CPU test, interim and final voltage floor with 30-minute rules | Sim replays the 15 November CPU day and reaches 57x / 1.320 V. HW: the combined CPU and RAM sign-off on the reference machine | HW |
| M13 | Final validation and report | Validation chain with step-back ladders, hot reboots, cold start prompt, whole-window error count, before/after, restore list check | Sim: a reset at 25 minutes is caught and stepped back. HW: full validation report on the reference machine matches the field notes format | HW |
| M14 | Wizard | Wizard pages in `window.c`, pipe verbs, plain verdicts | Click-through on `--shot` states; a first-time user run on the second machine from doctor to report | HW |
| M15 | Second machine | Memory profile for its kit, its board profile check | Full campaign on the second machine, including heat-soaked CPU tests it never had | HW |
| M16 | Portability | Board "learn" mode, family profiles, manual backend polish | Manual campaign on a non-ASRock AMI board by a volunteer | HW |
| M17 | Optional live CPU backend | Mailbox write behind the capability test; or A-Tuning automation as an ASRock plugin | Capability test passes on a board where it should; refuses on the reference board | HW |
| M18 | Daily-voltage alternative | Adaptive + LLC 3/4 + calibrated DC LL phase | Idle voltage lower at equal loaded voltage, CPU validation passes | HW |

The first useful release is M0 to M7 (a better tester, doctor, benchmark, thermal check: all read-only). Automatic BIOS
writes arrive with M10 and M11.

## 9. Risks and open questions for the owner

| # | Question | Notes and recommendation |
|---|---|---|
| Q1 | SCEWIN licensing | AMI proprietary, shipped inside vendor utility packages; redistribution is likely not allowed. Recommend: never bundle; the user points to their copy; we check the AMI signature. Ask AMI or a board vendor only if bundling is wanted |
| Q2 | Rename the binaries | `shakedown.exe` / `shakedown-cli.exe`, task name `Shakedown`, `%LOCALAPPDATA%\Shakedown`, `Local\ShakedownAlive`. Proposal: `ocautopilot.exe` / `ocap.exe`, with a one-time migration of the ledger and the helper task. Decide before M0 so paths are set once |
| Q3 | Data the tool may upload | Proposal: none, ever, by default. No telemetry. An optional "export anonymised report" file the owner can share by hand. A community result database is a separate future decision |
| Q4 | Third-party tools | Cinebench (Maxon licence), y-cruncher, TestMem5 0.13.1, PawnIO and its modules, HWiNFO: do not bundle; download links with pinned hashes; the doctor checks versions. Confirm each licence before any bundling |
| Q5 | Orchestrator language | Recommendation: C (one toolchain, no interpreter). Alternative: ship the Python harness with the embeddable Python runtime (faster to build, already has tests). Owner's call |
| Q6 | Crash dumps during a campaign | Campaign used both: off (an NMI bugcheck sat frozen an hour with dumps on) and small (needed to read 0x9F). Proposal: small dumps plus auto restart, and verify once on the reference machine |
| Q7 | Autologon | Needed for fully unattended reboots; stores the Windows password as an LSA secret. Proposal: optional, off by default, removed by the restore list |
| Q8 | BIOS admin password | Required for ASRock imports. Proposal: DPAPI machine-scope blob, admin-only ACL; the owner may prefer to type it at campaign start each time |
| Q9 | Default limits | Proposal for Raptor Lake: Vcore fixed 1.40 V hard cap with the owner's ceiling below it, TX 1.35 V, VDD2 1.45 V, SA 1.30 V (20 mV steps), DIMM 1.50 V, core stop 100 C, coolant 45 C, DIMM stop 65 C. Confirm |
| Q10 | Licence | PolyForm Noncommercial today. Fine for a public tool; confirm it covers the profile and quirk data contributions |
| Q11 | Remote power | No-POST and freeze need hands. Should the tool support a smart plug or a network power switch later? |
| Q12 | Reading timings without SCEWIN | Reading live DDR5 timings from the memory controller (MCHBAR) through a PawnIO module would give read-back on any board. Needs research |
| Q13 | PMIC read-back | DIMM VDD/VDDQ have no live BIOS value. Reading the PMIC over SMBus would replace the HWiNFO check. Vendor register maps differ (Richtek here). Needs research |

## 10. Future: GPU tuning (research first, nothing planned)

Owner's instruction: no GPU tuning until the domain is researched. GPU heat stays a test tool for CPU work on shared loops.
Questions to answer before any plan:

1. Which control paths exist on Windows for current NVIDIA and AMD cards (NVAPI, NVML clock offsets, the vendor's
   curve editor APIs, AMD ADLX), and which need signed drivers or are blocked for third parties?
2. What is the equivalent of WHEA for a GPU: which driver events, Xid-style errors or TDR records show a marginal clock,
   and can they be mapped to a cause?
3. Which loads find GPU core and memory instability fastest, and how long is "long enough" near the edge?
4. How do memory errors show up (GDDR7 error detection and replay, silent artefacts), and can they be counted?
5. Is undervolting (a curve point at lower voltage) or overclocking the right default for daily use and wear?
6. Power connector safety at 500 W and above: what limits must be enforced?
7. Shared loop interaction: how CPU stability changes when the GPU curve changes the loop's heat.
8. Does a live change need a reboot, and what is the recovery when the driver resets mid-test?
