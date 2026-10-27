# bzdiff — the model-vs-compiler differential (bzcore L1 vs `bezelc`)

Use case 2 of `docs/research/soundness/README.md` §3.2 and proposal 0047 §5.3's validation
bridge: every program of the executable ownership model (`docs/research/soundness/bzcore-l1/bzcore.py`, Layer 1
of 0047, spec pin v0.345) is printed as a real Bezel module, compiled by `bezelc`, and the two
sides are compared at two levels — the three-valued verdict with its rule family, then the
disposal/use trace and the run outcome of every path.

Findings: [`FINDINGS.md`](FINDINGS.md). Draft issues: `drafts/*.json`.

## Files

| file | what |
|---|---|
| `bzlprint.py` | the printer: model program → `.bzl` module (one module per program; paths driven by a command-line digit string). Its docstring states every rendering decision. |
| `bzdiff.py` | the harness: enumerate → model verdict (spec config and `r35-gaps` config) → compile checked → level 1; both accept → run every model path on the checked build, compare trace + outcome (level 2); then the unchecked build as supplementary evidence. One JSON line per program in `<out>/results.jsonl`. |
| `families.py` | the rule-family mapping (model rule → family; compiler diagnostic text → family, regex table; warnings the model does not express). |
| `model/bzcore.py` | a verbatim copy of `docs/research/soundness/bzcore-l1/bzcore.py` plus one knob (`S28_10_reject_unmarked_pin`) so the harness can also judge under r35's known, open spec gap #5582 (`diff` against the original shows the two hunks). The original is never modified. |
| `triage.py` | groups disagreements by signature with minimal witnesses and per-sweep agreement rates. |
| `show_one.py` | prints, compiles and runs one program given in the model's one-line syntax. |
| `issue_search.py` | regex search over the pulled open-issue snapshot (`issues/open_issues.json`, REST, 2026-10-27). |
| `run_samples.sh` | the stratified sample sweep that produced the numbers in FINDINGS.md. |
| `runs/<name>/results.jsonl` | sweep outputs. |

## How to run

```
export BEZELC=/path/to/bezelc            # default: the r35 release build used for FINDINGS.md
python3 -I bzdiff.py selftest --out runs/selftest
python3 -I bzdiff.py sweep --features locals,pins --vars 3 --len 4 --sample 3000 --out runs/pins
python3 -I bzdiff.py sweep --features all --vars 2 --len 3 --try --while --no-fmove --sample 2000 --out runs/tw
python3 -I bzdiff.py summary --out runs/pins
python3 -I triage.py runs/*/results.jsonl
python3 -I show_one.py "new a / brw b a / usingb p b / send a / use p"
```

Bounds and `--features` are the model's (`locals`, `asgm`, `pins`, `fields`, `temps`, `throw`,
`--if`, `--try`, `--while`, `--body`). `--sample N --seed S` takes a deterministic reservoir sample
of the enumeration; `--no-fmove` drops programs with the replacing `move` (unparseable on r35,
#5923). `--jobs` bounds the concurrent compiles (3 by default; each compile+link is ~0.25 s, the
sweep runs ~20 programs/s). Binaries are deleted after their runs; `runs/` holds JSON only.

## What is compared

**Level 1.** The model's verdict (`accept` / `accept` with `§28.2` warnings / `reject:<rule>`)
against the compiler's (exit status; errors and warnings mapped to families by `families.py`; the
K3.14 must-use lint and the preamble's §22.12 static-storage warnings are filtered, as the model
does not express them). Two rejections agree when the model's family is among the compiler's
error families (a program breaking two rules may be reported under either); `reject-family-differs`
is recorded separately. `l1` is under the spec model, `l1_35` under the model with the known r35
gaps switched off.

**Level 2.** For every path the model enumerates (`run_all`), the checked build is run with the
path's choice vector as its argument; the `N:`/`D:`/`U:` lines are the trace (allocation, first
disposal, member access) and are compared **in order** with the model's `new`/`disp`/`use`
events. Outcomes: `ok` (prints `END`, exit 0), `FAULT` (`DisposedAccessException`, exit 70),
`uncaught` (the thrown `R` reaches `Main`, exit 70), `RACE` (a `U:` of an object the model's
path sent to the foreign sink), `crash(sigN)`. On a model `FAULT` the model trace must be a prefix
of the compiler's and the remainder disposals only (the compiler unwinds, the model halts). The
model's disposal of an uncaught exception at termination is excluded from the comparison and
counted (see FINDINGS §"what the differential cannot see").

**Unchecked build.** Run on the same paths; crashes and outcome changes are recorded under
`unchecked` (use-after-dispose is undefined there, so a crash is evidence, not a verdict).
