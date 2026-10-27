# Master plan — defect campaigns for the autonomous sessions

*Working paper, non-normative. Written 2026-10-27 against main `c7f491704` (spec v0.345), from a full
measurement of the tracker ([`MEASUREMENTS.md`](master-plan/MEASUREMENTS.md): 1613 open, 5339 closed) and the
landing record of the last 24 days. It replaces the July proposal DAG (`master-plan.md` as it stood,
archived as [`master-plan-2026-07.md`](master-plan-2026-07.md), §5) and absorbs the lesson of
[`defect-campaign-2026-09.md`](defect-campaign-2026-09.md), the last attempt at this. Nothing here opens,
closes or re-labels an issue by itself, and nothing here changes a rule in `CLAUDE.md`: where this plan
asks for one, it says so and the owner decides.*

**Owner's direction (2026-10-27):** *"Rather than attacking the entire defect surface in parallel, focus
on particular topics or features and ensure that we actually fix something properly at every step."*

---

## 0. Why a campaign plan, in numbers

- The tracker went from **555 open (week 38) to 1613 (today)**, ×2.9 in six weeks. Last week: **770
  opened, 297 closed**. The pool is not slow: it lands ~**47 fix commits a day** and closes an issue a
  median of **one day** after filing. The surface grows because intake runs 1.5–2.6× closure.
- **63% of the last fortnight's intake is produced by our own process**: review probes, lane matrices,
  "found while fixing". **59% of all open issues** describe themselves as a sibling, residual, twin,
  follow-up or pre-existing (26% on 2026-09-17). Each narrow fix is correct and reviewed, and is
  followed by a filed sibling. That is the loop the September plan named and did not break.
- The September plan got the method right (families by **shared mechanism**, anchors before instances,
  done = members close, verify-first, loop-amplified ASan rows, a falsification rule the I1 lane
  actually exercised). Its anchors landed in four days. **Its members did not close**: G 19/21 open,
  H 10/10, A 26/43, M 15/17, I2 12/13. It did not stick because nothing in a session's routine read it:
  it had no WIP limit, no intake rule, no entry re-measurement step scheduled after the anchors, no
  owner-decision batch, and a progress table only one lane ever updated. This plan is those five things.

---

## 1. How to use this plan (the coordinator's session routine)

A coordinator session follows these steps in order. Every step is a command or a table edit; none needs
judgement about *what* to work on — that judgement is made once, below, and revised only at a campaign's
exit review.

**1.1 Session start.**
1. Read §6 (the progress table). The **active campaign** is the row with `status = ACTIVE`. If none,
   take the top `QUEUED` row whose `blocked on` column is empty; set it `ENTRY` and run §2.2.
2. Run the campaign's label query (§1.6) to get the live member list:
   `gh api 'repos/s8lap/bezel-lang/issues?state=open&labels=campaign:<slug>&per_page=100&page=N'`.
3. Run the five-source claim check per member exactly as `CLAUDE.md` *Claims* says. Members whose repro
   no longer fails on staging are closed with the commit that fixed them named (not "cannot reproduce").
4. Dispatch lanes from the campaign's lane plan (§3), anchor lane first.

**1.2 WIP limit.** **One campaign is active per repository.** A second may be `ACTIVE` only when all
three hold: it is in the *other track* (§3: ownership/codegen-disposal vs sema/stdlib-surface), its lane
plan declares hot files disjoint from the first's, and the first cannot use the pool (its lanes are
serialised by the one-disposal-lane rule, or it is `blocked on` a ruling). Never three. When the owner
asked for "one, maybe two", this is the "maybe two": a second track, never a second front in the same
code.

**1.3 Lanes inside a campaign.** The pool stays at six (`CLAUDE.md`); the campaign decides what the six
do: one **anchor lane** (Opus, always) until the anchor lands and is reviewed; then **instance lanes**,
one per sub-family from the campaign's lane plan, each file-disjoint by declaration in its brief; a
**reviewer** per handback (review-contract); a **harvest lane** per 4–6 picks. A lane brief names the
campaign, the sub-family, its members, the shared code path, and the DONE instrument it must extend
(§2.3). The standing rule *"at most one disposal/ownership/async/iterator lane at a time"* serialises
most of the ownership track. **Owner-ruled 2026-10-27 (§4.7):** inside a campaign, once its anchor has
landed and been reviewed, ownership-track lanes may run concurrently when their declared hot-file sets
are disjoint; before the anchor lands, serialise.

**1.4 An issue outside the active campaign is triaged, not worked.** Triage = add exactly one
`campaign:<slug>` label from §3 (or `parking-lot` if none fits), nothing else. Triage happens in the
same turn the issue is seen (a lane filing a sibling labels it; a coordinator reading the tracker labels
it). Three exceptions, and only these, may be dispatched outside the campaign: (a) a gate FAIL fix-forward
(mandatory anyway); (b) a §3 *campaign 0* enabler the active campaign's DONE instrument needs; (c) an
explicit owner directive. "It looks important" is not an exception; it becomes a `parking-lot` or
campaign label and waits.

**1.5 Spec trains interleave at campaign boundaries.** Campaign lanes never amend `docs/`. A spec
question found in a campaign is filed with `spec-question`/`spec-defect` **and** the campaign label, the
affected hunk is held out (lane-contract rule 12), and the coordinator appends it to the campaign
record's *Questions for the next train* list. A train is requested from the owner when the active
campaign exits, when its question list reaches ten, or when a member is blocked on one. When a train
lands, **its new `spec-tracking` trackers are triaged in the same session**: those citing the active
campaign's sections join its backlog (and count in its DONE), the rest get a campaign label and wait.
Today's instance: the 14 v0.345 trackers (#7344–#7357) — #7354 and #7357 fall to `using-heads`, #7350
and #7353 to `nullable-carrier`; #7344 (a method reference to `Dispose`) and #7345 (explicit `Dispose()`
reclaims at the call) are candidates for `using-heads` / `producer-temporaries` that their entry checks
confirm; the other eight wait under `spec-trains` (Appendix B).

**1.6 Labels (the first coordinator session creates them with `gh label create`, a tracker edit of the
same kind as the issue filing lanes already do; it touches no file in the repo).**
One `campaign:<slug>` per row of §3, plus `parking-lot`. The plan's member lists are the initial labelling
(`docs/proposals/papers/master-plan/work/out/campaign-members.tsv`, 1613 rows, exclusive). A label is the only state
the tracker carries; everything else is in §6 and the campaign record.

---

## 2. The campaign model

**2.1 Definition.** A campaign is **one topic**: a set of spec sections, one or two shared code paths,
and the open issues whose mechanism lives there. It has a slug, a track, a record file
(`docs/proposals/papers/campaigns/<slug>.md`, created at ENTRY from the template in §2.5), a lane plan,
and a DONE instrument. It is not a label for a symptom ("leaks") and not a file ("everything in
`LlvmBackend_Setup.cpp`"): the September clustering rule stands — *members name the same predicate,
emitter or ordering*.

**2.2 Entry check (status `ENTRY`, one lane, Opus, read-mostly).** Before any fix:
1. **Re-measure every member on HEAD.** Each member gets a row: `#N | reproduces / moved / fixed-by
   <sha> / cannot-build-repro | MEASURED evidence`. Issues whose repro is stale are re-derived from the
   title's mechanism, not dropped. (Of four headline repros spot-checked for this plan, one — #3469 —
   has already moved from SIGSEGV to a compile-time rejection; see `MEASUREMENTS.md` §7.)
