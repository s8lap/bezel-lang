# bzcore L1 — the ownership kernel as an executable rule-change checker

`bzcore.py` grows `docs/research/soundness/bzcore/` (the locals-only prototype, kept beside it
as `bzcore_locals_v0344.py`) to proposal 0047's full Layer 1 and turns it into a tool for use
case 1 of the soundness report: *before amending §28, re-run the model on the proposed rule
and read what changes.* Spec pin: **v0.345**. No dependencies; Python 3.11; one file.

Everything below was produced by the commands in `run_all.sh`; their outputs are in `out/`.

## 1. What the model now covers

The checker is written clause by clause from §28.3.1; every branch of `check_body` names the K
clause it implements, so it can be read against the kernel rule for rule. Over 0047 §3.1's
A-normal fragment with variables `a b c` and two classes (`R` with one `owned f: Q` field; the
leaf `Q`):

| area | constructs | kernel clauses |
|---|---|---|
| locals (the prototype's fragment) | `new decl brw move asg asgb disp send use`, `if`, blocks | K1 (states, incl. the auxiliary *explicitly-disposed* and *uninitialized*), K1.3, K3.3/K3.4/K3.9/K3.11, K3.13, K3.14, K4.1/K4.2, K5.1/K5.2/K5.3/K5.5, K7.1–K7.5, §28.2's warning, §28.3.3 state match and adoption |
| marked reassignment | `asgm` (`x := move y`) | K3.4 assignment form + K3.11 |
| `using var` pins (A5, §28.10) | `using usingm usingb` | adoption by `move`; the unmarked-owned rejection; the borrow-pin; non-reassignable, never movable out; explicit `Dispose()` of a pin admitted (§28.6) |
| fields (K9) | `fst fstm fbrw fmove` | K9.1 storage reads lend with the holder as source; the replacing `move` yields the displaced value owned with no intermediate state (K9.2) and disposes nothing (K9.5); K3.1 stores are frozen (K5.1); slot overwrites are not (K5.6) and are flag-backstopped; the destructor cascade |
| unbound temporaries (K8) | `tuse tdisp tsend tbrw` | K8.1 boundary disposal; K3.9 on a temporary with reclaim-only boundary (§28.3.11); K8.4 reject-not-extend |
| exceptions (K3.8, K3.15, §31.1) | `throw`, `tthrow`, `try … catch e` | `throw` is a keyword-announced, unfrozen consume; the exception is disposed after the arm (or at termination); `e` is a borrow scoped to the arm; every scope on the unwind edge runs the same cleanup as the normal edge; the handler's entry state is the K7 meet of every throw site; K7.5 absorption |
| loops | `while *` with one back edge | K7 at the back edge (fixpoint over the meet); K7.4's "no flow around a back edge into a use" |
| borrow escape | `asgb` into a wider scope | K2.3 for statically comparable scopes (locals and the catch binding) |
| the dominance the §28.2 warning reads | `Γ['__killed']` | K4.2 + K7.4: the explicitly-killed values on *every* path (intersected at joins) — the input of K5.3-a/b |

The machine's heap carries K4.1's flag (`live | disposed | foreign`, plus `freed` in the
#4602 variant); `send` moves an object (transitively through its slot) to a domain where no
flag is ever set (K4.3's domain clause). Scope-end and unwind disposal are driven by the
**static** classification the checker computed, as a compiler's emission would be, with the
per-path runtime tag consulted only for *maybe-consumed* — which is what lets a join mutation
show its leak and what makes the #4602 "release site reads the flag" question answerable.

Verdicts are three-valued (accept / accept-with-warnings / reject:rule). Runs end `ok`,
`FAULT` (checked-mode `DisposedAccessException`, the backstop doing its job), `RACE` (a read
of an object another domain owns), `STUCK` (a read of a moved-from binding), or `UAF`
(free-in-full variant only). The oracles:

- **T1 exactly-once** — every allocation that stays in this domain (the `Q` inside every `R`
  included) has exactly one `disp` event on every complete path, and the machinery never
  disposes what another holder owns (`static-mismatch`).
- **T2 / T3** — no `STUCK`, no `RACE`.
- **Trace oracle** — the full disposal trace, order included (allocation, first disposal,
  idempotent no-op reads of a set flag by site kind, release-without-call, free). Two mutations
  in the table change no verdict and are visible only here.

## 2. How to run it

```
python3 -I bzcore.py selftest                      # 42 worked shapes with expected verdict, run outcomes and trace
python3 -I bzcore.py enumerate [bounds]            # program count, verdict histogram, violations, release-site flag reads
python3 -I bzcore.py mutate    [bounds]            # the mutation table (17 rows) at these bounds
python3 -I bzcore.py check --mutation "K5.3-b: dominated" [bounds]      # the rule-change report
python3 -I bzcore.py check --define examples/mutation_k52_unfrozen_when_dead.py [bounds]
python3 -I bzcore.py retention [bounds]            # the #4602 free-in-full oracle
python3 -I bzcore.py mutations                     # the table's names
./run_all.sh                                       # everything in out/ (two lanes, ~1 h on 4 cores)
```

Bounds: `--vars N` (2–4), `--len N` (straight-line length, and the prefix budget for a shaped
program), `--features all|locals|<comma list of locals,asgm,pins,fields,temps,throw>`, and the
shapes `--if`, `--try`, `--while` (prefix 1..len−2, the construct, suffix 0..1; arms and
handlers ≤1 statement and declare nothing; try/while bodies 1..`--body` statements, may
declare, may end in a throw). `--features locals` reproduces the prototype's grammar exactly
(308,526 programs at vars 3 / len 5; 67,002 with `--if` at len 4). Enumeration is
deterministic and prunes only ill-formed programs (undeclared reads, redeclarations, type
mismatches between `R` and `Q`, dead code after a throw); nothing is pruned by verdict.
Memory: a sweep holds one record per program (~150 B); 2 M programs ≈ 400 MB.

`check` prints newly accepted / newly rejected / newly unsound (with minimal witnesses), the
fault profile of the newly rejected set under the baseline (every execution / some / never),
how many of them already carried a §28.2 warning, the warning-only and trace-only changes, the
run-time residual (accepted programs that still fault) and a paste-ready *Evaluation* block.
A new rule is a Python file: `RULES_OVERRIDE` flips knobs in `RULES`; `HOOKS_OVERRIDE`
replaces a clause's predicate (`frozen(ids, G, live)` for K5's criterion, `meet(a, b)` for a
K7 cell) — `examples/mutation_k52_unfrozen_when_dead.py` shows the shape.

## 3. Program representation, and the printer interface the next lane needs

A program is a Python list of tuples (the docstring of `bzcore.py` is the table); nested
bodies are lists; a statement's **site** is its position path (e.g. `(2, 's', 0)` = the first
statement of the try body of the third statement), which also names the value a producer at
that site mints. The printer lane needs, per program: `show(prog)` (one line per statement,
the key the sweeps use), the statement table, and the following decisions the model makes
that a `.bzl` rendering must match:

- Two classes: `public class R` with `public owned f: Q` initialised by a constructor taking
  `owned q: Q` (`new x` prints as `new R(new Q())`), and `Q` with no fields; both with a
  `destructor` printing `disp <id>` (0047 §5.3's instrumentation); the id is the allocation
  order the machine assigns (`Q` before its `R`, so `new a` is `Q#1, R#2`).
- The body is a static method **of class `R`** (so `x.f` and `move x.f with …` are read by the
  declaring type: §28.6 and K9.3 admit the field forms there, and the model admits them on any
  readable holder, owned or borrowed).
- `*`-branches are specialised per path at generation time (0047 §5.3): one module per path,
  or a `bool` parameter; `while *` runs 0, 1 or 2 iterations.
- `send x` is `Domain.Spawn(W, move x)`; `tsend` the same with a fresh argument; `catch e` is
  `catch var e: R =>`; `throw x` throws an `R` (make `R` extend `Exception` in the rendering,
  or use a `Q`-free exception class — the model only needs "owned, thrown, disposed after the
  arm").
- Expected verdict comparison is at rule-family granularity (0047 §5.3, Q3): the model's
  `reject:<rule>` names the family (`K5.1-frozen-transfer`, `K7.3-mixed-merge`, `state-match`,
  `K1.3-read-of-moved`, `K2.3-borrow-outlives-source`, `§28.10-…`, `A5-…`, `K8.4-…`,
  `§28.6-dispose-of-borrow`, `K3-consume-of-non-owned`); the model's warnings are
  `§28.2-use-after-dispose`, `§28.2-second-dispose` (and, under K5.3-a, `K5.3a-…`). The
  must-use lint (K3.14) is not modelled, so a plain-`var` auto-drop warning on the compiler side
  must be filtered before the three-valued comparison.
- The trace to compare: the `disp` events of the run for that path, in order; the `noop`
  events are compiler-internal (a no-op call) and are not observable through destructors.

## 4. Modelling decisions that are readings of the spec (not findings)

- **§28.2's letter on consumes.** "Under a dominating kill every use in the list above draws
  this warning — a consume exactly as a read does, `move x` and an `owned` argument position
  included." The prototype rejected `disp a; move b a` as a consume of a non-owned binding; the
  model now warns and admits it, the new holder owning a logically disposed object whose
  release is a no-op (this is where 50,529 → 54,195 accepted programs on the prototype's
  grammar comes from). A repeated `Dispose()` is the §28.2 no-op at run time and the warning
  at compile time; the prototype faulted on it.
- **`using var` and explicit `Dispose()`.** §28.6 lists a `using var` among the receivers an
  explicit call may dispose; the pin's scope end then emits nothing (K1's explicitly-disposed
  post-state). Unmarked `using var y := x` over an explicitly-disposed `x` is rejected like the
  owned case (an owned-state LHS family, §28.3.3 rule 3).
- **The replacing `move` through a borrowed holder** is admitted (§28.8.1 states no receiver
  classification); its result is tainted for the retention oracle when a borrow of the holder is
  live at the exchange (§28.8.1: "a borrow of the displaced value taken before the exchange is
  bounded afterwards by the taker, under the posture every field or slot overwrite already has").
- **An overwrite of an explicitly-disposed binding under a live borrow** is admitted (K5
  freezes a consume of an *owned* binding; the displaced value is already flagged, so the
  surviving borrow faults deterministically — probe A in the adversarial pass).
- **The catch binding** is a borrow of the in-flight exception scoped to the arm (§31.1); the
  handler's entry state is the meet of the states at every throw site inside the protected body
  (bindings the body declared are gone, unwound). A handler without a throw site is dead code
  and is not checked. `DisposedAccessException` is not caught by `catch e: R` (it is not an
  `R`), so a FAULT always ends the run.
- **Allocation-site value identity.** The checker names values by the site that produced them
  (so loop fixpoints converge); `Γ['__killed']` and borrow sources are sets of sites. This
  over-approximates a kill inside a loop as a kill of every value the binding may hold at that
  point, which is the direction that makes K5.3-b *reject more*, never accept more; the fault
  profile in §6 measures how much.
- **Condition 2 of #4602** ("no later release of the same binding reads the flag") is read as:
  no release site of the binding that is textually later, or inside the same enclosing loop, is
  in the *maybe-consumed* static state (where the compiler must run the idempotent disposer).
  Statically explicitly-disposed release sites — scope end, an overwrite, a dominated repeat
  call — emit nothing, as the ruling says.

## 5. Limits

- Fragment: two classes, one field, locals `a b c`, no calls/methods (K2.4 announcement is
  not exercised; 0047's `call m(…)` with annotated signatures is the one L1 production left
  out), no collections, spans, frames, closures, generics, statics, nullability (`decl` is the
  only uninitialized form; no `null`), no `return`/`break`/`continue`, no rethrow, no K9.5
  throwing-RHS or throwing-disposer edge (the RHS is always a `new`), no must-use lint. The
  v0.180 `send` sink is kept as the one-sink T3 instance from the prototype; Layer 2 proper
  (channels, interleavings) is not modelled.
- `catch` catches only the model's thrown `R`; a checked-mode fault is never caught.
- The enumerator's bounds are small by design (the historical bugs live at sizes 3–5); the
  `try` shape is the expensive one (handlers over `a b c e`), so its recorded bounds are vars 2 /
  len 3 / body 2. Loops are bounded to one back edge and ≤2 iterations in the machine.
- "Never faults" in the fault profile means no path of the program reaches a member access
  through the stale reference — it includes programs that create a stale borrow and never read
  it, and `send` of a disposed object (the other domain's fault is outside the model).
- The model finds counterexamples within its bounds; it proves nothing.

## 6. Results

### 6.1 Baselines (0 violations everywhere)

| bounds | programs | accepted | §28.2-warned | fault every / some / never | release-site flag reads | time |
|---|---|---|---|---|---|---|
| locals, vars 3, len 5 (the prototype's grammar) | 308,526 | 54,195 | 9,426 | 2,886 / 0 / 51,309 | 3,489 | 5 s |
| locals, vars 3, len 4, `--if` | 67,002 | 12,657 | 780 | 132 / 132 / 12,393 | 2,157 | 1.4 s |
| **all, vars 3, len 4** | 340,036 | 71,152 | 3,804 | 2,142 / 0 / 69,010 | 1,083 | 6 s |
| **all, vars 3, len 4, `--if`** | 1,912,456 | 287,254 | 11,526 | 3,015 / 4,002 / 280,237 | 28,182 | 65 s |
| **all, vars 2, len 3, `--try --while --body 2`** | 1,254,922 | 347,088 | 23,616 | | 7,028 | 34 s |
| all, vars 2, len 3, `--if --try --while` | 1,263,466 | 350,174 | 23,668 | 13,136 / 24 / 338,014 | 7,286 | 64 s |
| locals+fields, vars 3, len 5 | 832,158 | 169,632 | 27,918 | | | 6 K4.3 trust residuals (F1), 0 violations |

"Release-site flag reads" counts accepted programs in which some release site (scope end,
overwrite, unwind, catch-done, cascade, termination) or a repeated `Dispose()` runs the
idempotent disposer on an already-flagged object — the sites that force header retention under
#4602 (§6.4). The prototype's 50,529 → 54,195 and 450 → 780 §28.2-warned are the §28.2-letter
change (§4). The rejection histogram (`out/enum_*.txt`) is itself a check on the text: e.g. at
all / vars 3 / len 4 — 95,577 K1.3, 81,336 K8.4, 36,852 + 13,911 A5, 26,154 §28.10, 6,636
state-match, 6,210 consume-of-non-owned, 1,788 §28.6, 294 K5.1, 126 K5.2; with try/while,
2,184 K2.3.

### 6.2 The mutation table, all features, vars 3, len 4 (340,036 programs, 71,152 accepted)

| mutation | verdicts changed | newly accepted | of which unsound | newly rejected (every/some/never fault) | unsound among all accepted | warnings / trace only | witness |
|---|---|---|---|---|---|---|---|
| K5.1 deleted (transfers unfrozen) — the pre-v0.180 language | 90 | 90 | 54 | 0 | 54 | | `new a / brw b a / send a / brw c b` → RACE |
| K5.2 deleted (drop-on-overwrite unfrozen) | 54 | 54 | **0** | 0 | 0 | | `new a / brw b a / asg a / brw c b` (flag-backstopped) |
| K5.3-c: explicit Dispose() frozen under any live borrow | 108 | 0 | 0 | 108 (60/0/48) | 0 | | `new a / brw b a / disp a / brw c b` |
| K5.3-a (warning only) | 0 | 0 | 0 | 0 | 0 | warned +108 | |
| **K5.3-b (both halves)** | 3,597 | 0 | 0 | 3,597 (2,130/0/1,467) | 0 | | `new a / disp a / brw b a` |
| K5.3-b, borrow half only | 108 | 0 | 0 | 108 (60/0/48) | 0 | | `new a / brw b a / disp a / fbrw c b` |
| K5.3-b, same-variable half only | 3,489 | 0 | 0 | 3,489 (2,070/0/1,419) | 0 | | `new a / disp a / brw b a` |
| K7.3 weakened (owned ⊓ borrowed → borrowed) | 0 | 0 | 0 | 0 | 0 | | (needs a branch: 144 / 144 unsound with `--if`, locals) |
| K7.4 weakened (owned ⊓ moved → owned) | 0 | 0 | 0 | 0 | 0 | | (needs a branch: 3,324 / 3,729 unsound with `--if`, locals) |
| K1.3 deleted (moved-from readable) | 68,211 | 68,211 | 68,211 | 0 | 68,211 | | `decl a / brw b a` → STUCK |
| K3.13 flipped (an unmarked read consumes) | 8,220 | 3,204 | **0** | 5,016 (96/0/4,920) | 0 | trace changed in 1,158 | a different language, equally sound |
| release sites of an explicitly-disposed binding run the disposer again (K3.9 post-state ignored) | 0 | 0 | 0 | 0 | 0 | **trace changed in 8,412** | `new a / disp a` |
| §28.2 warning dropped | 0 | 0 | 0 | 0 | 0 | warned −3,804 | |
| K8.4 deleted (a borrow of a temporary may escape) | 26,853 | 26,853 | **0** | 0 | 0 | | `tbrw a` (flag-backstopped: K8.4 is K2.3's *statically comparable → reject* posture, not a soundness clause) |
| A5 deleted (a using var reassignable / movable) | 26,667 | 26,667 | **0** | 0 | 0 | | `using a / asg a` (A5 is declared intent, as the register says) |
| K3.15 deleted (no disposal on the exception edge) | 0 | 0 | — | 0 | **4,368** | trace changed in 4,590 | `new a / tthrow` → T1-leak (every verdict unchanged: only the trace oracle sees it) |
| K9.5 flipped (dispose the displaced value before the RHS) | 0 | 0 | 0 | 0 | 0 | trace changed in 17,232 | `new a / asg a` (order only; the throwing-RHS edge is outside the fragment) |

The locals-only re-run (`out/mutate_locals_v3l4_if.txt`, 67,002 programs) reproduces the
prototype's rows exactly where the grammar is the same — K5.1 78 / 72 unsound (36 in the
prototype: the §28.2-letter change admits more sends of disposed values, which are RACE-free
but counted here through re-borrows), K5.2 72 / 0, K5.3-c 60 (6/24/30), K7.3 144 / 144, K7.4
3,324 / 3,729 — and adds K5.3-b: 675 newly rejected (132 / 108 / 435). The `--if` and
`--try --while` tables at all features are in `out/mutate_all_v3l4_if.txt` and
`out/mutate_all_v2l3_try_while.txt`.

Reading: four clauses carry soundness in this fragment (K5.1, K7.3, K7.4, K1.3 — and K3.15 on
the exception edge, visible only in the trace); K5.2, K5.3, K3.13, K8.4 and A5 carry
readability or decidability — every new acceptance under their deletion is flag-backstopped,
which is exactly what each clause's own text claims.

### 6.3 K5.3 a/b/c — the numbers for the proposal (paste-ready)

```
Model: bzcore L1 (`docs/research/soundness/bzcore-l1/`), spec v0.345; bounded exhaustive
enumeration over 0047 §3.1's Layer 1 (locals, one owned field with the replacing move,
unbound temporaries, throw/try with edge parity, using var pins, branches, one-back-edge loops);
oracle: the K4.1 flagged-heap machine, every path.

Bounds A — all constructs, 3 variables, ≤4 statements, straight-line: 340,036 programs,
71,152 accepted, 3,804 with a §28.2 warning, 0 T1/T2/T3 violations.
  (a) warning only:   0 verdicts change; 108 programs gain a warning (all silent today, all a
                      use through a borrow taken before the dominating kill).
  (b) errors:         3,597 newly rejected, 0 newly accepted, 0 newly unsound.
                      Of these 2,130 fault on every execution, 0 on some, 1,467 never reach a
                      member access through the stale reference (they create a dead borrow or
                      move/send the disposed value and never read it).
                      3,489 carried the §28.2 warning today (same-variable half); 108 were
                      silent (borrow half, e.g. `new a; var b := a; a.Dispose(); var c := b.f;`).
                      Run-time residual after (b): 12 accepted programs still fault on every
                      execution — all of the K5.6 shape `var b := a.f; a.f := new Q(); b.M()`
                      (a slot overwrite, not an explicit kill; the flag stays the backstop).
  (c) Dispose() frozen under any live borrow: 108 newly rejected (60 / 0 / 48), 0 unsound;
                      identical to (b)'s borrow half at this size, i.e. (c) rejects exactly the
                      programs (b) rejects but also the 48 that never fault — (c) is the
                      over-rejection, (b) adds nothing to it except the same-variable half.
Bounds B — all constructs, 2 variables, ≤3-statement prefix, with a branch, a try/catch and a
loop (bodies ≤2): 1,263,466 programs, 350,174 accepted, 23,668 §28.2-warned, 0 violations.
  (b): 21,146 newly rejected (13,136 every / 24 some / 7,986 never), 0 newly unsound;
       all 21,146 carried a §28.2 warning today (the borrow half needs a fourth statement).
       The 24 "some" are conditional-on-the-other-arm shapes such as
       `new a; if * then a.Dispose() else throw a end; var b := a.f;`.
       Run-time residual: 13,136 + 24 faulting programs at the baseline → 0 + 0 under (b).
  (a): 0 verdicts change; (c): 0 at this size.
Bounds C — all constructs, 3 variables, ≤4 statements, with a branch (the full table is
out/mutate_all_v3l4_if.txt): 1,912,456 programs, 287,254 accepted, 11,526 §28.2-warned,
0 violations.
  (b): 10,695 newly rejected (2,991 every / 3,408 some / 4,296 never), 0 newly accepted,
       0 newly unsound; borrow half 270 (60 / 90 / 120, all silent today), same-variable half
       10,425 (2,931 / 3,318 / 4,176, all §28.2-warned today).
  (a): 0 verdicts change; 270 programs gain a warning.
  (c): 1,170 newly rejected (72 / 594 / 504), 0 unsound — four times (b)'s borrow half:
       (c) also rejects every conditional-kill shape (`var b := a; if * then a.Dispose() end;
       b.M()`), which (b) deliberately leaves to checked mode.
  For comparison at the same bounds, the clauses that carry soundness: K5.1 deleted → 702
  newly accepted, 666 unsound; K7.3 weakened → 744 / 744; K7.4 weakened → 38,214 / 43,614;
  K3.15 deleted → 63,582 leaking traces with no verdict change.

Mapping onto the §28.2-warned set: at bounds A the §28.2-warned set is 3,804; (b) turns 3,489
of them into errors; the 315 that stay warnings are exactly the repeated-Dispose() programs
(the carve-out). Dropping the §28.2 warning altogether (the prototype's 450 → 177 row) now
reads 3,804 → 0 at these bounds: the repeated-Dispose() warning is §28.2's own and falls with it.
Back-edge residual (measured, loops bounded to 2 iterations): `new a; var b := a;
while * do b.M(); a.Dispose(); a := new R(); end` stays accepted under (b) and faults on the
second iteration only (runs: ok, ok, FAULT) — the conditional-kill and back-edge residual the
rule leaves to checked mode, as its text says.
```

Stability across bounds (`out/stab_K5_3_b_*.txt`, all constructs) — K5.3-b keeps its shape at
every bound: 0 newly accepted, 0 unsound, the newly rejected split into every-execution faults
and never-faulting stale references, with a "some execution" slice only once branches exist:

| bounds | programs | accepted | §28.2-warned | (b) newly rejected | every / some / never |
|---|---|---|---|---|---|
| vars 2, len 3 | 3,308 | 1,336 | 32 | 28 | 16 / 0 / 12 |
| vars 3, len 3 | 11,704 | 3,991 | 72 | 66 | 36 / 0 / 30 |
| vars 2, len 4 | 53,870 | 14,912 | 1,062 | 932 | 588 / 0 / 344 |
| vars 3, len 4 | 340,036 | 71,152 | 3,804 | 3,597 | 2,130 / 0 / 1,467 |
| vars 3, len 3, `--if` | 30,259 | 10,216 | 189 | 174 | 36 / 54 / 84 |
| vars 2, len 4, `--if` | 406,474 | 76,390 | 4,062 | 3,566 | 974 / 1,314 / 1,278 |
| vars 2, len 3, `--if --try --while` | 1,263,466 | 350,174 | 23,668 | 21,146 | 13,136 / 24 / 7,986 |

The ratio newly-rejected / §28.2-warned stays at 0.85–0.95 (the remainder are repeated
`Dispose()` programs); the never-faulting share is 36–48% everywhere, and is made of borrows and
moves of a disposed value that are never read through.

### 6.4 The #4602 trace oracle: free-in-full

For every accepted program with an explicit `Dispose()` site, the machine was re-run with a
heap that reclaims the object **in full** at the sites a given rule admits, after which any read
of its flag (a member access through any alias, a release site running the idempotent disposer,
a cascade, a repeated call) is a use-after-free. Rows are variants of the ruling's conditions;
the column is the number of *accepted* programs that read freed memory.

| free-in-full rule | all, v3 l4 (24,403 programs with a Dispose site) | locals+fields, v3 l4 `--if` (8,796) | all, v2 l3 `--if --try --while` (139,199) | locals+fields, v3 l5 (`out/retention_fields_v3l5.txt`) | all, v3 l4 `--if` (117,403) | witness |
|---|---|---|---|---|---|---|
| free at every explicit Dispose() | 2,835 | 5,460 | 17,352 | 20,634 | 33,012 | `new a / disp a / fbrw b a` |
| condition 1 as worded (no live borrow of the value) | 2,775 | 5,202 | 17,352 | 19,260 | 31,962 | `new a / disp a / fbrw b a` |
| condition 2 only (no later maybe-consumed release) | 2,835 | 1,200 | 17,076 | 20,634 | 8,562 | |
| **the ruling: conditions 1 and 2 as worded** | **2,775** | **1,170** | **17,076** | **19,260** | **8,412** | `new a / disp a / fbrw b a`; `new a / disp a / move b a` |
| 1 + 2 + the disposed binding itself is dead after the call | **0** | **0** | **0** | **6** | **0** | `new a / fbrw b a / fmove c a / disp c / use b` |
| 1 + 2 + binding dead + slot-displaced values retained | 0 | 0 | 0 | **0** | 0 | |
| all but condition 1 | 60 | 30 | 0 | 948 | 150 | `new a / brw b a / disp a / fbrw c b` |
| all but condition 2 | 0 | 3,906 | 276 | 0 | 23,190 | `new a / if {} else { disp a }` (scope end, overwrite and unwind read the flag) |

What the rows say:

1. **Condition 1 as worded is not sufficient.** "Where it proves no borrow is live, the object
   is reclaimed in full" frees the object under every §28.2-warned same-variable use —
   `a.Dispose(); a.M()`, `a.Dispose(); var b := a.f;`, `a.Dispose(); var b := move a;` (b's scope
   end then reads a freed flag), `a.Dispose(); throw a` (the catch arm and the catch-done
   disposal read it). These programs compile today with a warning and fault deterministically in
   checked mode; under the ruling as worded they read freed memory. The fix is one clause: the
   disposed **binding** itself must be dead after the call (no later use of it on any path) — the
   same liveness, applied to the binding rather than to borrows of it — or K5.3-b, which makes
   every such use an error except the repeated `Dispose()`, which the ruling already elides.
   With that clause the count is 0 at every bound tried.
2. **Condition 2 is necessary and, read as "no later release site in the maybe-consumed
   state", sufficient** in the fragment: dropping it leaves 3,906 / 276 programs whose scope end,
   overwrite or **unwind** (a throw after a conditional kill — `new a; if * then … else
   a.Dispose() end; throw new R()`) runs the idempotent disposer on the freed block. The unwind
   edge is a release point the ruling's list of three readers does not name; it is K3.15's edge
   parity applied to the same site.
3. **The slot-displaced alias** (§28.8.1: "a borrow of the displaced value taken before the
   exchange is bounded afterwards by the taker, under the posture every field or slot overwrite
   already has") is invisible to the K5 liveness condition 1 names: in `var b := a.f; var q :=
   move a.f with new Q(); q.Dispose(); b.M();` no live borrow has `q` as a source, yet `b` reads
   `q`'s object. The retention comment's `slot-borrowed` bit (its case 2) is the needed fallback;
   the model's "slot-displaced values retained" row is that bit. At ≤4 statements the shape does
   not fit (the rows coincide); at vars 3 / len 5 on the fields fragment, conditions 1 + 2 + the
   dead-binding clause still leave **6** programs reading freed memory (the witness above), and
   the slot bit brings them to 0.
4. The retained-heap baseline shows how many accepted programs force retention at all: 1,083
   of 71,152 (all, v3 l4) read a set flag at a release site, by site kind
   `scope-end 528, explicit repeat 405, termination 135, unwind 12, cascade 12, overwrite 6`.

### 6.5 Spec findings

**F1 — T3 does not hold unconditionally on Layer 1 (the replacing `move` + a slot borrow +
transfer).** `new a / fbrw b a / fmove c a / send c / use b`, i.e.

```bezel
var a := new R(new Q());
var b := a.f;                          // K1.5 / K9.1: a borrow whose source is the holder a
var c := move a.f with new Q();        // K9.1: b now aliases c's object; b's source set is still {a}
Domain.Spawn(W, move c);               // K3.3: not frozen — no live borrow names c (K5's criterion is over sources)
b.M();                                 // a read of an object another domain owns: no flag is ever set
```

is accepted by the kernel as written and is a T3 violation (RACE) in the machine. The spec
knows this shape and admits it as **recorded trust**, not as a checked rule: §28.3.9 *Scope*
clause 1 — "A slot kill *by move* … into a transfer, a spawn argument, or one of the
domain-reachable constructions of the domain clause — disposes nothing and sets no flag, and is
governed by the borrow lifetime rules' domain clause above (K4.3), a recorded obligation, not by
this exemption" — and §28.8.1 ("bounded afterwards by the taker … the domain clause, K4.3").
So this is not a defect of the text; it is a defect of **proposal 0047 §2.3's reduction
argument**, which excludes borrow-holding storage from v1 on the ground that "excluding them
makes v1+L2's T3 *unconditional on its fragment*": with owned fields, the replacing `move` and a
local borrow of the slot's value, T3 is already conditional on the K4.3 clause inside Layer 1.
The model classifies these programs as `K4.3-trust-residual` (counted separately from
violations): 6 of 169,632 accepted programs at locals+fields / vars 3 / len 5, none at ≤4 statements. Recommendation for 0047: state T3
for L1 as conditional on the slot-kill-by-move clause, or add the slot-displaced alias to K5's
source tracing (then the `move c` above is frozen — the model's `slot` taint is that rule, and it
over-approximates: it also freezes when the live borrow is of the holder itself rather than of
the slot).

**F2 — the #4602 ruling's condition 1 as worded** (§6.4 item 1): "no borrow survives" must be
read as "no borrow *and no later use of the binding itself* survives", or be paired with
K5.3-b's same-variable error; otherwise every §28.2-warned program reads freed memory in checked
mode, and the deterministic-fault promise of §28.2 is lost exactly where the warning says the
fault will happen.

**F3 — the unwind edge is a flag reader** (§6.4 item 2): the ruling lists scope end, overwrite
and repeated `Dispose()` as the later sites that read the flag; a `throw` after a conditional
kill is a fourth (K3.15: the same cleanup on the exception edge).

**F4 (model-level, for the prototype's successor)** — the prototype rejected `a.Dispose();
var b := move a;` as a consume of a non-owned binding and faulted on a repeated `Dispose()`;
§28.2's text makes both a warning and the repeat a run-time no-op. The prototype also lacked
K2.3's local-scope rule, so `var c: Q; do var b := new Q(); c := b; end do; c.M();` was an
accepted faulting program rather than a rejection. Both corrected here; both move the
§28.2-warned and fault-profile numbers a proposal would quote.

No accepted program violates T1 or T2 at any bound; no accepted program violates T3 except the
K4.3-admitted shape of F1.

## 7. Adversarial pass

- **Clause-by-clause:** each branch of `check_body` cites the K id; the selftest's 42 shapes
  pin one clause each with the expected verdict, per-path outcomes and (for the disposal-order
  rules) the exact trace. Sixteen further hand-written probes (`out/probes.txt`) cover the
  interactions the enumerator's bounds do not reach: overwrite of a disposed binding under a live
  borrow, slot-displaced aliases, disposed-then-thrown, nested try with a fresh throw from the
  arm, a loop-carried borrow, pins, catch-binding escape (which exposed the missing K2.3 and is
  now rejected), F1.
- **Bounds are stated** on every output line (`bounds:`), and the enumerator prunes only
  ill-formedness.
- **Stability across bounds:** the K5.3 rows keep their shape — 0 newly accepted, 0 unsound,
  every newly rejected program either faults on every execution or never reaches a member
  access — at every bound tried (vars 2–3, len 3–4, with and without `--if`, `--try`,
  `--while`); the absolute counts scale with the program count, and the borrow half needs ≥4
  statements to appear (see §6.3's stability table). The freeze and join rows need the same
  sizes they needed in the prototype (K5.1/K5.2/K5.3-c ≥4 statements; K7.3/K7.4 a branch),
  which is why the small-bound mutation tables show zeros for them.

## 8. Files

- `bzcore.py` — the model (checker, machine, oracles, enumerator, mutation harness, CLI).
- `bzcore_locals_v0344.py` — the prototype this grew from, untouched, for diffing.
- `examples/mutation_k52_unfrozen_when_dead.py` — the `--define` interface (a `frozen` hook);
  at the recorded bounds it changes no verdict, which is itself the report one wants from an
  innocuous rewording (`out/check_define_example.txt`).
- `examples/probes.py` → `out/probes.txt` — the adversarial probes of §7.
- `run_all.sh` → `out/*.txt` — every number in this file; `out/selftest.txt` is the 42-shape
  selftest transcript.
