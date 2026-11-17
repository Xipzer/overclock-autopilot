# Plugin interface

Status: design, not built. Names and signatures may change until milestone M11 of [ROADMAP.md](ROADMAP.md) ships.
Today the core is the stability tester only; nothing here is called by the current code.

This document says what the core does, what a backend must provide, and the rules every backend follows. It is based on
the roadmap and on the Python harness in
[raptor-lake-field-notes](https://github.com/Xipzer/raptor-lake-field-notes) (`harness/`), which already runs this loop
against a simulator and a SCEWIN backend. The C interface below is the port of that design.

## 1. Split of work

| The core owns | A backend owns |
|---|---|
| The tuning loop: one change per step, apply, read back, classify, test, keep or fail, restore | What can be changed on its hardware, and the limits for it |
| Outcome classes (section 6) and the step-back ladders | How to write a full set and read it back |
| Test runners, the temperature guard, WHEA reading, sensors common to all platforms | What "applied" means: live, after a driver reset, or after a reboot |
| State that survives reboots, the journal, the run ledger, the report | Platform facts for the doctor, and quirks |
| The presence gate, step cards, the restore list | Hints for who caused a failure (CPU, RAM, GPU) |
| Limits: a backend can make them stricter, never looser | Its own private state, saved through the core |

A backend never runs a test, never decides PASS or FAIL alone and never keeps a value. It reports facts; the core
judges.

## 2. Kinds of backend

| Kind | Examples | Provides |
|---|---|---|
| Domain | `cpu` (ratios, Vcore, LLC), `ram` (timings, DIMM and IMC voltages), `gpu` (core and memory offsets, voltage/frequency curve, power limit) | Setting catalogue, limits, plan, tests per step, classification hints, extra sensors |
| Writer | `scewin` (AMI BIOS from Windows), `manual` (step cards, the owner types values), `sim` (no hardware), `mailbox` (live CPU voltage), a GPU driver writer | Read, write with read-back, failsafe, commit (reboot, driver reset or nothing) |
| Test | `cinebench`, `ycruncher`, `tm5`, `memlat`, a GPU load | Runs one test under the core's guard and returns a structured result |

A campaign uses one writer per domain. The Raptor Lake edition pairs `cpu` and `ram` with `scewin` (or `manual`). The
Blackwell edition pairs `gpu` with its driver writer. Every edition also builds with `sim`, so the loop can be tested
in CI.

Backends are compiled in: an edition builds the core sources plus its own backend files and registers them in a table.
No DLL loading. The "system DLLs only" rule and the privilege rules in ROADMAP section 6.2 still hold.

## 3. Common types

```c
typedef enum { V_INT, V_MV, V_RATIO, V_ENUM, V_AUTO } ValueKind;

typedef struct {
    const char *name;     /* logical name: "tRFC", "vcore", "pcore_ratio", "gpu_core_offset" */
    const char *unit;     /* "clk", "mV", "x", "MHz", "W" */
    ValueKind   kind;
    long long   min, max, step;
    unsigned    flags;    /* see below */
    const char *linked;   /* names that move with this one as one step, or NULL ("tRFC2") */
    const char *follows;  /* names this one moves when changed, re-read after ("tWTR") */
    const char *test;     /* test group for a step on this setting: "ram", "cpu", "gpu" */
} SettingDef;

enum {
    S_VOLTAGE   = 1 << 0, /* checked against voltage limits; never Auto */
    S_DERIVED   = 1 << 1, /* shown and checked, never set (tRC, tWTR) */
    S_RISK_POST = 1 << 2, /* may stop the PC from starting: presence gate */
    S_RISK_HANG = 1 << 3, /* may hard-freeze: presence gate */
    S_HEAT      = 1 << 4, /* fault shows only when warm: long runs at real-use temperature */
    S_CORRUPT   = 1 << 5, /* a fault can corrupt data: integrity checks after an unclean stop */
};

typedef struct { const char *name; long long value; int is_auto; char shown[32]; } Value;
typedef struct { int n; Value v[256]; } ValueSet;

typedef struct {
    ValueSet effective;   /* what the hardware runs, as the writer can see it */
    ValueSet stored;      /* what is saved (NVRAM, driver profile) */
    char     profile[64]; /* e.g. memory profile in use; empty if not applicable */
    int      manual;      /* 1 when values came from the owner, not a read */
} Snapshot;
```

Settings use logical names. The writer maps them to its own names (SCEWIN question names with an index for
duplicates, driver API fields). Tokens and offsets that change between firmware versions never appear in a domain
backend.

## 4. Writer backend

```c
typedef enum { COMMIT_LIVE, COMMIT_DRIVER_RESET, COMMIT_REBOOT } CommitKind;

typedef struct WriterBackend {
    const char *name;
    CommitKind  commit_kind;
    int  (*probe)(Ctx *, Facts *out);                      /* read-only; can it work here? */
    int  (*prepare)(Ctx *);                                /* baseline export, once per campaign */
    int  (*read)(Ctx *, Snapshot *out);
    int  (*apply)(Ctx *, const ValueSet *full, Snapshot *readback, char *why, int cb);
    int  (*arm_failsafe)(Ctx *, const ValueSet *last_good, const char *tag);
    void (*disarm)(Ctx *);
    int  (*failsafe_restored)(Ctx *);                      /* 1 once, after the failsafe acted */
    int  (*commit)(Ctx *, const char *reason);             /* reboot, driver reset, or nothing */
} WriterBackend;
```

### Read

- `read` returns the live state. It must be fresh: never a cached copy from before the last commit.
- A value the writer cannot see is left out, not guessed. "Could not read" is an error, not a zero.

### Write with read-back

- `apply` takes the full wanted set: baseline plus every kept value plus this step. It refuses a partial set.
- Build every write from a fresh read, not from a saved script (a stale script was silently skipped in the campaign).
- After writing, read back and compare every value in the set, with the per-board read-back offsets (for example a
  setting that reads back one lower than entered). Any mismatch, unmatched name or tool error fails `apply`, and
  nothing is committed.
- `apply` returns the read-back snapshot so the core can journal what was really stored.
- Secrets (a BIOS password) are masked in every output the writer returns.

### Apply and restart semantics

| `commit_kind` | Meaning | Core does |
|---|---|---|
| `COMMIT_LIVE` | New values run as soon as `apply` returns (CPU mailbox, GPU clock offsets) | Tests at once; a crash or reboot returns to the stored or stock values, which is the recovery |
| `COMMIT_DRIVER_RESET` | Values run after a driver reload | Calls `commit`, waits for the device, reads back, then tests |
| `COMMIT_REBOOT` | Values run after a restart (BIOS) | Writes "SET x. If nothing follows, the PC did not come back", arms the failsafe, calls `commit`, and does post-boot analysis at the next start |

`commit` for a reboot must release anything that can hang shutdown first (the core passes the list of GPU processes it
may close). A live writer whose values survive a reboot must say so in `probe`, because then a crash does not undo a
bad value.

### Failsafe

- `arm_failsafe` stores the last good set where the startup task can read it (admin-only folder). It must survive the
  writer's own process dying.
- The failsafe acts on the second start of the same armed step without the test starting, writes the last good set by
  name from a fresh read, and restarts once. `failsafe_restored` reports that once.
- Writers that cannot have a failsafe (manual) return 0 from `arm_failsafe`; the core then sets the presence gate for
  every step that can stop a boot.

## 5. Domain backend

```c
typedef struct DomainBackend {
    const char       *name;                 /* "cpu", "ram", "gpu" */
    const SettingDef *settings;
    int               nsettings;
    int  (*probe)(Ctx *, Facts *out);       /* read-only facts and quirks for the doctor */
    int  (*limits)(Ctx *, const Facts *, Limits *out);
    int  (*plan)(Ctx *, const Facts *, const Snapshot *baseline, Plan *out);
    int  (*tests_for)(Ctx *, const Step *, TestList *out);
    Outcome (*classify)(Ctx *, const Step *, const Snapshot *baseline,
                        const Snapshot *now, const BootFacts *, char *why, int cb);
    Blame (*blame)(Ctx *, const TestResult *, const Evidence *);
    int  (*sensors)(Ctx *, SensorFrame *out);   /* optional: DIMM temps, GPU hot spot */
} DomainBackend;
```

| Function | Must |
|---|---|
| `probe` | Read only. Never change a setting, install anything or collect serial numbers |
| `limits` | Return hard caps for its family (for example Vcore, DIMM voltage, GPU power). The core takes the stricter of these and the owner's limits |
| `plan` | Groups of steps in a safe order (voltages first, then tertiaries, refresh, heat-sensitive, risky last), each with its risk flags, linked pairs and a prediction for risky steps |
| `tests_for` | Name the test group for a step. A step that touches the CPU always includes the CPU test |
| `classify` | Before any test runs: compare `now` with what was asked. Return `NO_TRAIN` when untouched values moved or a fallback signature is live, `NOT_APPLIED` when only our own value differs, else nothing |
| `blame` | Say which domain most likely caused a failure, with the evidence (WHEA on a core, high package power, an error that needs memory traffic). A failure blamed on another domain never pulls back this domain's ladder |
| `sensors` | Optional extra readings. Never return 0 for a sensor that did not answer |

What each domain adds:

- **CPU:** ratios per core or all-core, E-core and ring ratios, Vcore, LLC. Uses `rank` for the weakness order. Stop
  rule: when each voltage step only delays the crash, the ratio is a silicon limit. Near the voltage floor every step
  runs 30 minutes and keeping needs two passes.
- **RAM:** timings, IMC and DIMM voltages, refresh. Declares derived settings and their formulas, linked pairs, the
  fallback signature of a failed training, and heat-sensitive settings. Supplies DIMM temperatures; refuses RAM tests
  with no reading.
- **GPU:** core and memory offsets or curve points, voltage, power limit. Usually `COMMIT_LIVE`. Its failure evidence
  is driver resets and device errors instead of WHEA. What counts as enough testing is still research (ROADMAP
  section 10); the Blackwell edition owns those answers.
- **BIOS writer:** see section 4. A vendor writer adds a board profile: logical name to vendor name, read-back offsets,
  Auto value, fields that need an OC mode, and harmless warnings to ignore.

## 6. Outcomes

The core's outcome classes, from ROADMAP section 4.10. Backends return only the ones marked "backend".

| Outcome | From |
|---|---|
| PASS, FAIL | core (test results) |
| CRASH | core (event 41, 6008, 1001 after the step's SET time) |
| NO TRAIN, NOT APPLIED | backend `classify` |
| UNSTABLE BOOT, NO POST, FREEZE | core, from boot facts or the owner's `mark` |
| NOT A RESULT | core (guard stop, tool error, test did not start) |
| SKIPPED | core or backend `plan` (a rule refused the step) |

## 7. Test hook

```c
typedef enum { T_PASS, T_FAIL, T_ABORT, T_ERROR } TestStatus;

typedef struct {
    TestStatus status;         /* ABORT = guard stop, ERROR = tool problem: both NOT A RESULT */
    char       summary[256];
    double     minutes;
    double     peak_core_c, peak_dimm_c, peak_coolant_c, peak_gpu_c;
    int        whea, errors;
    double     metric[8];      /* score, ns, GB/s, as named by the test */
} TestResult;

typedef struct TestBackend {
    const char *name;
    int (*run)(Ctx *, const TestSpec *, Guard *, TestResult *out);
} TestBackend;
```

- The core starts the test in a job object with the affinity limit, runs the guard on every sample and ends the job
  when a limit trips. The test backend never decides to ignore the guard.
- Results are structured. Parsers match result lines only, never settings echoes, and keep fixtures from real output.
- Each run writes `runs\*.json`; the core reads that, never the CSV ledger, for control flow.
- Today's `run` command (Cinebench by default, WHEA by core, throttle and load checks) becomes the first test backend.

## 8. State

- The core owns `state.json` (atomic write: temp file, flush, `MoveFileEx` with write-through), the journal and
  `events.jsonl`, the `running` and `maintenance` markers.
- A backend gets one named blob in that state for its own data (for example the baseline export path). It is saved
  with the core's state and must be enough to continue after a reboot. Backends keep no other files that matter for
  resuming.
- Backends must be deterministic given their inputs, so the simulator can replay a whole campaign in CI.

## 9. Rules for every backend

1. One change per step. Declared linked pairs count as one.
2. Write the full set and read it back before any commit. No partial writes.
3. Never above the limits. Never "Auto" for a voltage. Never raise a limit without the owner's typed acceptance.
4. Never write a value the profile marks as data-corrupting until the owner changes cooling or voltage.
5. `probe` and `read` never change anything.
6. Anything elevated reads only from the admin-only folder.
7. No serial numbers, user names, host names or addresses in output, profiles or fixtures.
8. A tool error is not a hardware failure. Report it as an error and let the core mark NOT A RESULT.

## 10. Mapping from today

| Today | Becomes |
|---|---|
| `run`, the verdict, held-out cores | Test backend `cinebench` (or any `[load]` command) |
| `rank` | Helper used by the `cpu` domain backend's plan |
| `sensors`, `watch`, helper task | Core sensors |
| `whea` | Core WHEA reader, used by `classify` and `blame` |
| `ledger.csv` | Core ledger, plus `runs\*.json` |
| GPU heat | Core load used by CPU tests on shared loops |
| Harness `ScewinBackend`, `ManualBackend`, `SimMachine` | Writers `scewin`, `manual`, `sim` |
| Harness `Engine` and `rules.py` | Core loop and step rules |