2. **Draw the sub-families by shared code path** (file and function, read at HEAD), and apply the
   September falsification rule: a sub-family whose shared site would close fewer than half its members
   is split, and the split is recorded.
3. **Name the anchor**: the one authority that, once it exists, makes every member either a missing arm
   of it or a different campaign. If the anchor is a design nobody has written, the entry lane writes it
   into the record (one page) and the coordinator sends it to the owner only if it changes a rule.
4. **Build the DONE instrument's skeleton**: the grid or matrix §2.3 names, as conformance cases or a
   generator, run once on HEAD to record the baseline (how many cells wrong today).
5. **List the rulings needed** (§4.8) and the rule-corpus `fail` rows in scope
   (`baseline.tsv` filtered by the campaign's sections).
The entry lane's handback is the first version of the campaign record. Only then does the status become
`ACTIVE`.

**2.3 DONE is measured, not declared.** A campaign is DONE when **all** of these print true:
- every member at entry is **closed by a `fix #N` commit** or **re-scoped** with one of three recorded
  reasons: *split to campaign X* (labelled), *blocked on ruling #R* (the ruling is filed and linked), or
  *not a defect* (the spec sentence that says so is quoted);
- every rule-corpus case citing the campaign's sections is `pass`, or its `fail` row carries an `issue:`
  header pointing at an OPEN issue labelled for another campaign or a ruling;
- the **DONE instrument is green**: the campaign's grid (face × representation, receiver × argument kind,
  head kind × binding form, …) runs under `BEZEL_AOT_FORCE_SANITIZE=address` with every cell *correct or
  rejected with the named diagnostic*, and never a wild jump, a leak or a compiler abort; the grid is a
  conformance set or a `scripts/regression_matrix.py` shape, so it runs at every gate afterwards;
- the relevant research-matrix cells (`docs/research/{borrow-flow,disposal,generics}-matrix`), where the
  campaign's sections have cells, are re-run and clean or re-filed;
- **no in-scope sibling filed during the campaign is left untriaged**: each is a member (closed) or
  labelled for a later campaign with its mechanism named;
- `scripts/check_leak_census.py`, the toolchain-skip ratchet and the campaign's own ratchet (if the anchor
  created one — e.g. `check_nested_body_scope_coverage.py`, `check_enumerable_grid_spellings.py`) are
  banked **lower**, not sideways.

**2.4 Exit review (status `EXIT`, a fresh reviewer, never the campaign's lanes).** Re-runs the DONE
checks with its own instruments, reads the record, and writes one paragraph per sub-family: what the
anchor was, what it closed, what it split, what it taught. The exit review names the **next campaign's
first lane brief** (the reviewer saw the neighbouring defects). The coordinator then flips §6 to `DONE`
or, if a DONE line fails, to `ACTIVE` with the failing line as the next lane. A campaign may also exit as
`RE-SCOPED` when the entry check or an anchor lane proves the topic is two topics; the record says why.

**2.5 Campaign record template** (`docs/proposals/papers/campaigns/<slug>.md`):
```
# Campaign <slug> — <title>
Track · Sections · Shared code paths (file:function @ sha) · Anchor (issue or one-page design)
## Members at entry (table: #, severity, HEAD status MEASURED, sub-family, lane, outcome)
## Sub-families and lane plan (hot files per lane; order; model tier)
## DONE instrument (what it is, where it lives, baseline cells wrong at entry)
## Rulings needed (issue, what it unblocks, asked on <date>)
## Questions for the next train (filed spec-question numbers)
## Siblings filed during the campaign (#, triaged to)
## Lane records (one block per handback: SHAs, rows, fail-without proof, matrix result)
## Exit review
```
The record is updated **in the same commit** as the pick that changes it (the September plan asked each
lane to do this; only one did — so it is now the integrator's step in `pick_stage.sh`'s turn, like
`harvest.tsv`).

---

## 3. The campaigns, ranked

**Ranking criteria, in order.** *Value*: memory-safety and crashes before wrong values before leaks
before diagnostics. *Leverage*: one authority that closes many members and stops the sibling stream.
*Risk*: how many hot files, how many lanes can run, whether a wrong anchor trades a leak for a UAF.
*Dependencies*: owner rulings, spec trains, the soundness research's recommended gate, other campaigns.

**Two tracks** keep the WIP-2 option honest. **Ownership track** (codegen disposal, ownership pass,
iterators, async, closures): campaigns here share `LlvmBackend_Setup.cpp`, `_Disposal.cpp`, `_Stmts.cpp`,
`Ownership*.cpp` and are serialised by the one-disposal-lane rule. **Sema/stdlib track** (sema admission,
builtin arms, nullable, generics, cross-module): `ExprTypecheck*.cpp`, `TypeResolution*.cpp`,
`LlvmBackend_String.cpp`, `ModuleLoader.cpp`. Counts below are from the exclusive assignment
(`MEASUREMENTS.md` §5); S/C/L = soundness / crash / leak members.

| # | campaign (track) | open (S/C/L) | why here | depends on |
|---|---|---|---|---|
| 0 | **Enablers** (standing, ≤1 lane) | 166 total; worked only as DONE-instrument needs | measurement must not lie before anything is declared fixed | owner: #2924/#7145 policy |
| 1 | **`using var` / `await using` over every head kind** (ownership) | 24 (7/3/5) | heap-buffer-overflow and SEGV measured today on a two-line program; bounded; the pilot that proves the mechanics | ruling #3493 for half the members — **RULED 2026-10-27 (option 2, the `DisposeAsync` dispatch-table slot); the entry check found §3's U1 SPLITS into U1a/U1c/U1d and U2 into U2a–U2e, so the pilot's scope is three sub-families, not three** |
| 2 | **Built-in call admission: typed arms for the stdlib surface** (sema) | 189 (33/53/0) | 53 compiler aborts and 33 wrong-value/soundness; the largest intake stream (159 filed in 30 d) is being fixed one arm at a time right now | none hard — **the entry check measured a LIVE SCOPE of 98 (92 reproduces + 6 moved) and re-labelled 77 of the 189 to other campaigns; the B0 leverage is at most 52/189 (27.5 %), or at most 45/98 (45.9 %) of the live members — BELOW ONE HALF, so the plan's own falsification rule forces B0 to SPLIT** |
| 3 | **Enumerable heads and iterator frames: one bridge per face** (ownership) | 125 (43/9/27) | highest soundness density (wild jumps, UAF); September's G family untouched (19/21); grid instrument already sketched | campaign 1 (shares `registerUsingDisposal`) |
| 4 | **Producer temporaries and boxes: one discharge authority** (ownership) | 148 (14/7/98) | the largest leak family and the loop's main engine (116 filed in 30 d); contains the UAFs #6393, #6983, #6409 | campaign 3 (shares the minting gate) |
| 5 | **Nullable carrier unwrap at every value consumer** (sema/codegen) | 54 (14/0/0) + the builtin-position subset handled in 2 | tagged carrier read as a value: garbage, malformed IR; one unwrap authority | campaign 2 |
| 6 | **Closures and capture cells, second pass** (ownership) | 111 (24/3/49) | A's anchor (#2718) landed; 26 old members plus ~40 new; #4522 names the four shapes the refcount "deliberately declines" | campaign 4 |
| 7 | **Generic instantiation boundaries** (sema/codegen) | 88 (17/13/3) | I2/I3 untouched (12/13, 8/9); the S3 codegen anchor #4567 is filed and unworked | campaign 5 |
| 8 | **Owned collections: element lifetime in the runtime** (ownership/runtime) | 77 (13/1/30) | string/class elements of built-in collections leak or over-release; `elem_disposer` stamp per mint site; filed faster than closed | ruling #6358; campaign 4 |
| 9 | **Borrow provenance and the sink rule** (ownership pass) | 96 (30/1/17) | the v0.320–v0.345 soundness trackers for §28.3/§28.14/§9.7; accept-side holes that become UAFs | soundness research Stage 0 (pinmap), a train |
| 10 | **Choice payload lifetime and layout** (ownership/codegen) | 30 (10/6/10) | stack-buffer-overflow at construction (#6167), owning-match UAF (#5453); small | campaign 4 |
| 11 | **Async return slot and frame-carried locals** (ownership/async) | 32 (16/1/7) | task result dangling (#6051, #6097, #6098); frame fields for captured cells | campaigns 1, 6 |
| 12 | **Cross-module parity: retire AST-gated symbol reads** (sema) | 25 (9/2/4) + the cross-module halves parked in 7/8 | #6364: the import-blind-cast family is unbounded while `check_imported_symbol_reads` is baselined at 31 | campaign 7 |
| 13 | **Records, statics, contracts, exceptions** (mixed) | 49 (9/0/1) | remainder; re-cluster at its entry check | — |
| — | spec-trains (222 unclaimed `spec-tracking`), features-proposals (99), parking-lot (78) | — | triaged, not worked, until a campaign claims them | — |

### Campaign 0 — Enablers (standing; never more than one pool slot; not a campaign in §6's sense)

Worked only when a DONE instrument needs them, in this order: **(a)** `internal:` degrade → the compiler
stops (#7145 measured a NULL-deref binary shipped after an `internal:` note; #2924's disposition record
is the anchor; the *pointer-degrade* sites flip to Error — **owner decision**, §4.8); **(b)** lit
`CHECK-NOT` ordering vacuity (#7065, #7069, #7070, #7071) — a plan-wide oracle hole; **(c)** the
uninstrumented hand-rolled emit runners (#3917) and the leak census blind spot (#7335); **(d)** the
checked-disposal default (#4602, **owner decision**); **(e)** promote `docs/proposals/papers/master-plan/work/*.py`
to `scripts/tracker/` so §6's numbers are reproducible from the repo (a coordinator decision). **(f)** kernel pins for
the ownership corpus (from the soundness research, `docs/research/soundness/pinmap/`): a `kernel:`
header naming the K/A clauses on each of the 435 §28/§30.5 rule-corpus cases, a verbatim `rule:` quote
for those cases (242 paraphrase today), and a ratchet that no live kernel clause has zero executable
pins (six have none: K1.2, K1.4, K3.6, K4, K5.4, K5.8; 44 of 80 are pinned only by matrix cells). It
needs no spec change. Required before campaign 9's entry, and wanted by any campaign whose DONE grid
cites kernel clauses. **(g)** the model-vs-compiler differential (soundness research Stage 0, the
`bzcore` model and its `.bzl` printer): its findings are matched against open issues and get a
campaign label at intake; they are worked only inside the owning campaign (§1.4).

### Campaign 1 — `using var` / `await using` over every head kind (ownership track; the pilot)

**Scope.** Spec §28.10, §28.12, §30.7 (v0.345), stdlib §2.10 (`IAsyncDisposable` extends
`IDisposable` since v0.345, #7354). Label `campaign:using-heads`. Shared code path: `registerUsingDisposal`
(its `asyncDispose` arm and the two spellings of the builtin-collection build, #3471), the sema vacuity
check (§28.9 warning), `classOfLocal`/`usingVarBuiltinHeadName` (#3482).

**Members (24).** Sub-family **U1 — RHS kinds with no class disposal obligation take the class path**:
#7322 (string RHS, heap-buffer-overflow — MEASURED SIGSEGV today), #7327 (function-typed RHS),
#7283 (lambda), #7306 (string), #7337 (`Object`-declared holding a runtime-managed value), #7338
(`Type` token missing from the runtime-managed set — sema and codegen disagree), #7339 (`Object` holding
a string), #3482 (generic type-parameter head reads word 0 as a vtable), #7326 (`await using` outside an
async body not rejected), #7316/#7305/#7357 (value-like choice copy path leaks), #7345 (v0.345: explicit
`Dispose()` reclaims at the call where the spec says logical disposal only — the holder frees).
Sub-family **U2 — `DisposeAsync` keyed on the declared type** (September's H): #3493 (anchor: the
dispatch point), #3483 (interface head runs the destructor), #3516 (abstract head), #3515 (derived
level's owned fields stranded), #7328 (interface/base-typed RHS takes the synchronous path), #5653
(v0.328: await the run-time object's `DisposeAsync`), #7354 (v0.345 conformance), #1531/#1603 (direct
`DisposeAsync()` physical reclamation incl. the fault edge). Sub-family **U3 — async enumerators**:
#2623 (owned field holding an async enumerator), #2624 (explicit `Dispose()` plus scope end leaks the
pending advance), #6194 (`using var t := tcs.Task` then `await t` hangs). Cleanup: #3471.

**Entry check.** Re-measure all 24 on HEAD (two moved today already: #7305 fixed the construction
path, #7322's fix left #7327). Build the DONE grid: **head kind** {class, derived-behind-base, interface,
abstract, generic `T` (class-constrained / unconstrained), string, function value, value-like choice,
owning choice, `Object` holding each of these, `Type`, built-in collection, async enumerator, `Task`} ×
**binding form** {`using var`, `await using var`, explicit `Dispose()`, explicit `DisposeAsync()`, copy
of a `using var` local, field store} — every cell is *disposed exactly once / vacuous with the §28.9
warning / rejected with the §28.10–§28.12 diagnostic*, run under ASan. About 90 cells; many already
exist as corpus cases (`s28_10_*`, `s28_12_*`) — the entry lane dedups against `cases/` first.

**Anchor.** One predicate, shared by sema and codegen, answering *"what disposal obligation does this
RHS carry, and of which kind"* (none / class sync / class async / value-like payload / move-only),
keyed on the **runtime representation** where the declared type is `Object`, `T` or an interface.
U1 needs no ruling: §28.9 (v0.345) already says these values have no `Dispose()`. U2 needs the #3493
ruling (a fixed vtable slot vs an `IAsyncDisposable` itable lookup at the disposal site); **ask for it
in the first session**, so it is ruled by the time U1 lands.

**Lane split (serialised by the one-disposal-lane rule).** Lane U1 (Opus): the predicate + the U1
members + the grid's sync half; files `LlvmBackend_Disposal.cpp`, the `registerUsingDisposal` site, the
sema vacuity check. Lane U2 (Opus, after the ruling): the dispatch point + U2 members + the async half of
the grid. Lane U3 (Sonnet, after U1): the three async-enumerator members and the #3471 de-duplication.
Each lane: `runAotChecked` rows, loop-amplified, with the **double-free control row** (`CLAUDE.md`:
never trade a leak for a UAF). Expected duration: two sessions. While U-lanes are serialised, the pool's
other slots run **Campaign 2** (the other track).

**DONE.** §2.3 with: the grid green under ASan; `s28_10_*`/`s28_12_*` fail rows re-banked (**the
coordinator measured 4 banked fail under §28.10 and 6 under §28.12, plus 3 UNBANKED failures under
§28.12 (`s28_12_0013`, `s28_12_0020`, `s28_12_0021`) — the plan's "2 and 5" was STALE, and the true
in-scope divergence count is 4 and 9**); U2 either closed or every member re-scoped *blocked on
#3493* if the ruling has not arrived — in which case the campaign exits `RE-SCOPED`, with U1 and U3 DONE
and U2 re-queued behind the ruling. **=> THE ENTRY CHECK (2026-10-28) SPLIT U1 AND U2, SO THE
"THREE LANES" SPLIT BELOW IS SUPERSEDED BY THE RECORD'S OWN SUB-FAMILIES: U1a/U1b/U1c/U1d,
U2a/U2b/U2c/U2d/U2e, U3a/U3b/U3c, U0. THE ANCHOR'S SITE IS FOUR PLACES, NOT ONE (a shared predicate
in `src/common/`, `Emitter::registerUsingDisposal`, `MethodOwnership::handleLocalDecl`, and
`Emitter::emitAsyncDisposeAwaitAndFree`).**

### Campaign 2 — Built-in call admission: typed arms for the stdlib surface (sema/stdlib track)

**Scope.** Spec §22.6 (applicability and ambiguity), §22.5 (named arguments), §5.1 (conversions), §7.3/
§7.4 (nullability at parameters), §17.4; stdlib §2 (primitives: `Parse`/`TryParse`/`ToString(format)`/
radix, `Math`, `Convert`), §3 (`StringBuilder`, `Encoding`), §9.6 (string members). Label
`campaign:builtin-call-admission`. Shared code path: the hand-written per-builtin arms in
`ExprTypecheck_Calls.cpp` (named in 110 open issues), `LlvmBackend_String.cpp`, the `isStrBld`/`Parse`
arms in `LlvmBackend_Setup.cpp`, `BuiltinRegistry.cpp`.

**Mechanism (QUOTED from the members).** A builtin member is recognised by **head name** and lowered by an
arm that trusts its argument shapes: a non-string first argument to `int.TryParse` aborts the compiler
(#7290), the decimal arms are "a second copy in a different file" (#7300), a user class named
`StringBuilder` is claimed by the builtin arm (#7217, #6845), nullable carriers reach `Math.Floor`
unwrapped (#7297 — MEASURED today), `Math.Max(3u, 4000000000u)` prints 3 (#6864), a reordered named
argument hands an `i1` to a `ptr` parameter (#7180). Today's landing log shows the loop in motion:
`narrow #7290`, `narrow #7293`, `narrow #7299`, `narrow #7300`, `fix #7313` — one arm per commit.

**Members (189; the full list is `work/out/campaign-members.md`).** Sub-families:
**B0 anchor** — a single admission gate: every builtin call resolves against a declared signature
(the stdlib index is the source: `Bezel_Standard_Library.md`, mirrored in `BuiltinRegistry`) *before*
lowering; arity, §5.1 conversions, §7.3 nullability, §22.5 named arguments and the receiver's **symbol**
(not its name) are checked there, once; an arm that is reached with an unadmitted shape is a compiler
bug by construction (an `internal:` that stops, campaign 0(a)).
**B1 Parse/TryParse/radix** (#7285, #7290, #7293, #7294, #7299, #7300, #7286, #7295, #7296, #7304,
#7315, #7320, #7323, #7325, #7341, #7268, #7271, #1792, #7258, #4890 spec-question).
**B2 ToString(format)/Convert rendering** (#7265, #7284, #7358, #7287, #7329, #7359, #7361, #7362,
#7319, #7331, #7333, #7336, #7303, #7311, #7239, #7291, #7234, #7242, #6782, #6825, #7267 leak twin in
campaign 4).
**B3 Math/Convert argument typing** (#7297, #7232, #7218, #7219, #7227, #7254, #6647, #6884, #6648,
#6864, #6865, #6942, #6295, #7061, #7249, #7252, #7253, #7274, #7292 double evaluation).
**B4 StringBuilder and string member arms** (#7244, #6809, #7154, #7180, #6635, #6636, #6826, #7217,
#6845, #6159, #7210, #7221, #7228, #7229, #7108, #7189, #7198, #7245, #7129, #7277, #7130, #6753,
#6755, #6777, #6780, #6820, #7028).
**B5 nullable carrier at a builtin position** (#6823, #6255, #6637, #2625, #2642, #7342, #6646, #6873,
#6116, #6126, #7004, #7089 — the general rule is campaign 5; B5 takes only the builtin-position cells).
**B6 null at a non-nullable builtin parameter** (#6810, #6811, #6812).
The remaining ~90 rule-matched members (sema-side overload ranking on builtin heads, `?.` spellings,
`Object`-held builtins) are confirmed or re-labelled at the entry check.

**Entry check.** Re-measure; build the DONE generator: **receiver kind** {each primitive, `string`,
`StringBuilder`, `Math`/`Convert` statics, nullable of each, `Object` holding each, a user type named like
a builtin, a type parameter} × **argument kind** {matching, §5.1-convertible, wrong type, nullable
(narrowed / not), null literal, class instance, unit} × **spelling** {positional, named, reordered named,
`?.`, method reference}. Verdict per cell from the stdlib signature: accept-and-run, or reject with the
§22.6 wording. The generator's existing precedent is `scripts/regression_matrix.py`. Baseline cells wrong
at entry are the campaign's number to drive to zero.

**Lane split.** B0 (Opus) first and alone in `ExprTypecheck_Calls.cpp`; then at most three instance
lanes with disjoint arms: B1+B2 (Sonnet; the radix/format arms), B3+B6 (Sonnet; `Math`/`Convert` and
null admission), B4 (Opus; `StringBuilder`/string, touches `LlvmBackend_String.cpp` and the runtime
`bz_sb_*` calls), then B5 (Opus). Every rejection added or removed sweeps embedded test programs
(lane-contract 19b) — this campaign flips many permissive-acceptance pins, each named in the commit body.

**DONE.** §2.3 with: generator green (0 compiler aborts, 0 `IR verification failed`, every wrong-shape
cell rejected by §22.6); members closed or re-scoped; the stdlib §2/§3/§9 corpus fail rows (**the
coordinator measured 31 + 2 + 8 = 41 today — the plan's "25 + 3 + 6 = 34" was STALE**) re-banked; the
`permissive-acceptance` warning count in `src/` for builtin heads banked lower. **=> THE ENTRY CHECK
(2026-10-28) FOUND THE B0 ANCHOR DOES NOT SURVIVE THE FALSIFICATION RULE AND SPLITS** (at most
52/189 = 27.5 %, at most 45/98 = 45.9 % of the live members — below one half), **AND THAT THE
DONE INSTRUMENT NEEDS ONE MORE THING BEFORE IT IS THE DONE INSTRUMENT: 39 of its 60 reject needles
PREDICT a diagnostic the compiler does not produce today, while the compiler has at least three
families for a missing member — so a CORRECT fix could leave the grid red. THE CLASSIFIER ALREADY
TAKES A LIST OF NEEDLES; NO CELL USES ONE.** **=> THE GRID IS THE CAMPAIGN'S BASELINE, NOT YET ITS
DONE INSTRUMENT.**

### Campaign 3 — Enumerable heads and iterator frames: one bridge per face (ownership track)

**Scope.** Spec §29.2 (iteration protocol and wrapper frames), §29.11 (single consumption), §28.6
(getters are borrow-producing positions), §21.4/§21.5 (`for`, `break`), §26.5 (extensions on
`IEnumerable<T>`); stdlib §5 (query operators), §4 heads. Label `campaign:enumerable-heads`. Shared
code path (QUOTED from G and the new members): `wrapAsIEnumerableForTarget` and its gate,
`lowerIEnumerableFor`, `wrapArrayAsIOwningEnumerator`, `registerIterableSourceDisposal`, the §5
terminal arms' minting gate and `freeFreshInputTemp`, the getter-frame faces
(`propBlockGetterReturnKinds_`, `kPropGetterReturnMint`), spread over `LlvmBackend_Assign/_Calls2/
_Calls3/_Iterators/_Setup/_Stmts.cpp`.

**Mechanism.** The runtime representation behind an `IEnumerable<T>`/`IOwningEnumerable<T>` head is
chosen per **face** (local, return, getter, argument, field, `for`, interface dispatch, user class
implementing the interface), and each face has its own bridge or none; a missing bridge drives the
§29.11 protocol on a vtable whose slot is out of bounds (the recurring `pc = 1` wild jump), or frees a
field the holder owns (#6387), or hands back a bare slot pointer with no +1 (#6393).

**Members (125).** **E0 anchor**: one bridge keyed on the *runtime* representation, consulted at every
face (September G's prescription, never executed). **E1 getter frames and laundering (§28.6)**: #3469
(now fenced by the getter diagnostic — the fence is #6451's complaint), #6451, #6487, #6489, #6490,
#6491, #6493, #6539, #6403, #6407, #6438, #6447, #6387, #3370, #3363, #3390. **E2 argument and
parameter faces**: #6453, #6455, #6406, #6409, #6412, #6415, #3475, #7039, #2297, #4007, #4009, #4010,
#6703. **E3 §5 terminals, the minting gate and materializers**: #6393, #6396, #7160, #7179, #7183,
#7184, #7212, #7213, #6563, #6567, #6613, #6556, #6559, #6626, #6712, #6759, #4706, #7178 (an exception
inside a selector is swallowed), #3576, #6138, #6242, #6246. **E4 `for` lowering**: #6185 (loses its
index across a suspension), #6186, #6245, #6277, #6280, #6545, #6964, #6473, #4886, #4887, #4888,
#6819, #6890, #6863. **E5 owning heads and §29.11 single consumption**: #2307, #3478, #3476, #3479,
#4827, #4828, #3561, #3406, #2520, #3519, #1839, #3544, #6356, #6360, #3522, #7063. Spec-question
candidates already visible: #4740 (does a span mutation invalidate an enumerator), #2331.

**Entry check.** Re-measure (G's 19 are 60–105 days old; expect several to have moved like #3469).
The DONE instrument is G's **face × representation grid**: faces {local, return, getter (block / arrow
/ virtual / interface-dispatched), argument (owned formal / borrowed formal / interface formal), field
store, `for` (with `break`, with `await`/`yield` in the body), `_ :=` discard, interpolation hole,
extension receiver} × representations {Array family, Set/Dictionary snapshot, sorted snapshot, lazy
pipeline (eager and lazy terminals), sync iterator frame, async iterator frame, user class implementing
`IEnumerable<T>`, string as `IEnumerable<char>`, `IOwningEnumerable<T>` heads, imported each}. The
existing `scripts/check_enumerable_grid_spellings.py` + `enumerable_grid_spellings.baseline` is the
spelling half; the entry lane extends it into run-and-check cells. Every cell: correct, or **rejected**,
never a wild jump.

**Lane split.** E0 (Opus) alone first. Then E1 (Opus; `_Setup.cpp` getter faces), E3 (Opus;
`_Calls3.cpp` terminals), E4 (Sonnet; `_Stmts.cpp`/`_Iterators.cpp`), E2 and E5 (Sonnet, after E0's
bridge covers their faces) — pairwise file-disjoint by declaration, otherwise serialised (§1.3).

**DONE.** §2.3 with: the grid green under ASan; the §29 corpus fail rows (7 today) and the stdlib §5
row (1) re-banked; `enumerable_grid_spellings.baseline` lower; the `itmap$` wild-jump signature absent from
every cell's ASan log.

### Campaigns 4–13, summarised

**4 — Producer temporaries and boxes (ownership).** Members: #7152 (a temporary at an interface- or
`Object`-typed formal is never discharged — "a fourth arm of the K8 rule"), #7171, #7187, #7188, #7211,
#7230, #7231, #7267, #7272, #7222, #6775, #6632, #6889, #6896, #7190, #6736, #6744, #6857, #7278,
#6938, #6421 (`_ :=` discard), #6444, #6633, #5350, #6003, #6117; boxes (F): #7005, #7298, #6769,
#6827, #6929, #6926, #6733, #6209, #6093, #6094, #6085, #6930, #5951, #6149, #4841, #4782, #6832,
#3447, #3480, #2760, #3908, #3905. Anchor: one answer to *"is this expression a +1 producer, and who
discharges it"* (`isFreshOwnedClassProducer`, the §5 minting NAME list, `freeFreshInputTemp`) consulted
by every arm, with **statement-end discharge of every unbound producer** as the default and escape as the
exception. DONE instrument: producer kind {string op, `ToString`, `new` builtin, collection literal,
query terminal, box mint, user `owned` return} × position {argument (plain / owned / interface /
`Object`), receiver, `while`/`if` condition, interpolation hole, index, discard, `return`, field store,
`base(...)`}; under ASan, 0 leaks and 0 double frees. Risk: the highest — the bias "leak over UAF" is
correct and every row needs its double-free control.

**5 — Nullable carrier unwrap (sema/codegen).** #7004, #7089, #7066, #6659, #6677, #6084, #6116,
#6126, #6254, #6750, #2620, #6072/#6073 (four conversion positions never checked), #6225
(spec-question: is `T? → Object` a conversion), #6859, #6482, #5903 (v0.335 *a narrowing converts
nothing*), #5522, #6731, #6723, #6724, #7350, #7353, #6772. Anchor: one unwrap-before-consume authority
in the lowering of a §7.4/§12.7-narrowed read, and one §5.1 conversion check applied at every position
(#6072 lists `??`, `for var t: T in`, target-typed lambda body, `yield return`).

**6 — Closures and capture cells, second pass (ownership).** September A's 26 open plus: #7288, #7112,
#7183, #6953, #7031, #6766, #6839, #6628, #6480, #6212, #6213, #6226, #6230, #6269, #6306, #5927,
#6108, #6127/#6137, #5850, #5851, #5866, #5228, #4938, #4860, #4534, #4535, #4542, #4522 (the four
declined shapes), #4474; sink-rule members #6634, #6651, #6652, #6653, #6710, #7259, #7038, #6141.
Entry check decides whether #2718's refcount is complete (then the members are missing arms) or the
"deliberately declined" shapes need a second mechanism (then a design page). Must land as **one series on
one tree** (#4438's interaction record).

**7 — Generic instantiation boundaries (sema/codegen).** I2: #2289, #2291, #3348, #3378, #3524, #2650,
#2220, #2861, #2020, #710, #3655, #3659, #3526; I3: #3501, #3525, #3486, #3474, #3495, #3485, #3514,
#2580; new: #7021, #7056, #6792, #6931, #6932, #6927, #6257, #6254, #6232, #6112, #6021, #4861,
#4862, #7062, #7066, #6866, #6747, #6315, #4980, #5417, #6392, #6443, #6407; anchors #4567 (codegen
substitution authority), #4568 (`substitutedChoicePayloadType`); #2735/#2736 (I1's unfinished split).
Rule: *no template-emitted code is ever entered with a substituted-type object*. DONE instrument: the
generics research matrix's substitution kinds × constructs, run as programs.

**8 — Owned collections: element lifetime in the runtime (ownership/runtime).** #6936, #6937, #7058,
#7059, #7060, #6945, #6992, #6993, #6994, #6691, #6238, #6282, #6145, #6943 (imported), #2648/#2645
(the stamp), #6516, #6514, #7216, #6672, #6685, #6760, #6995, #7052, #7053, #7043, #2327, #3393, #4332,
#2762, #2676, #2672/#2685, #6184, #6448 (spec-question), #6467, #6365, #6488, #6186; the §28.2
disposed-guard trio #7076, #7081, #7082 and #6314. Anchor: one `elem_disposer` stamp keyed on the element
type at every mint site (literal, `new`, imported literal, `From`, snapshot), and one release path per
runtime container in `bezel_runtime_collections.cpp`. Needs ruling #6358 (does `bz_dict_free` release
string keys — a co-ownership contract question).

**9 — Borrow provenance and the sink rule (ownership pass).** The `spec-tracking` soundness trackers of
v0.320–v0.345 for §28.3 (provenance clauses, consume-site freeze, set-erasing positions,
bare-type-parameter returns), §28.14, §9.7, §30.5: #5516, #5517, #5518, #5539, #5552, #5553, #5561,
#5562, #5565, #5623, #5649, #5650, #5697, #5698, #5699, #5700, #5702, #5704, #5718, #5719, #5720,
#5721, #5722, #5803, #5819, #5820, #5829, #5830, #5831, #5903, #5904, #5933; the measured holes #7107,
#7097, #7240, #7092, #6886, #5973, #5974, #5845, #6136, #6616, #6708, #6737, #7087, #7075, #6848; B's
`checkRead` members #3967, #4102, #4122, #4124, #3979, #3959, #3999, #4055, #4450; C's #4428, #3827,
#3520, #3569; M's predicates #4301, #4277, #4274, #4291, #4360, #2615, #2636, #2705, #2690, #1933,
#2577, #2569, #2551, #2570, #3632, #2671, #2740. **Depends on the soundness research's Stage 0**
(`docs/research/soundness/README.md` on `origin/claude/sharp-wozniak-2ocblf`): the pin map that lists,
per K/A clause, the matrix cells and corpus cases to re-judge is exactly this campaign's entry
instrument, and its item 1 (a `kernel:` header on the 435 §28/§30.5 cases) is what makes "every
rule-corpus case in scope" computable. Recommend the owner schedule Stage 0 as the next train's tooling
item, before this campaign reaches ENTRY. The three research matrices are re-run (targeted) at this
campaign's exit.

**10 — Choice payload lifetime and layout.** #7316, #7357, #7305, #7047, #6738, #6170, #6233, #5912,
#5992, #6014, #6168, #6167, #5453, #5153, #4804, #4684, #4634, #6164, #6165, #6084, #6232, #6478,
#3905, #2220, #6034, #6043, #6638, #6640, #4816, #4883, #3939, #2571. Anchor: one payload-slot
layout/lifetime authority (construction, copy, match binding, box) — the J family's classifier is done;
this is its codegen twin.

**11 — Async return slot and frame-carried locals.** #6097, #6098, #6099, #6051, #5949, #5311, #5280,
#5276, #5410, #5982, #5981, #6194, #6211, #6245, #4883, #5015, #6173, #6446, #5831, #5699, #5704,
#5719, #5722, #5657, #5900, #3403, #3466, #3928, #1422. Anchor: the §30.5 return-slot ownership and a
frame field for every captured cell holder.

**12 — Cross-module parity.** #6364 (anchor: ratchet `check_imported_symbol_reads` to zero), #7223,
#7062, #7078, #6943, #6944, #6991, #6994, #7008, #6743, #6833, #6918, #6927, #6932, #6360, #6206,
#6257, #6289, #6043, #5214, #3671, #3655, #3378, #3357, #3350, #2861, #2648, #2650, #2735, #2291,
#1425, #2590, #7281 (spec). Every campaign 3–11 carries its imported twin as a grid column; this
campaign is what is left after they run.

**13 — Records, statics, contracts, exceptions** (#6204, #6206, #6497, #6498, #4645 §22.16 invariants;
#6856–#6858; #7057; #2295, #2114 generic statics; #3966; #1882/#1878 throwing destructors; #2215
§28.8.1). Re-cluster at entry; expect it to split into two small campaigns.

---

## 4. Rules that keep a campaign honest

1. **One root cause per lane.** A lane brief names one shared code path and the members that sit on it.
   A lane that finds a second site stops, records the split in the campaign record, and files or
   re-labels the members — it does not fix both. (September's I1 lane did this correctly and is the
   model: *"one mechanism across four layers, each layer needs its own anchor"*.)
2. **Fix the family, not the instance.** A `fix #N` commit in a campaign must say which other members
   it closes or why it closes only one (`narrow #N` with the family named). A narrow fix is allowed only
   when the anchor is not yet landed *and* the member is a measured memory-safety defect on a path users
   reach; otherwise the instance waits for the anchor.
3. **Measure the siblings before closing.** Before `fix #N` is picked, the producer's handback lists the
   neighbouring shapes it ran (lane-contract 19a's matrix) *in the campaign's DONE grid's terms* — the
   same cells, so the grid grows with every fix instead of a scratch matrix nobody keeps. A sibling the
   matrix exposes is filed with the campaign label in the same handback, never left for a reviewer.
4. **The "narrow fix produces a sibling" loop is broken at intake, not at fixing.** When a review or a
   lane finds a new shape: if it is *inside the active campaign's scope*, it is a member — labelled,
   added to the record, and the campaign is not DONE until it is closed or re-scoped; if it is *outside*,
   it gets its campaign label and is not worked. Filing stays mandatory (lane-contract 18); working it
   does not follow from filing it. The weekly signal that the loop is broken: §6's *in-scope siblings
   filed* column falls while *closed* rises; the plan is failing if both rise together for two sessions,
   and the exit review must say why (anchor incomplete, or family drawn too wide).
5. **Stop and file a spec question when** the spec does not decide a cell of the DONE grid, two members
   cite the same section for opposite verdicts, a fix would flip a banked `pass` corpus row, or a
   closed-form rule (a table, a closed list) is missing the case. File with `spec-question`/
   `spec-defect` + the campaign label; hold the hunk out; keep working the cells the spec does decide.
   Never resolve it in code (`CLAUDE.md` rule 1); never let it stall the campaign — the member is
   re-scoped *blocked on #R* and the DONE line still holds.
6. **Never weaken, never relabel to hide.** A `parking-lot` or `campaign:` label never changes a
   severity label; a member re-scoped *not a defect* quotes the spec sentence. The test rules of
   `CLAUDE.md` apply unchanged.
7. **Lane concurrency inside a campaign (asks the owner).** The standing rule allows one
   disposal/ownership/async/iterator lane at a time. Inside an ownership-track campaign, after the anchor
   has landed and been reviewed, this becomes *file-disjoint by declared hot-file sets*, with the
   coordinator checking the sets against `claims.tsv` before dispatch. **Owner-approved 2026-10-27**
   and written into `CLAUDE.md` *Pools*. Before the anchor lands, campaigns serialise.
8. **Owner decisions are batched at campaign boundaries.** The first batch was asked and **ruled on
   2026-10-27** (owner-approved; each ruling is posted on its issue):
   - **#3493**: dispatch `await using` on the run-time object through a `DisposeAsync` dispatch-table
     slot (option 2), covering #5653 and #3483 too; reclamation stays with #3515, then #3516.
   - **#2924 / #7145**: every `internal:` degrade site becomes `Severity::Error`, with a ratchet holding
     the warning-site count at 0; a newly failing program is a lowering gap filed to its campaign.
   - **#4602**: bound checked-mode retention (keep the header word with the disposed flag, free the
     payload; header-only for arrays, dictionaries and closure cells), then default to checked, add
     `--unchecked-disposal`, keep `--checked-disposal` as a no-op. **Amended:** at an explicit
     `Dispose()` where the K5 liveness proves no borrow of the value survives, reclaim in full at the
     call (no retention); retain only where a borrow may survive or the analysis is unsure (spans,
     `Shared<T>`, collection elements, unknown provenance). Also retain where a later release of the same binding
     (scope end, overwrite, a repeated `Dispose()`) is reached on a *maybe-consumed* join and would
     read the flag; where the explicitly-disposed post-state is static there, emit nothing. Check elision is a later campaign, after
     the soundness model checks its premise.
   - **#6358**: the dictionary records at construction that its keys are strings, and every free path
     (`bz_dict_free`, `Remove`, `Clear`, overwrite) releases them; no per-site emission.
   - **#1877 / #3379**: the 2026-07-24 refcounting ruling stands; split the umbrella into per-kind
     sub-issues at campaign 4's entry, each fixed as refcount adoption for that kind.
   - **Soundness research Stage 0** (the pinmap): a campaign-0 enabler, required before campaign 9's
     entry check, not before.
   - **`CLAUDE.md`**: the WIP/intake rule (§1.2, §1.4) and rule 7 above are now standing rules there.
   Later campaigns add theirs at their entry check.
9. **Progress is reported in one table (§6), updated at the end of every session**, in the same
   commit that updates the campaign record. Each row's numbers are produced by the label query and the
   corpus runner, not typed from memory; the session's handback to the owner is that table plus three
   lines: what landed, what is blocked, what is asked.

---

## 5. What happens to the old plan

*Done 2026-10-27, in the commit that installed this plan; the list is kept as the record of what moved.*

- **Archive the July DAG as a dated paper**: `git mv docs/proposals/papers/master-plan.md
  docs/proposals/papers/master-plan-2026-07.md`, add a one-line banner at its top ("Archived 2026-10-27;
  the proposal DAG and status ledger are historical; the live plan is `master-plan.md`"), and install this
  document as `docs/proposals/papers/master-plan.md`. The DAG's content is not lost: the proposal
  status ground truth is `docs/proposals/README.md` (spec-change process §8), and the DAG's build layers
  stay relevant for the feature programme, which this plan leaves to `implementation-roadmap-2026-07.md`
  and the `features-proposals` label (99 trackers, not worked by campaigns).
- **Cross-references to update** (every file that links `master-plan.md`, from `git grep`):
  - `docs/proposals/README.md:263` — the *Working papers* row: retitle to the campaign plan, add a row
    for `master-plan-2026-07.md` ("the 2026-07 proposal DAG, archived").
  - `docs/proposals/papers/implementation-roadmap-2026-07.md:81` and `:134` — cite
    `master-plan-2026-07.md §1·Δ·G(b)` (the parser/lexer lane and the RFC subtraction note refer to the
    old ledger).
  - `docs/proposals/papers/defect-campaign-2026-09.md:8` and `:791` — "sequenced by `master-plan.md`"
    becomes `master-plan-2026-07.md`; add a banner at its top: "Superseded 2026-10-27 by
    `master-plan.md`'s campaigns; family outcomes measured in `MEASUREMENTS.md` §6".
  - `docs/proposals/0011-process.md:381`, `:388` — "master-plan §4b" → `master-plan-2026-07.md §4b`.
  - `docs/proposals/0115-collection-literal-borrowing-span-family.md:187` — the charter C9 citation
    `master-plan.md:141` → `master-plan-2026-07.md:141`.
  - `docs/proposals/0103-shared-value-generics-study.md:190`,
    `papers/proposals-currency-2026-08-11.md:3`, `papers/stdlib-readiness-2026-08-11.md:106` — same
    rename.
  These are docs-only edits to non-normative papers (no version bump, spec-change process §5 item 5's
  guide-pass rule by analogy); they belong in the same commit as the archive move.
- **`defect-campaign-2026-09.md` stays** as the record of the family method and of what its anchors
  closed; its §2–§3 family tables are superseded by the campaign records this plan creates.
- **`MEASUREMENTS.md` and `work/`** land beside the plan (`docs/proposals/papers/master-plan/`), so the
  numbers in §0 and §3 can be re-run; campaign 0(e) moves the scripts under `scripts/tracker/` if the
  coordinator wants them gate-adjacent.

---

## 6. Progress table

*Updated at the end of every session, in the commit that updates the campaign record. Numbers come
from the label query and the corpus runner. Status ∈ QUEUED · ENTRY · ACTIVE · EXIT · DONE · RE-SCOPED.*

| # | campaign | track | status | entry date | members at entry (S/C/L/other) | closed | re-scoped | in-scope siblings filed / triaged | corpus fails in scope (entry → now) | grid cells wrong (entry → now) | blocked on | last session | next action |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | enablers | — | standing | — | — | — | — | — | — | — | — | — | #2924/#7145 errors; #4602 bounded retention, then default flip (ruled 2026-10-27) |
| 1 | using-heads | ownership | ENTRY | 2026-10-28 | 24 (7/3/5/9) | 0 | 0 | 0 / 0 | §28.10: 4, §28.12: 6 (+3 unbanked) → — | 41 wrong + 5 known-red (153 cells) | — | R36 | ENTRY LANDED (record + grid on `r36-c1-entry` @ `2381f534`, under review); U1 SPLITS into U1a/U1c/U1d — dispatch the U1c anchor lane |
| 2 | builtin-call-admission | sema | ENTRY | 2026-10-28 | 189 (33/53/0/103) | 0 | 0 | 0 / 0 | stdlib §2: 31, §3: 2, §9: 8 → — | 48 wrong (110 cells) | — | R36 | ENTRY LANDED (record + grid on `r36-c2-entry` @ `8a356f9c` + `87c1e94f`, under review); the grid's 39 PREDICTED reject needles block the DONE line (N1) |
| 3 | enumerable-heads | ownership | QUEUED | — | 125 (43/9/27/46) | 0 | 0 | 0 / 0 | §29: 7, stdlib §5: 1 → — | — | campaign 1 | — | — |
| 4 | producer-temporaries | ownership | QUEUED | — | 148 (14/7/98/29) | 0 | 0 | 0 / 0 | §28.3/§28.9 subset → — | — | campaign 3 | — | — |
| 5 | nullable-carrier | sema | QUEUED | — | 54 (14/0/0/40) | 0 | 0 | 0 / 0 | §7: tbd | — | campaign 2 | — | — |
| 6 | closures-cells | ownership | QUEUED | — | 111 (24/3/49/35) | 0 | 0 | 0 / 0 | §27.5/§28.14 subset | — | campaign 4 | — | — |
| 7 | generic-boundaries | sema | QUEUED | — | 88 (17/13/3/55) | 0 | 0 | 0 / 0 | §14: tbd | generics matrix: 1 ambiguous | campaign 5 | — | — |
| 8 | collection-elements | ownership/runtime | QUEUED | — | 77 (13/1/30/33) | 0 | 0 | 0 / 0 | stdlib §4: 22, §6: 23 | — | campaign 4 | — | — |
| 9 | borrow-provenance | ownership | QUEUED | — | 96 (30/1/17/48) | 0 | 0 | 0 / 0 | §28.3: 9, §28.6: 2, §28.14: 4, §9.7: 6 | matrices: 0 open at v0.336 | soundness Stage 0; a train | — | — |
| 10 | choice-payloads | ownership | QUEUED | — | 30 (10/6/10/4) | 0 | 0 | 0 / 0 | §35: tbd | — | campaign 4 | — | — |
| 11 | async-frames | ownership | QUEUED | — | 32 (16/1/7/8) | 0 | 0 | 0 / 0 | §30: 7 | — | campaigns 1, 6 | — | — |
| 12 | cross-module | sema | QUEUED | — | 25 (9/2/4/10) | 0 | 0 | 0 / 0 | §2.5/§2.8: tbd | `imported_symbol_reads` baseline 31 → 0 | campaign 7 | — | — |
| 13 | records-statics-misc | mixed | QUEUED | — | 49 (9/0/1/39) | 0 | 0 | 0 / 0 | §8, §22: tbd | — | — | — | re-cluster at entry |

**Session handback format** (three lines under the table): *Landed:* `fix #…` × n, members closed n,
grid cells cleared n. *Blocked:* rulings awaited, with dates asked. *Asked:* new spec questions filed
(numbers), labels added (count).

**R36 session handback (2026-10-28, in progress — the run ends 2026-10-30 02:00 NZDT).**
*Landed:* `narrow #7316` and `narrow #7333` picked onto staging (2 fix issues; UNATTRIBUTED 0);
#1792 and #7108 closed as fixed (each with a coordinator measurement; #7108's mechanism was already
closed under #7168). *Blocked:* the two campaign grids are not yet their campaigns' DONE instruments
— campaign 2's 39 PREDICTED reject needles (a correct fix could leave the grid red), campaign 1's
`Task` cells hang on one build and not on another; both entry commits are under review. *Asked:* 8
spec questions filed from the certain-faults research (#7378–#7384) + 11 campaign-1 defects
(#7387–#7397) + 2 codegen discriminators (#7385, #7386) + the 23 campaign-2 measurement comments;
labels applied by the Appendix B sweep. **=> THE GATE CADENCE (owner directive 2026-10-28): ONE GATE
CARRIES 60–100 ISSUE CLOSURES, NOT 5. => THE HAUL IS ACCUMULATED; DO NOT GATE A SMALL ONE.**

---

## Appendix A — the mechanical commands

```bash
# live member list of the active campaign (paginate explicitly)
gh api 'repos/s8lap/bezel-lang/issues?state=open&labels=campaign:<slug>&per_page=100&page=1'
# corpus fail rows in scope (sections as the campaign lists them)
python3 tests/conformance/rules/runner.py build/bezelc --filter 28.12
grep -E '^s28_(10|12)_' tests/conformance/rules/baseline.tsv | grep -c 'fail'
# the campaign's grid under ASan (the entry lane names the filter)
BEZEL_AOT_FORCE_SANITIZE=address scripts/agent_check.sh --area codegen <grid-suite>
# re-measure the tracker (the plan's numbers)
docs/proposals/papers/master-plan/work/fetch_issues.sh all && python3 -I docs/proposals/papers/master-plan/work/measure.py
python3 -I docs/proposals/papers/master-plan/work/campaigns.py
# triage a new issue
gh issue edit <N> --add-label 'campaign:<slug>'      # or parking-lot
```

## Appendix B — the initial labelling

`docs/proposals/papers/master-plan/work/out/campaign-members.tsv` assigns each of the 1613 open issues to exactly one
slug (seeded members from the issue bodies, the rest by title rule; 78 `unassigned` → `parking-lot`,
222 unclaimed `spec-tracking` → `spec-trains`, 99 → `features-proposals`). The first coordinator
session applies it with `gh issue edit --add-label` in batches of 100 and records the count in §6. The
assignment is a starting point; every campaign's entry check corrects it for its own members.
