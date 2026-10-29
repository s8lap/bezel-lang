# Campaign `using-heads` — `using var` / `await using` over every head kind

Track ownership · Sections spec §28.10, §28.12, §30.7, stdlib §2.10 (v0.345) ·
Shared code paths `Emitter::registerUsingDisposal` and `Emitter::usingVarBuiltinHeadName`
(src/codegen/LlvmBackend_Setup.cpp @ `d9f11d15`), `MethodOwnership::handleLocalDecl` and
`MethodOwnership::rhsIsDisposableClassType` (src/ownership/Ownership.cpp @ `d9f11d15`),
`common::isDisposableBuiltinHeadName` (src/common/BuiltinRegistry.cpp @ `d9f11d15`) ·
Anchor: **the design on "The anchor" below** — a shared predicate answering *"what
disposal obligation does this right-hand side carry, and of which kind"*, with a
static half and a run-time half.

*Entry lane: `r36-c1-entry`, branch `r36-c1-entry`, spec v0.345. Every number
below was produced by a command run on the tree's HEAD `d9f11d15`; the command
and its output are quoted. (`origin/main` has since moved to `9be24022`; no
commit in between touches a `src/` file, so the compiler under test is
unchanged — see the "Lane records" block.)*

### The anchor — one page

**The question.** *What disposal obligation does this right-hand side carry, and of
which kind?* One predicate, shared by sema and codegen.

**The five kinds.**

| kind | when | what scope exit does |
|---|---|---|
| **NONE** | the RHS is a runtime-managed value (§28.9: `string`, `Type`, boxed value types, function values) or a value type with no owned payload | nothing; §28.10: "the disposal at scope end is a no-op … and the only effect is the reassignability restriction" |
| **CLASS-SYNC** | the RHS's runtime class carries a destructor chain | run the destructor chain (§28.4), then reclaim |
| **CLASS-ASYNC** | the RHS's runtime class implements `IAsyncDisposable` | `await using` awaits its `DisposeAsync`; §28.12 step 2 then runs the cascade and the chain; the destructor body at a level that declares or overrides a non-`abstract` `DisposeAsync` is **skipped** |
| **VALUE-LIKE PAYLOAD** | a value type whose active case holds a runtime-managed payload with a +1 reference (§35.3 / §35.4) | **release the reference** — there is no `Dispose()` to call, but the runtime lifetime must be the plain-`var` twin's (§28.10) |
| **MOVE-ONLY** | an owning inline array or an owning choice | dispose the owned slots in place (§15.3, §35.4) |

**The key: the slot's REPRESENTATION, not its declared type.** Where the declared
type is `Object`, `T` or an interface, the declared type says only what the holder
*promises*; the representation is what the disposal would read. §28.12: "`await
using` selects the logical-disposal path by the class of the object the binding
**holds** … **never by the binding's static type**." Where the static type cannot
answer at all (an `Object`-typed slot, a `T`-typed slot, a monomorphized generic
head), the answer must come from the run-time value — a dispatch, not a static
classification.

**Is the anchor a MISSING ARM of an existing authority, or a NEW authority?**
**Both, and the split matters.** The compiler already has three near-answering
authorities, each answering a *different* question, each with a different
population — this is the §3419 "duplicated classification predicates" shape:

1. `Emitter::registerUsingDisposal`'s local lambdas `typeMayHoldClass` /
   `awaitUsingSlotMayHoldClass` (src/codegen/LlvmBackend_Setup.cpp) — the CODEGEN
   side. It answers one bit ("may this slot hold a class?"), it is a **local
   lambda inside one function** (so nothing else can call it), and it is keyed on
   the DECLARED type first, with a later narrowing on the right-hand side's kind.
2. `MethodOwnership::rhsIsDisposableClassType` (src/ownership/Ownership.cpp) +
   `common::isDisposableBuiltinHeadName` (src/common/BuiltinRegistry.cpp) — the
   SEMA/OWNERSHIP side. It answers a BOOLEAN ("does this type carry a disposal
   obligation?"), it is keyed on the **declared** type only, and it has no
   runtime-managed arm at all (which is why the §28.9 vacuity lint and the codegen
   predicate disagree — #7338).
3. `Emitter::usingVarBuiltinHeadName` (src/codegen/LlvmBackend_Setup.cpp) — the
   head-NAME resolver. It answers a NAME, by name only, and it cannot name a
   **monomorphized generic head** — which is exactly why #3482's head reaches the
   word-0 read.

=> The anchor is therefore **a missing arm plus a new half**:

* **A. The static half** — one shared predicate in `src/common/` (next to
  `common::isDisposableBuiltinHeadName`), returning the five kinds above, keyed on
  the slot's representation (the right-hand side's own type where the declared type
  is `Object`, `T` or an interface), monomorphization-aware where the head is a
  type parameter. It replaces `awaitUsingSlotMayHoldClass`'s three-valued bit and
  `rhsIsDisposableClassType`'s boolean, and sema and codegen both call it. This
  is a MISSING ARM of an existing authority, generalised from two ad-hoc forms to
  one, and it changes **no rule** (§28.9 and §28.10 already decide every static
  cell) — so **no owner ruling is needed for it**.
* **B. The run-time half** — where the static type cannot answer (an
  interface-typed or `Object`-typed slot, an abstract-class head, a base-typed
  head whose base does not declare `DisposeAsync`), the obligation must be decided
  by the slot's **run-time** class. §28.12 names the mechanism for the async half
  (the #3493 ruling: a trailing `DisposeAsync` dispatch-table slot); the
  reclamation half is #3515's open choice; the *value* dispatch (an `Object` slot
  that holds a STRING at run time — #7339) has **no** ruling and **no** existing
  authority, and is the one part of the anchor that would change a rule ⇒ **the
  coordinator sends that half to the owner**.

**What the anchor closes and what it does not.** Counted over the 24 members:

* **Closes 8:** #3482 (a monomorphization-aware representation key), #7338 (the
  `Type` token in the runtime-managed set), #7339, #3483, #7328, #5653, #3516,
  #3515 — the last six through the run-time half.
  **⚠ THIS IS THE ENTRY LANE's **PREDICTION**, NOT THE ANCHOR's MEASURED OUTCOME, AND IT IS
  SUPERSEDED BY THE ANCHOR LANE's BLOCK BELOW. MEASURED 2026-10-28 BY THE EXIT REVIEW: **#7339,
  #3483 AND #3515 ARE ALL OPEN** — #3483's family is an owned `IAsyncDisposable` RETURN (it needs
  the #3493 ruling's itab route) and #3515's is the PER-LEVEL RECLAMATION CHAIN (a file the anchor
  lane does not own). THE ANCHOR's OWN `fix` SUBJECT NAMES **FIVE**: #3482 #7338 #7328 #5653
  #3516, WITH `narrow #3483 #3515`. **THE EIGHT HERE MUST NOT BE READ AS THE CLOSURE SET.**
* **The shapes of six are already fixed before the anchor (the anchor
  generalises, it does not newly close): 6** — #7322 and #7306 (both the
  vacuous-`await using`-over-a-runtime-managed-value shape, fixed by
  `9bb9387f narrow #7322: the vacuous `await using` shape is not an async
  disposal`), #7283, #7327 and #7337 (fixed by `87fdfa67 fix #7337, #7327:
  the `await using` slot predicate must decide by the right-hand side's
  class` — **#7283 is the function-typed (`lambda()`) shape and #7327 is its
  re-file**, so `87fdfa67` carries #7283, #7327 and #7337), and #3493's
  dispatch half.
  ⚠ **All three of #7322, #7306 and #7283 are CLOSED — the tracker owns
  them.** MEASURED (`gh issue view <N> --json state,closedAt`): **#7306**
  closed 2026-10-27T14:43:15Z, **#7283** closed 2026-10-27T14:43:18Z and
  **#7322** closed 2026-10-27T14:43:21Z — all three closed **before** this
  commit's own timestamp (2026-10-27T14:51:59Z). **Their fixing commits are
  named by the issues' own closing comments**: **#7283 → `87fdfa67`** (the
  closing comment quotes that commit's own ASan measurement: a
  function-typed right-hand side fell through the predicate's switch, so the
  closure's word 0 was read as a vtable), **#7306 → `9bb9387f`**, and **#7322
  → `9bb9387f`** for its own part plus **`87fdfa67`** for the two shapes it
  was split into (#7327 and #7337). ⚠ **And #3493's dispatch half is
  re-derived by MEASUREMENT on this tree, not by a citation**: the earlier
  text here cited `e9c564fd fix #3494 / narrow #3493`, and that commit is
  **not an ancestor of `origin/main` nor of this tree** (it lives on a
  diverged local `main`). MEASURED here
  (`await using var u: Base := new Der();` over a `virtual` base
  `DisposeAsync`): the program prints **`DER_DA`** — the derived override IS
  awaited — so the shape is fixed in this tree and the fix's commit cannot be
  named from this tree's history. #3493 itself is OPEN, and its residual
  (#3515, the reclamation half) is not.
* **Does NOT close 10:** #1531 and #1603 (the direct `DisposeAsync()` path is
  `Emitter::lowerAwait`'s special case in src/codegen/LlvmBackend_Async.cpp — a
  different site), #2623 (the owned-field cascade in
  src/codegen/LlvmBackend_Disposal.cpp), #2624 (the async iterator's dispose tail),
  #6194 (the task-local disposal), #7316 and #7357
  (`Emitter::valueLikeChoiceLocalIsCopy` — the anchor names the VALUE-LIKE PAYLOAD
  obligation, but the copy path's ARMING is a different function), #7326 (a sema
  admission rule), #7354 (a sema interface-chain question), #3471 (a code-shape
  cleanup).
  => **the anchor is NOT the campaign's whole answer: 8 of 24 members close on it,
  and 10 need their own sites.** The anchor is the *authority* those sites must
  agree with, not a replacement for them.

**The anchor must be a KIND, not a boolean.** The #7316 reviewer (#7316's
fix, `b6053a68`) is the closest thing to the anchor that exists today, and its
verdict on its own fix is *"PARTLY — it is a CORRECT STEP, NOT THE ANCHOR'S
PREDICATE"*. The part that generalises: `valueLikeChoiceLocalIsCopy` now answers
a question about the **VALUE** ("is this local a copy of a named local or
parameter?"), not about the **SPELLING**. The part that does not: the fix
threads a **boolean** `allowUsingVar` through the mint, hardcoded `true` at two
call sites. **A boolean spelling opt-in can only admit or refuse; it cannot say
WHICH obligation the right-hand side carries** — and the `await using var`
spelling is exactly the spelling a boolean opt-in misses (which is why the
`await` copy of the same value still leaks: #7374). So the anchor is a value
with **five** kinds, computed **from the right-hand side alone**, and every
spelling — `using var`, `await using var`, an explicit `Dispose()`, a copy, a
field store — reads the same kind. The grid's expectations are written against
that kind, not against a spelling: the `value_like choice` head's cells all
expect the SAME thing (compile, run, print, and the runtime lifetime must be
the plain-`var` twin's) whichever spelling the cell uses, and the three
`value_choice` copy cells differ only in where the SOURCE comes from.

**The anchor's site: ONE function, or several?** **Several — four**, and this is
the answer to the brief's question 1:

1. `src/common/` (NEW, next to `common::isDisposableBuiltinHeadName`) — the
   predicate itself, plus the monomorphization-aware representation key.
2. `Emitter::registerUsingDisposal` (src/codegen/LlvmBackend_Setup.cpp) — the
   CODEGEN call site, where the local lambdas `typeMayHoldClass` /
   `awaitUsingSlotMayHoldClass` live today and where the `disposeAsyncFn` /
   `disposeAsyncVSlot` / `asyncDisposeCls` resolutions live.
3. `MethodOwnership::handleLocalDecl` (src/ownership/Ownership.cpp) — the SEMA
   call site, where the §28.9 vacuity lint's Case A arm lives.
4. `Emitter::emitAsyncDisposeAwaitAndFree` (src/codegen/LlvmBackend_Async.cpp) —
   the RUN-TIME dispatch site, where the awaited `DisposeAsync` is called and
   where the run-time half of the anchor lives.

**A MANUAL checklist item for the anchor lane** (the `check_src_citations.py`
gate does NOT catch it — that checker tests RESOLVABILITY, not the line number):
the two comments the #7316 commit (`b6053a68`) added to
`src/codegen/LlvmBackend_Setup.cpp` cite *"spec §28.10, line 7260"* — that line
number is **STALE** at v0.345 (the sentence now lives at line 7276). **Drop the
line numbers, keep the §-section citations.** The quoted WORDING is verbatim
correct at v0.345.

**The control row that catches the campaign's forbidden move** (the house's
"never trade a leak for a UAF"): the grid's `class`/`await_using_var` and
`interface`/`await_using_var` cells assert the destructor chain runs **exactly
once** (`['DTOR']`, `['DA']`), and the `interface`/`field_store` cell asserts the
owned interface-typed field's block is **freed** (no leak). Any anchor that
"fixes" the dispatch by suppressing a disposal instead of dispatching it turns
those cells red (a leak) or red twice (a double free / UAF). Every cell's
program is built with `bezelc --sanitize=address`, so the leak/crash verdicts
are real sanitizer verdicts, and the grid's `NATIVE SANITIZER: 0 check(s)
skipped` line is the counter that says **no cell's ASan check was skipped**
(the ASan runtime was available for every cell that produced a binary; a cell
whose program was rejected never reached the sanitizer at all — it is a WRONG
cell, and it blocks the DONE verdict).


**Membership.** `docs/proposals/papers/master-plan/work/out/campaign-members.tsv`,
rows where `campaign == using-heads`: **24 rows** (severity: 7 soundness, 3 crash,
5 leak, 9 other). Cross-check against §3's prose list ("Members (24)"):

* §3's list and the TSV agree on 22 members.
* §3 lists **#7305** under U1 ("#7316/#7305/#7357 (value-like choice copy path
  leaks)"). #7305 is **CLOSED** (`fix #7305`, `9c65e617`) and is not in the TSV.
* §3 lists **#7345** under U1 ("v0.345: explicit `Dispose()` reclaims at the call
  where the spec says logical disposal only — the holder frees"). The TSV assigns
  #7345 to **`spec-trains`**, not to `using-heads`. It is not counted as a member
  here; §1.5 names it as a candidate for `using-heads` / `producer-temporaries`
  "that their entry checks confirm", and this entry check does not confirm it —
  the shape is a §28.2/§28.6 question about the explicit `Dispose()` call's
  reclamation, which is `producer-temporaries`' subject, not the head-kind question.
* **The tracker label query — ONE statement, measured twice, each number with
  the date it belongs to.** At **this commit's own timestamp**
  (2026-10-27T14:51:59Z = 2026-10-28 03:51 NZDT) the query
  `gh issue list --label "campaign:using-heads" --state open --limit 100`
  returned **34 OPEN** issues: the **19** still-open members of the campaign's
  24 (24 minus the five closed — #7327, #7337, #7283, #7306 and #7322),
  **#7374**, the twelve newly filed **#7386–#7397**, and **#5355** and
  **#7146** (older issues that carry the campaign label) — 19 + 1 + 12 + 2 =
  **34**. **The LIVE query, measured on 2026-10-28 at the `5ea3e73d` timestamp, returned 40**:
  six issues had been labelled since — **#7405, #7406, #7412, #7415, #7416 and #7417**.
  **⚠ RE-DATED BY THE EXIT REVIEW (2026-10-28, later the same day): THE LIVE COUNT IS NOW **39
  OPEN / 4 CLOSED** — #7416 CLOSED SINCE. **THE LABEL COUNT IS A PROPERTY OF A **MOMENT**, NOT OF
  THE CAMPAIGN, SO EVERY COUNT IN THIS RECORD IS DATED AND NONE OF THEM IS "LIVE" IN THE SENSE OF
  BEING CURRENT.**
  ⚠ The **37** that an earlier revision of this record quoted for "2026-10-28"
  counted #7283, #7306 and #7322 as open; they had been closed eight minutes
  before that commit, so **37 was wrong at its own stated date**. An earlier
  section of this record said the same query returned `[]` (0 issues) — that
  was true before the §1.6 labelling sweep and is now superseded. Membership
  below is still from the TSV, as the brief directs.

## Members at entry

All 24 rows re-measured on `d9f11d15` with `scripts/agent_check.sh --probe --asan`
(one program per row; the ASan leg is a real ASan/LSan run — `agent_check.sh` prints
`agent_check: probe run exit=<rc>` and the LSan report follows). "does not reproduce"
means the repro ran and produced the CORRECT behaviour.

| # | severity | HEAD status (MEASURED) | evidence |
|---|---|---|---|
| 1531 | other | **reproduces** | `var x := new AsyncRes(); await x.DisposeAsync();` → `SUMMARY: AddressSanitizer: 16 byte(s) leaked in 1 allocation(s)`. The §28.12 step-2 cascade does not run at the await's completion: with `using var r := new Res(); await r.DisposeAsync();` the owned field's destructor had not run at the await (`mid Cell.n=1`; `final Cell.n=0` only after the scope end). The corpus row `s28_12_0005` is banked `fail` on the same shape. |
| 1603 | other | **reproduces** | The abandoned-task corner: `var r := new Res(); using var t := r.DisposeAsync();` → `mid Cell.n=1 / DA / RES_DTOR / final Cell.n=0` — the cascade follows the scope end, not the task's terminal state. LSan clean at the end, so no separate leak is measured; the divergence is the *timing* §28.12 names ("reclamation follows there"). |
| 2623 | leak | **reproduces** | `h.En := h.Gen().GetAsyncEnumerator();` then `await h.En!.MoveNextAsync();` → `SUMMARY: AddressSanitizer: 72 byte(s) leaked in 1 allocation(s)`, the allocation site `bz$asynciter$P2623.H$Gen$GetAsyncEnumerator`. `fin` is never printed, so the field's Dispose never ran. (The issue records 64 B; the frame is 72 B at HEAD.) |
| 2624 | leak | **does not reproduce** | One shot: LSan reports nothing (`ASAN_OPTIONS=exitcode=77` → rc 1 = the program's own `return sum`, not a sanitizer error). Looped 300×: `sum=300`, LSan clean, rc 0. The 185 B / 2-alloc leak the issue records is absent at HEAD. Fixing commit not isolated by this lane. |
| 3471 | other | **reproduces** (code shape) | `Emitter::registerUsingDisposal` still carries the four collection predicates **twice**: `dictTypeOfLocal` / `linkedListTypeOfLocal` / `listInterfaceTypeOfLocal` / `collectionTypeOfLocal` each appear 2× in the function, `taskTypeOfLocal` 1×. Measured by counting the occurrences in the function body at HEAD. |
| 3482 | soundness | **reproduces** | `await using var s := new T();` at `T = Stopwatch` → `ERROR: AddressSanitizer: SEGV on unknown address 0x000000000010 ... in bz$async$P3482.Wrap$Run$Resume.2` (`rax = 0x10`). The plain `using var` twin leaks `32 byte(s) in 1 allocation(s)` — the twins still disagree. |
| 3483 | soundness | **reproduces** | `await using var d: IAsyncDisposable := new Res();` prints `DTOR` — the destructor, not the awaited `DisposeAsync` (`DA` never printed). |
| 3493 | soundness | **moved — the dispatch half is fixed** | With a `virtual` base `DisposeAsync`, `await using var u: Base := new Der();` prints `DER_DA`: the derived override IS awaited. **MEASURED on this tree** — the fixing commit cannot be named from this tree's history (the earlier text here cited `e9c564fd fix #3494 / narrow #3493`, which is neither an ancestor of `origin/main` nor of this tree; see the anchor's section above). The residual is #3515 (reclamation), which the fix's commit body names. |
| 3515 | leak | **reproduces** | `await using var u: Base := new Der();` with `public owned _d: Cell;` on `Der` → `Cell.n=1` (Der._d never disposed) and `SUMMARY: AddressSanitizer: 16 byte(s) leaked in 1 allocation(s)`. |
| 3516 | soundness | **reproduces** | `await using var u: Abs := new AbsD();` (Abs declares `abstract DisposeAsync`) prints `ABSD_DTOR / ABS_DTOR` — the destructor chain, not `DA`. |
| 5653 | soundness | **reproduces** | `await using var b: Base := new Der();` where `Base` declares no `DisposeAsync` prints `DER_DTOR / BASE_DTOR` — not `DA`. §28.12's own worked example demands `Der.DisposeAsync`. |
| 6194 | crash | **reproduces** | `using var tcs := new TaskCompletionSource<int>(); using var t := tcs.Task; _ := tcs.TrySetResult(77); return await t;` → the AOT run **times out** (`agent_check: probe run exit=124 (124=timeout/hang)`). The plain-`var` twin exits 77. |
| 7283 | crash | **does not reproduce** | `await using var c := lambda(): int => 42;` in an async method → `body`, exit 0, no ASan report. |
| 7306 | soundness | **does not reproduce** | `await using var t := o.ToString();` → `body`, exit 0, no ASan report. |
| 7316 | leak | **reproduces** | `var a := Msg.Note("a" + "b"); using var b := a;` ×101 → `SUMMARY: AddressSanitizer: 3535 byte(s) leaked in 202 allocation(s)`. The plain-`var` twin of the copy is CLEAN (rc 0). |
| 7322 | soundness | **does not reproduce** | `await using var t := o.ToString();` — in an async method and in a non-async one — → `body`, exit 0, no ASan report. |
| 7326 | other | **reproduces** | The same program in a **non-async** `Main` **COMPILES** (`rc 0`) with only the §28.9 vacuity warning. §28.12's "`await using` is valid only inside an async method or async lambda" is not enforced; the parallel §30.2 sentence IS (`'await' is valid only inside an `async` method or `async` lambda (§30.2)`). The corpus row `s28_12_0015` (`expect: reject`) is banked `fail` on exactly this. |
| 7327 | crash | **does not reproduce** | `await using var f := lambda(): int => 1;` → `body`, exit 0, no ASan report. |
| 7328 | other | **reproduces** | `await using var d: IAsyncDisposable := new Solo();` prints `DTOR`, not `DA` — the same shape as #3483, measured separately. |
| 7337 | other | **does not reproduce** | `await using var x: Object := o.ToString();` → `body`, exit 0, no ASan report. |
| 7338 | other | **cannot-build-repro** (forward-looking) | `typeof(Int32)` is rejected: `reflection is not yet implemented: 'typeof' (§17.10, #487); deferred pending System.Reflection §11`. The §28.9 vacuity warning **does** fire on the same binding (`\`using var\` \`x\` is vacuous — the RHS is a value-typed/runtime-managed value`), so sema classifies the `Type` token as runtime-managed while the codegen predicate's runtime-managed set is `{string, function}` — the disagreement the issue reports is confirmed at HEAD; the crash is not reachable. |
| 7339 | other | **reproduces** | `var s: Object := o.ToString(); await using var x: Object := s;` → `SUMMARY: AddressSanitizer: 34 byte(s) leaked in 2 allocation(s)`. |
| 7354 | other | **partly reproduces** | (1) `Gen.Run<T>(x: T): int where T: IDisposable` — **admitted**; (2) `var d: IAsyncDisposable := move r; d.Dispose();` — **admitted**, prints `DTOR` (the destructor, not `DisposeAsync`, as §28.12 step 1 wants); (3) `public record R: IAsyncDisposable … end R;` — **admitted** (no declaration-site error); (4) `public choice C: IAsyncDisposable … end C;` — **admitted** (no declaration-site error). §28.12: "a record, enum or choice that lists it, or lists an interface extending it, is a compile error at the declaration". |
| 7357 | leak | **reproduces** | The `using var` copy of a **PARAMETER** source → `35 byte(s) leaked in 2 allocation(s)`; of a **NULLABLE** source → the same; and the **plain-`var`** copy of a PARAMETER source → the same. Symmetric, exactly as the issue says. |

**Membership, as the tracker sees it.** The plan's §3 campaign-1 row lists
**24** members (the TSV this lane read). **#7327 and #7337 are CLOSED** (both
were in the TSV; both measured "does not reproduce" here, which is consistent
with their closure), and **#7374 is NEW** — the `await using var` copy of a
value-like choice local, filed off the #7316 review and labelled with this
campaign. The grid's `value_choice` / `copy_await_source` cell is #7374's
exact spelling, and it is red. The campaign's label count is stated ONCE, in
the anchor's section above (the query, the date and the number).

**The six "does not reproduce" rows, re-scoped (§2.3: a member is closed by a
fix commit OR re-scoped with a recorded reason — split / blocked-on-ruling /
"not a defect", with the spec sentence quoted).** None of the six is closed by
this lane; the re-scoping is what the §2.3 rule asks for, and the ISSUES stay
with the tracker.

| # | fixing commit | re-scoping (with the deciding spec sentence QUOTED) |
|---|---|---|
| #2624 | none isolated by this lane | **blocked on a ruling.** The repro ran and produced the CORRECT behaviour, and the spec does not name the pending per-advance Task: §29.1's iterator protocol says (spec line 7771, **quoted**: *"Disposing the enumerator (`break` from a `for` or `await for` loop, exhaustion, or `Dispose()`/`DisposeAsync()` on a manually-held enumerator) disposes any outstanding yielded value the iterator **owns**"*) — the per-advance Task is not a yielded value, so whether it is part of the disposal obligation is not decided by the text. The ISSUE is OPEN. |
| #7283 | `87fdfa67 fix #7337, #7327` (the function-typed `await using` shape; #7327 is its re-file) | **not a defect at HEAD.** A function value is runtime-managed, and §28.9 says *"None of them is a class, and none has a `Dispose()` member: `x.Dispose()` over a value of one of these types is a compile error"*, so the `await using` spelling of it carries no disposal obligation, and §28.12's "`await using var` takes §28.10's admission unchanged" admits it. **The ISSUE is CLOSED** (2026-10-27T14:43:18Z), fixed by **`87fdfa67`** — the commit is named in the issue's own closing comment. |
| #7306 | `9bb9387f narrow #7322` (the vacuous `await using` shape) | as #7283, for a string (also runtime-managed, §28.9). **The ISSUE is CLOSED** (2026-10-27T14:43:15Z), fixed by **`9bb9387f`**. |
| #7322 | `9bb9387f narrow #7322` (its own part) and `87fdfa67` (the two shapes it was split into: #7327 and #7337) | as #7283; the commit names this issue. **The ISSUE is CLOSED** (2026-10-27T14:43:21Z). |
| #7327 | `87fdfa67 fix #7337, #7327` | **closed by the fix** — the commit is in `origin/main`; **THE ISSUE WAS CLOSED MANUALLY (ITS `closed` EVENT NAMES NO COMMIT)**. |
| #7337 | `87fdfa67 fix #7337, #7327` | **closed by the fix** — the commit is in `origin/main`. The ISSUE is CLOSED. |

**Summary of the 24:** **reproduces 15** (#1531, #1603, #2623, #3471, #3482, #3483,
#3515, #3516, #5653, #6194, #7316, #7326, #7328, #7339, #7357) · **does not
reproduce 6** (#2624, #7283, #7306, #7322, #7327, #7337) · **moved 1** (#3493) ·
**cannot-build-repro 1** (#7338) · **partly reproduces 1** (#7354). The
authoritative per-row verdicts are the table above.

## Sub-families and lane plan

The master plan §3 proposes U1 / U2 / U3. **All three are split by the §2.2
falsification rule** (a sub-family whose shared site would close fewer than half
its members is split). The shared sites below were read at HEAD (`d9f11d15`).

| id | shared site (file:function @ `d9f11d15`) | members | closes | verdict |
|---|---|---|---|---|
| U1a | `Emitter::registerUsingDisposal`'s local lambdas `typeMayHoldClass` / `awaitUsingSlotMayHoldClass` (src/codegen/LlvmBackend_Setup.cpp) — the codegen half of the anchor | #7338, #3482 | 2 of 2 | **survives**, but it is the anchor's static half, not a separate lane |
| U1b | `MethodOwnership::rhsIsDisposableClassType` (src/ownership/Ownership.cpp) + `common::isDisposableBuiltinHeadName` (src/common/BuiltinRegistry.cpp) — the sema half of the anchor | (the same two, from the sema side) | 2 of 2 | **survives**, merged into the anchor lane |
| U1c | `Emitter::valueLikeChoiceLocalIsCopy` (src/codegen/LlvmBackend_Setup.cpp) — `if (!v.init || v.isUsing) return false;` | #7316, #7357, #7374, and the entry lane's new-9 / new-10 | 5 of 5 | **survives** as its own lane — a different function from the anchor. The grid's FOUR copy cells (a `var` source, the same source in the `await using` spelling, a `const` source, a parameter source) are this lane's DONE instrument; the entry lane measured the parameter source to be a **use-after-free** (new-10), so this lane is a soundness lane, not only a leak lane. |
| U1d | the sema admission of the `await using` spelling (no site: §28.12's "valid only inside an async method or async lambda" is unenforced; §30.2's twin is enforced) | #7326 | 1 of 1 | **survives** as a SEMA lane, not a codegen one |
| U2a | `Emitter::registerUsingDisposal`'s async-disposal arm (`d.disposeAsyncFn` / `d.disposeAsyncVSlot`) + `Emitter::emitAsyncDisposeAwaitAndFree` (src/codegen/LlvmBackend_Async.cpp) + the interface itab build | #3483, #7328, #5653 | 3 of 3 | **survives** — exactly the three the #3493 ruling's option 2 covers |
| U2b | the same arm, but the vtable slot is not read when `methodFns_` has no entry (an `abstract` declaration has no body) | #3516 | 1 of 1 | **survives**, but is **gated on U2c** (the issue and the ruling both say so) |
| U2c | `d.asyncDisposeCls` / `disposerReclaimFns_[cls]` (src/codegen/LlvmBackend_Setup.cpp) + `Emitter::emitAsyncPhysicalReclaim` (src/codegen/LlvmBackend_Disposal.cpp) | #3515 | 1 of 1 | **survives** |
| U2d | `Emitter::lowerAwait`'s direct-await special case and the generic `CallExpr` path (src/codegen/LlvmBackend_Async.cpp) | #1531, #1603 | 2 of 2 | **survives** — a different file and function from U2a |
| U2e | the builtin-interface chain walk (src/sema/TypeResolution*.cpp) | #7354 | 1 of 1 | **survives** as a SEMA lane |
| U3a | the owned-field cascade + the iterator-format slot-1 dispatch (src/codegen/LlvmBackend_Disposal.cpp) | #2623 | 1 of 1 | **survives** |
| U3b | `Emitter::emitFreeAndReturn`'s tail (src/codegen/LlvmBackend_Async.cpp) — the per-advance / pending task release | #2624 | 1 of 1 | **survives**, but the member **does not reproduce** at HEAD |
| U3c | the task-typed local's scope-end disposal (`taskTypeOfLocal` / the task arm, src/codegen/LlvmBackend_Setup.cpp) + the await machinery | #6194 | 1 of 1 | **survives** |
| U0 | `Emitter::registerUsingDisposal`'s two spellings of the builtin-collection disposal build (src/codegen/LlvmBackend_Setup.cpp) | #3471 | 1 of 1 | **survives** — a code-shape cleanup, no behaviour change |

**Falsification verdicts.**

* **§3's U1 SPLITS.** Its members are #7322, #7327, #7283, #7306, #7337, #7338,
  #7339, #3482, #7326, #7316, #7357 (+ #7305 closed, + #7345 not in this campaign)
  = 11 open members in the TSV. The shared site §3 names
  (`awaitUsingSlotMayHoldClass`, in `registerUsingDisposal`) closes **5 of them
  (#7322, #7327, #7283, #7306, #7337) — and all 5 are ALREADY CLOSED** by the
  predicate's predecessors (`9bb9387f narrow #7322`, `87fdfa67 fix #7337, #7327`).
  So it closes **0 of the 6 still-open members**, which is fewer than half ⇒
  **SPLIT** into U1a (the anchor's static half), U1c (the choice-copy path) and
  U1d (the sema admission). #7339 and #3482 do not sit on the predicate either:
  #7339 needs a *run-time* dispatch and #3482 a monomorphization-aware head
  resolution.
* **§3's U2 SPLITS.** 9 members (#3493, #3483, #3516, #3515, #7328, #5653, #7354,
  #1531, #1603). The shared site §3 names (`disposeAsyncFn` resolution in
  `registerUsingDisposal`) closes **3 of 9** ⇒ fewer than half ⇒ **SPLIT** into
  U2a (the dispatch point), U2b (the abstract head), U2c (the reclamation), U2d (the
  direct `DisposeAsync()` path — a different file) and U2e (a SEMA conformance
  question). The #3493 ruling's option 2 is scoped to exactly U2a.
* **§3's U3 SPLITS.** 3 members (#2623, #2624, #6194), three different sites:
  the owned-field cascade, the async iterator's dispose tail, and the task-local
  disposal. The best shared site closes **1 of 3** ⇒ fewer than half ⇒ **SPLIT**
  into U3a, U3b, U3c.

**Lane plan** (serialised by the one-disposal-lane rule until the anchor lands and
is reviewed; owner-ruled §4.7 rule 7 then allows file-disjoint concurrency):

1. **Anchor lane (U1a + U1b, Opus)** — the shared predicate, the two sites that
   call it, the grid's static half. Hot files: `src/codegen/LlvmBackend_Setup.cpp`
   (`Emitter::registerUsingDisposal`, `Emitter::usingVarBuiltinHeadName`),
   `src/ownership/Ownership.cpp` (`MethodOwnership::handleLocalDecl`,
   `MethodOwnership::rhsIsDisposableClassType`), `src/common/BuiltinRegistry.cpp`.
   Members: #7338, #3482. Grid rows: the `generic_unconstrained`, `type_token`,
   `string`, `function` and `value_choice` rows, in EVERY form (the anchor
   decides the head's KIND, so every spelling of every head is its cell set).
   **No owner ruling needed** (§28.9 and §28.10 decide these).
2. **U2a lane (Opus, after the anchor)** — the `DisposeAsync` dispatch point.
   Hot files: `src/codegen/LlvmBackend_Setup.cpp` (the async-disposal arm),
   `src/codegen/LlvmBackend_Async.cpp` (`emitAsyncDisposeAwaitAndFree`), the itab
   build. Members: #3483, #7328, #5653. Grid rows: `interface`, `derived_base`
   (`await_using_var`). Needs the #3493 ruling — **already made (2026-10-27)**.
3. **U2c + U2b lane (Opus, after U2a)** — the reclamation dispatch, then the
   abstract head (which is gated on it). Members: #3515, #3516. Grid rows:
   `abstract`, `derived_base` (`field_store`).
4. **U1c lane (Opus, SOUNDNESS)** — the copy path. Members: #7316, #7357, #7374,
   new-9, new-10, new-12, new-13. Grid rows: the `copy_local_source`,
   `copy_await_source`, `copy_const_source` and `copy_param_source` rows of
   every head. **Start with new-10** (#7387): the `using var` copy of an
   INTERFACE-typed PARAMETER disposes the borrowed parameter (a
   heap-use-after-free, measured with a 20-line repro), and it is the one cell
   in the family that is a memory-safety defect rather than a leak. Then
   new-12 (the same shape over an OWNED parameter — the fault is inside the
   callee) and new-13 (the builtin-collection head — a double free of the
   array block). The `value_like choice` copy cells are #7316/#7374/#7357.
5. **U1d + U2e lane (Sonnet, sema)** — the `await using` admission and the
   `IAsyncDisposable` conformance. Members: #7326, #7354. Not a disposal lane, so
   it may run beside the ownership lanes once the anchor lands.
6. **U2d lane (Opus)** — the direct `DisposeAsync()` physical reclamation.
   Members: #1531, #1603. Hot files: `src/codegen/LlvmBackend_Async.cpp`.
7. **U3a + U3b + U3c lane (Sonnet)** — the async enumerators and the task hang.
   Members: #2623, #2624, #6194.
8. **U0 lane (Sonnet)** — #3471, the duplicated collection build. Last: it is the
   cleanup the other lanes' changes would otherwise re-dirty.

## DONE instrument

**What it is.** `scripts/using_heads_grid.py` — the head-kind × binding-form ×
source-kind grid, in `scripts/regression_matrix.py`'s shape (a deterministic
generator of `.bzl` programs, compiled with `bezelc --sanitize=address`, run,
classified into CLEAN / LEAK / CRASH / CF / TIMEOUT) but with a **spec** oracle
instead of a two-build comparator: every cell carries ONE expectation, and the
grid is green only when every cell's observed behaviour matches it.

**Where it lives.** `scripts/using_heads_grid.py` (this branch). It is
deterministic, needs no network, and writes only under `--out`.

```
scripts/using_heads_grid.py --bezelc build/bezelc --out /var/tmp/bezel-scratch/<lane>/grid \
    [--filter class,string] [--jobs 10] [--timeout 60] [--run-timeout 90] [--report FILE] \
    [--twins] [--check-issues]
```

`--filter` is a comma-separated list of SUBSTRINGS matched against the head
name, the **form name**, the form's label and the head's label (so
`--filter class` selects every head whose name or label contains `class`, and
`--filter copy_await_source` selects the await-spelling copy cells). Exit
status 0 when nothing blocks the DONE verdict (no WRONG, no KNOWN-RED, no
MISMATCH and no TIMEOUT cell), 1 when at least one cell blocks it, 2 on a usage
or infrastructure error (a filter that matches no cell is a usage error, not a
pass). The summary prints `NATIVE SANITIZER: <n> check(s) skipped`; **n must
be 0** — **`<n>` is the SANITIZER's own counter**: the number of cells whose
ASan check was skipped because the ASan RUNTIME was unavailable (measured by
inspecting each built executable for the runtime), and **not** the timeout
count. A program that fails to compile or to link is a REJECTED cell — a
WRONG cell when the cell expected a run — and it blocks the DONE verdict on
its own; there is **no SKIPPED verdict**. A **COMPILE TIMEOUT** is its own
verdict (`TIMEOUT`), it is **not** a sanitizer skip, and it **blocks** the
DONE verdict: a cell whose program hung the compiler is UNMEASURED, and a
DONE instrument must not print YES with an unmeasured cell.

**Where it is WIRED (the reviewer's M2).** The grid itself is **not** at the
gate: it compiles 182 programs and runs them under ASan, which is far too
slow for a gate cycle, and its DONE verdict is the campaign's, not the
repository's. What IS at the gate is the grid's **classifier**:
`scripts/gate/tests/test_using_heads_grid.py` (48 tests) is discovered by
`scripts/pregate.sh` (it runs `python3 -m unittest discover -s
scripts/gate/tests`), so a change to the grid's needles, its KNOWN-RED
mechanism, its quarantine, its agreement check or its report writer fails the
gate. The test uses a FAKE `bezelc` (a shell script), so it is build-free
and takes no compiler. The campaign's DONE run is the grid itself, run by hand
by the campaign's lanes and quoted in this record.

**The report TSV (the reviewer's M3/M4).** `--report FILE` writes one line
per cell and five TAB-separated fields — `head`, `form`, `verdict`,
`observed`, `detail` — plus a header. The detail column is **flattened** (a
newline becomes " | ", a tab a space), so no row splits across physical lines,
and the program path in it is **cell-relative** (`programs/<head>__<form>.bzl`),
so two runs into two different `--out` directories produce **byte-identical**
TSVs. Both properties are asserted in the gate test.

**What a cell's needle must be.** A `REJ` cell's needle names the RULE it
decides — a `§` marker (`§28.10`, `§21.1`, `§8.7`, `§35.8`, `§14.2`, `§17.10`,
`§22.5`, `§27.1`, `§11.3`), or a list of markers when more than one section can
decide the same cell (`['§11.3', '§12.1', '§28.12']` for a member that is not
declared). **A generic token is never a discriminator.** The grid's first
version named `"using var"` for the copy cells, and that token is the §28.3
must-use diagnostic's REMEDY — so a cell that claimed §28.10's "Adopting a
named owned binding" was green on §28.3's rule, and it would have gone RED
when a lane implemented §28.10 properly (that diagnostic's remedies are
`move x` and a `var`/`const` binding, not "using var"). The needle is matched
against the diagnostic that **rejected** the program, not against the whole
compile log: a warning that happens to cite the section (the §28.10 borrow-pin
note does) must not make a cell green.


**The three dimensions.** A cell is **(head kind) × (binding form) × (source
kind)**, and the third one is spelled out rather than implicit, because a
collapsed dimension is a silently unpinned variant:

* **head kinds (17):** `class`, `class_async` (a class implementing
  `IAsyncDisposable`), `derived_base` (§28.12's own `Base`/`Der` example),
  `interface` (an `IAsyncDisposable`-typed head), `abstract` (an
  `abstract DisposeAsync` head), `generic_class` (`where T: new()` at `T = Res`),
  `generic_unconstrained` (`where T: new()` at `T = Stopwatch`, the vtable-less
  head of #3482), `string`, `function`, `value_choice`, `owning_choice`,
  `object_class` (an `Object`-declared head holding a class), `object_string`
  (an `Object`-declared head holding a string), `type_token` (`typeof`,
  deferred), `builtin_collection` (`new Array<int>(3, 0)`),
  `collection_literal` (the same collection as a LITERAL — an owned producer,
  §28.3), `task` (`tcs.Task`).
* **binding forms (9):** `using var`, `await using var`, an explicit
  `Dispose()`, an explicit `DisposeAsync()`, the four copy forms below, and an
  owned field store.
* **source kinds (4, for the copy form only):** the copy's SOURCE is a `var`
  local read by the plain `using var` spelling (`copy_local_source`), the same
  `var` local read by the `await using var` spelling (`copy_await_source`), a
  §21.1 `const` local (`copy_const_source`), or a PARAMETER
  (`copy_param_source`). **These are four different programs with four
  different obligations, so they are four cells.** A `const` source is
  admitted only over a non-owned initializer (§21.1: "The initializer of a
  runtime `const` must not be an owned production"), so the class heads' const
  cells expect a rejection while the value heads' expect a run; a parameter
  source is a BORROW, so the callee's copy is a borrow-pin that disposes
  nothing (§28.10's "`using var` initialized from a borrow"). The `await
  using` spelling of the copy takes §28.10's admission **unchanged** (§28.12),
  so the grid does not merely RE-TYPE the plain copy's expectation — it
  **COMPARES** the two: each head's `using var` / `await using var` pair and
  its `copy_local_source` / `copy_await_source` pair must agree on the
  ADMISSION, and a disagreement is its own verdict (`MISMATCH`), which blocks
  the DONE verdict like a wrong cell. (The grid's first version DERIVED the
  await cell's expectation from the plain cell's — a copied expectation, which
  is exactly the kind of drift the comparison forbids; that is the reviewer's
  H2.) The await cells keep their own needle (`§28.10` or `§28.12`), so a
  correct §28.10 implementation makes them green on the RIGHT rule, and the
  `object_class` pair is **not** compared — that is the OWNER's open question
  (see "Questions for the next train"), and the grid adds no verdict of its
  own there.

17 × 9 = **153 cells**, 9 of which are N/A (the form does not exist for that
head — e.g. `owned` is inadmissible on an `Object` field, §28.1), so **144
runnable cells** — of which the 9 `task` cells are QUARANTINED (see the
oracle discipline below), leaving **135** cells the DONE verdict covers.

**The EXTENDED cells (R37 lane C1-L7, spec v0.350): 29 more cells, 182 in all.**
The product grid has no axis for the shapes the campaign's open members live
in, so seven cell kinds were added as hand-written programs (`EXTRA` in the
script), each expectation read off a quoted spec sentence and never off the
compiler; each has a CONTROL (the same program minus the offending line, or
the corrected spelling) that must RUN, which is what shows the program is a
valid one:

| kind (head) | cells | members | spec sentence the expectation is read off |
|---|---|---|---|
| owned-returning call as the head (`call_rhs_class`, `_class_async`, `_interface`, `_derived_base`) | `using_var`, `await_using_var` x4 | #3483, #7417 | §28.10: an owned-returning call is an owned production, so the `using var` disposes; §28.12: the path is chosen "by the class of the object the binding holds ... never by the binding's static type", so the expectation is the `new` head's |
| adopting an `owned` parameter (`owned_param`) | `adopt_unmarked`, `adopt_unmarked_await` (REJ §28.10); `adopt_move`, `adopt_move_await` (RUN, control) | #7481 | §28.10 "Adopting a named owned binding": the unmarked `using var y := x;` over a named owned binding (an `owned` parameter among them) of class type is a compile error; `move x` adopts |
| the nullable owning-choice parameter (`owned_param_nullable`, with `owned_param_choice` as the #7415 control) | `using_var_param` x2 | #7486 | §28.10 + §15.3: an unmarked read of a move-only binding moves and is an owned right-hand side, "its nullable wrapper `C?` included" |
| pin and move-out (`pin_move_out`) | `pin_then_transfer`, `move_out_of_using_var`, `move_out_after_dispose` (REJ); `pin_no_transfer`, `move_out_of_var` (RUN, controls) | #7406, #7405 | §28.10 "initialized from a borrow": the pin freezes the source, "the frozen kills (a move, a transfer ...) are compile errors"; axiom A5: a `using var` is "never movable out" |
| per-level reclamation (`per_level_reclaim`) | `await_using_virtual_base`, `await_using_plain_base`, `using_var_virtual_base` | #3515 | §28.12 step 2: "owned fields are disposed ... and the base class's reclamation chains, once, immediately after logical disposal has completed" |
| `IOwningCollection<T>` local (`owning_collection_local`) | `using_var_interface`, `using_var_concrete` (control) | #7463 | stdlib 4.2: an owning collection's `Dispose()` "cascades through their elements"; stdlib 4.1: an owned value at an interface type is disposed at `using var` "by dispatch to the runtime type" |
| iterator-frame heads (`async_enumerator`, `sync_enumerator`, `async_enumerable`) | `using_var`, `await_using_var`, `field_store` | #2623, #7422 | §29.7: the disposal of a frame advanced into a `try` "runs the pending cleanup actions ... `finally` clauses"; §28.5: an owned field's value is disposed by its owner |

The cells that are red on the staging compiler today are the ones for the open
members: `call_rhs_interface/await_using_var` (#3483), `call_rhs_derived_base/
await_using_var` (#7417), `owned_param/adopt_unmarked` and `_await` (#7481),
`pin_move_out/pin_then_transfer` (#7406), `pin_move_out/move_out_after_dispose`
(#7405), `per_level_reclaim/await_using_virtual_base` (#3515),
`owning_collection_local/using_var_interface` (#7463), and the two
`sync_enumerator` / `async_enumerable` field cells (#7422). The nullable
parameter cell (#7486) is green: that issue is a coverage gap, not a live bug,
and the cell is what keeps it covered. The agreement check covers these heads
too.

**Expectation kinds.** `REJ:<needle>` (must be rejected with a diagnostic
holding the needle), `RUN:<markers>` (must compile, run clean under ASan, and
print exactly those lines), `VAC:<markers>` (as `RUN`, and the §28.9 vacuity
warning must be present), `NOVAC:<markers>` (as `RUN`, and the §28.9 vacuity
warning must be ABSENT), `TWIN:<markers>` (as `RUN`, and the cell's lifetime
must be the plain-`var` twin's — used only where the twin itself leaks).
`None` marks a cell the spec does not decide; it is reported UNDECIDED and is
not counted wrong.

**Oracle discipline (the house's traps).**

* **Every cell that can make a leak claim LOOPS its operation `LOOP_N` = 60
  times** (inside the 50–500 band). A one-shot LSan repro is VACUOUS: the dead
  frame's stale pointer reads as a root, so a one-shot program can report
  nothing while the leak is real — and a one-shot cell that claims "must not
  leak" cannot fail at all. Every non-`REJ` cell therefore loops, and the
  per-iteration markers are compared `LOOP_N` times over.
* The head's SETUP lives **inside** the loop, so every iteration builds a fresh
  right-hand side (a head whose source is consumed — `move osrc` — or whose
  value is a per-iteration allocation needs one per iteration for the leak
  accounting to mean anything). The one exception is the `Task` head, whose
  `TaskCompletionSource` must outlive the loop: the loop's first iteration's
  scope-end disposal awaits the uncompleted task — that is #6194's hang.
* Every `RUN`-family cell asserts the program's **printed output**, not just
  its exit code. A `REJ` cell asserts the diagnostic **text**, never the exit
  code alone. Every cell runs the ASan-instrumented binary, so the leak/crash
  verdicts are real sanitizer verdicts.
* **A cell that is red because a NAMED issue is still open is reported as
  KNOWN-RED with that issue — and a KNOWN-RED cell BLOCKS the DONE verdict
  just like a wrong one does.** The campaign's DONE criterion is **"no WRONG
  cell, no KNOWN-RED cell, no SPELLING MISMATCH and no TIMEOUT cell"**: a red
  cell is red, whoever owns it, and a cell whose program hung the compiler is
  UNMEASURED. (The grid's first version counted a KNOWN-RED as
  not-wrong and consulted no tracker, so a filter run that printed "5
  known-red" **exited 0** with five cells red and leaking. A KNOWN-RED row
  must now name the commit that fixes the cell — that commit must exist in
  the repository and its subject must name the issue — and `--check-issues`
  re-promotes a KNOWN-RED whose issue is CLOSED to a wrong cell. A cell whose
  issue has no fixing commit yet is a WRONG cell; the record carries the
  attribution.) The KNOWN-RED rows today are the two `value_choice` copy
  cells that #7316's fix (`b6053a68`) covers — see the table below.
* **The `task` row is QUARANTINED out of the DONE count** until #6194 /
  #7395 land. This is a **real runtime race in the compiled program**
  (#7395: 2 of 30 runs hang under ASan, 4 of 30 plain), **not a flaky
  oracle**: the grid and its program are deterministic, and the same
  executable completes on one run and hangs on the next (#6194's hang,
  the `Task` scope-end disposal). A DONE verdict that included the row would
  flip run-to-run. The quarantined cells are counted separately and reported
  on their own line; they never make the verdict better and never make it
  worse.
* **FOREIGN cells (R37, v0.350): shown, but outside the DONE count.** Twelve
  cells are red today for a defect another campaign owns, and campaign 1
  cannot exit before four other campaigns land otherwise. A `FOREIGN(#N)` row
  is printed in the table with the underlying verdict and the issue in its
  text, is counted on its own `FOREIGN:` line, and is held out of the
  verdict: function copy x4 (`copy_local_source`, `copy_await_source`,
  `copy_const_source`, `copy_param_source`; #7288), `collection_literal` x5
  (`using_var`, `await_using_var`, `copy_await_source`, `copy_const_source`,
  `copy_param_source`; #7092), `builtin_collection/copy_param_source` (#7241)
  and `string/explicit_dispose` + `explicit_dispose_async` (#7273). Only
  WRONG / MISMATCH / KNOWN-RED are demoted (a TIMEOUT is unmeasured and
  keeps blocking); a FOREIGN cell that turns OK says to delete its row; and
  `--check-issues` promotes a cell back to blocking when its issue is closed.
  #7241's attribution was re-checked: the vacuity lint's guard
  (`!rhsIsDisposableClassType`) answers `classifyDisposal(...).carriesObligation`,
  the same predicate #7241 names, but the cell is the borrow-pin form rather
  than #7241's owned-holder repro, so it is INFERRED (not proven) from the shared
  predicate, not by a fix. `collection_literal/copy_local_source` is NOT in
  the list (the brief's twelve exclude it), though it is likely gated by
  #7092 too.
* **The two spellings of a program must AGREE (§28.12: "`await using var`
  takes §28.10's admission UNCHANGED").** The grid compares each head's
  `using var` and `await using var` cells, and its `copy_local_source` and
  `copy_await_source` cells, on the ADMISSION (rejected or not). A
  disagreement is its own verdict (`MISMATCH`) and it blocks the DONE verdict
  like a wrong cell. The `object_class` head is NOT judged this way: §28.12's
  "unchanged" against §28.10's adoption rule over an `Object`-declared
  binding is the OWNER's open question, and the grid adds no verdict of its
  own there (its two cells keep the verdicts the reading below gives them).


**Baseline on this tree, measured** (the grid's script, against the compiler at
`d9f11d15` — `git diff --name-only d9f11d15 origin/main` names **no `src/` file**,
so the compiler under test is the same on both; the command is
`python3 scripts/using_heads_grid.py --bezelc build/bezelc --out <dir> --jobs 10
--timeout 60 --run-timeout 90 --report <file> --twins`):

```
153 cells: 82 ok, 43 wrong, 2 known-red, 8 mismatch, 0 undecided, 9 n/a, 0 timeout
QUARANTINED: 9 cell(s) held out of the DONE count (pending #6194): the OK/WRONG split is UNSTABLE — it is NOT quoted as a point value (measured 5 OK/4 WRONG and 6 OK/3 WRONG on two runs of this tree, and 4 OK/5 WRONG on the reviewer's run)
NATIVE SANITIZER: 0 check(s) skipped
DONE: NO — 53 cell(s) block it (wrong + known-red + mismatch + timeout)
```

⚠ **The quarantined row's split is UNSTABLE and is deliberately NOT quoted as
a point value.** The printed line always names the split of THAT run (the grid
counts it), but the split flips run-to-run — it is which of the nine `task`
cells hung, which is #6194's race — so the record quotes the **DONE number**,
which is stable, and labels the split as unstable. (An earlier revision of
this record quoted "6 OK, 3 WRONG" in one place and "5 OK, 4 WRONG" in another
for the same row; both were true of a run, and neither is a property of the
tree.)

=> **53 cells block the DONE verdict** — 43 WRONG + 2 KNOWN-RED + 8 MISMATCH —
of the **135** cells the count covers (153 − 9 N/A − 9 quarantined). **THAT WAS
THE CAMPAIGN'S DONE NUMBER **AT `d9f11d15`**, THE TREE THIS BLOCK MEASURES, AND IT IS EXACT FOR
IT.** **THE CAMPAIGN'S DONE NUMBER **AT THE CURRENT TIP** IS **39**, MEASURED 2026-10-28 ON
`2250daa52` BY THE EXIT REVIEW: **31 WRONG + 0 KNOWN-RED + 8 MISMATCH + 0 TIMEOUT** OF THE SAME
135 CELLS — `153 cells: 96 ok, 31 wrong, 0 known-red, 8 mismatch, 0 undecided, 9 n/a, 0 timeout`,
`DONE: NO — 39 cell(s) block it (wrong + known-red + mismatch + timeout)`. **THE MOVE IS SPLIT
BETWEEN TWO LANES AND BOTH ARE MEASURED WITH THIS RECORD's OWN INSTRUMENT: THE **ANCHOR**
(`e4a55c95`) MOVED **8** CELLS (53 → 45) AND THE **U1c COPIES LANE** MOVED **6** (45 → 39).** THE
CAMPAIGN IS DONE WHEN THE NUMBER IS **0** (no WRONG, no KNOWN-RED, no MISMATCH, no TIMEOUT).

**The two-run determinism proof (the reviewer's H3).** Two consecutive runs of
the same script on the same compiler, into two different `--out` directories:

| | run A | run B |
|---|---|---|
| `153 cells:` | 82 ok, 43 wrong, 2 known-red, 8 mismatch, 0 undecided, 9 n/a, 0 timeout | 82 ok, 43 wrong, 2 known-red, 8 mismatch, 0 undecided, 9 n/a, 0 timeout |
| `QUARANTINED:` | the OK/WRONG split is UNSTABLE (that run: 5 OK, 4 WRONG) | the OK/WRONG split is UNSTABLE (that run: 6 OK, 3 WRONG) |
| `DONE:` | **NO — 53 cell(s) block it** | **NO — 53 cell(s) block it** |

and the two runs' report TSVs are **byte-identical on all 144 non-quarantined
rows**
(`diff <(grep -v QUARANTINED reportA.tsv) <(grep -v QUARANTINED reportB.tsv)`
⇒ no difference at all). The only rows that differ are the nine **quarantined**
`task` rows, and there the difference is *which* of them hung on that run
(#6194's hang) — the DONE verdict is identical, which is the whole point of
the quarantine. (Before the quarantine, the same race made the DONE line flip
between 100 ok/39 wrong and 99 ok/40 wrong **on one build**.)

**The coverage gap the grid DECLARES (the reviewer's L6).** Master plan §3's
head-kind list says *"Object holding EACH OF THESE"* (ten heads), and the grid
carries **two** `Object`-declared shapes (`object_class`, `object_string`).
The gap is **eight Object-declared heads × 9 binding forms = 72 cells**, and
they are ABSENT — not red, not green, simply not measured. The eight heads are:
derived-behind-base, interface, abstract, generic `T` (×2), function value,
value-like choice, owning choice. **The DONE number above therefore covers 153
of the 225 cells the master plan's head-kind list implies**, and the campaign
is not DONE until the 72 are added (or the owner rules that the two
`Object`-declared shapes stand for the eight).
**⚠ CORRECTED BY THE EXIT REVIEW's FINDING 12 (2026-10-28): THE "225" AND THE "72" ARE BOTH
WRONG, IN OPPOSITE DIRECTIONS. **THE PLAN's HEAD-KIND LIST NAMES **TEN** HEADS BEFORE "Object
holding each of these" — class, derived-behind-base, interface, abstract, generic `T` (×2), string,
function value, value-like choice, owning choice — **TEN** OBJECT-EXPANDABLE HEADS, PLUS `Type`,
built-in collection, **ASYNC ENUMERATOR** AND `Task`: **24 HEADS × 9 = 216 CELLS**, NOT 225.
AND THE GRID's SEVENTEENTH HEAD, `collection_literal`, IS **ONE THE PLAN DOES NOT NAME** — so the
earlier figure counted the grid's EXTRA head as if the plan had named it, and in exchange DROPPED
THE PLAN's `async enumerator` HEAD FROM THE GAP. **=> THE TRUE GAP IS **81 CELLS** (NINE MISSING
HEADS × 9), NOT 72 — THE NINTH BEING THE **ASYNC ENUMERATOR**, **AND #2623 IS A MEMBER WHOSE HEAD
KIND IS THEREFORE NOT MEASURED AT ALL.** THE DONE NUMBER ABOVE COVERS **153 OF THE 216** CELLS THE
PLAN IMPLIES. THE INSTRUMENT IS NOT CHANGED BY THIS EDITION — `scripts/using_heads_grid.py` IS THE
COORDINATOR's FILE AND ADDING A HEAD IS AN INSTRUMENT CHANGE THAT MUST BE REVIEWED AND RE-BASED;
THE NEXT LANE PROPOSES THE CELLS AND THE COORDINATOR MAKES THE CHANGE.** This lane adds none of them: it
is a read-mostly entry check and the fix-forward's scope was the instrument,
not the coverage.

**⚠ A COVERAGE HOLE IN THE INSTRUMENT ITSELF — THE EXIT REVIEW's FINDING 11 (2026-10-28).**
**THE `object_class` HEAD's CELLS EXPECT `('RUN', ['DTOR'])` — A PLAIN RUN, WHICH DOES NOT LOOK AT
A WARNING — AND THE §28.9 **VACUITY WARNING FIRES WRONGLY** ON AN `Object`-DECLARED CLASS BINDING
AT **BOTH** `d9f11d15` AND THE TIP** ("the RHS is a value-typed/runtime-managed value", WHILE THE
RUNTIME VALUE IS A CLASS INSTANCE). **=> THAT DEFECT — THE RECORD's OWN QUESTION 2 — IS INVISIBLE TO
THE DONE VERDICT: THE GRID'S DONE LINE CAN BE SATISFIED WHILE IT STANDS.** **THE `object_class`
CELLS NEED `NOVAC`, NOT `RUN`.** THE GRID SCRIPT IS THE COORDINATOR's FILE; THE CHANGE IS NOT MADE
BY THIS EDITION AND IS THE NEXT LANE's PROPOSAL.

**⇒ DONE 2026-10-29 (R37 lane C1-L7, spec v0.350): THE `object_class` RUN-FAMILY CELLS NOW EXPECT
`NOVAC`.** Four cells that were green turned RED on purpose — `object_class/using_var`,
`/copy_local_source`, `/copy_const_source`, `/copy_param_source` ("a §28.9 vacuity warning was emitted
where none is due"); `/await_using_var` and `/copy_await_source` were already red for #7390 and will
meet the same warning when #7390 lands. The warning is the defect the new tracker issue (filed from
`r37-c1-l7-grid/issue-novac.md`) carries; until it is fixed, those cells are WRONG, not hidden.

**The fail-without / fail-with proof for the grid's new cells.** The grid is
new, so it has no fail-without of its own; the #7316 fix (`b6053a68`, "narrow
#7316: a `using var` copy of a value-like choice local releases the payload at
scope end") is the one commit that moves a cell, so it is the proof. Two
worktrees are built at the two commits — `d9f11d15` (the base) and
`b6053a68` (the fix) — and the SAME (fix-forward) grid is run against each
compiler:

| cell | base (`d9f11d15`) | fix (`b6053a68`) |
|---|---|---|
| `value_choice` / `copy_local_source` | KNOWN-RED `#7316` | **OK** |
| `value_choice` / `copy_const_source` | KNOWN-RED `#7316` | **OK** |
| `value_choice` / `copy_await_source` | WRONG `#7374` | WRONG `#7374` |
| `value_choice` / `copy_param_source` | WRONG `#7357` | WRONG `#7357` |
| `value_choice` / `await_using_var` | WRONG `#7374` | WRONG `#7374` |

(the whole grids: base `153 cells: 82 ok, 43 wrong, 2 known-red, 8 mismatch, 0
undecided, 9 n/a, 0 timeout` ⇒ `DONE: NO — 53 cell(s) block it (wrong +
known-red + mismatch + timeout)`; fix `153 cells: 84 ok, 43 wrong, 0
known-red, 8 mismatch, 0 undecided, 9 n/a, 0 timeout` ⇒ `DONE: NO — 51 cell(s)
block it (wrong + known-red + mismatch + timeout)`. **The summary's FORMAT
changed in the grid-discharge commit** — the timeout count is no longer
labelled "skipped", and the DONE line names the timeout — so those two
measurements are quoted in the current format; the NUMBERS are the same. The
base's numbers are also the entry tree's own numbers, which is the expected
result: `git diff --name-only d9f11d15 origin/main` names no `src/` file, so
the two builds are the same compiler.)

=> the grid **discriminates the `var` source from the `const` source, and both
from the `await using` spelling and from the parameter source**: the fix turns
the two it covers green and leaves the two it does not (#7374, #7357) red.
The `const` source and the `await using` spelling were unpinned by anything
before this lane; they are now cells. Note also that the fix's two green cells
carry **no KNOWN-RED** on the fix arm: a KNOWN-RED is a *demotion of a red
cell*, so a cell the fix turns green is simply green — the demotion is not a
state a cell can be in for ever.

(The `Task` row is QUARANTINED, and the quarantine is what replaced the old
"three `task` cells flip between the two builds" note: the row's cells hang
on one run and not on the next **on the same compiler**, so it is counted
separately and never moves the DONE verdict. See the determinism proof above.)

**The wrong cells on this tree, with the member each belongs to (43 rows; the
headline and the table agree):**

| cell | observed | member |
|---|---|---|
| `class` / `copy_local_source` | compiled, expected rejection | §28.10's "Adopting a named owned binding" (#5582 / `s28_10_0015`) |
| `class_async` / `copy_local_source` | compiled, expected rejection | as above |
| `derived_base` / `copy_local_source` | compiled, expected rejection | as above |
| `interface` / `copy_local_source` | compiled, expected rejection | as above |
| `abstract` / `copy_local_source` | compiled, expected rejection | as above |
| `generic_class` / `copy_local_source` | compiled, expected rejection | §28.10's bare-type-parameter clause |
| `generic_unconstrained` / `copy_local_source` | compiled, expected rejection | as above |
| `builtin_collection` / `copy_local_source` | compiled, expected rejection | as above |
| `collection_literal` / `copy_local_source` | compiled, expected rejection | as above |
| `class_async` / `explicit_dispose_async` | LEAK 960B/60 | #1531 / new-1 |
| `abstract` / `explicit_dispose_async` | LEAK 960B/60 | #1531 / new-1 |
| `interface` / `explicit_dispose_async` | rejected, expected a run | #3483 (the built-in `IAsyncDisposable` member has no dispatch arm, #2457) |
| `derived_base` / `await_using_var` | `['DER_DTOR','BASE_DTOR']` ≠ `['DA','BASE_DTOR']` | #5653 |
| `interface` / `await_using_var` | `['DTOR']` ≠ `['DA']` | #3483 / #7328 |
| `abstract` / `await_using_var` | `['ABSD_DTOR','ABS_DTOR']` ≠ `['DA','ABS_DTOR']` | #3516 |
| `interface` / `field_store` | LEAK 960B/60 | an `owned IAsyncDisposable?` field's cascade (new-4) |
| `generic_unconstrained` / `using_var` | LEAK 1920B/60 | #3482 |
| `generic_unconstrained` / `await_using_var` | CRASH: SEGV | #3482 |
| `generic_unconstrained` / `copy_param_source` | LEAK 1920B/60 | #3482's family |
| `string` / `copy_local_source` | LEAK 2040B/120 | the `using var` copy of a runtime-managed value (new-9) |
| `string` / `copy_await_source` | LEAK 2040B/120 | as above, in the `await using` spelling |
| `string` / `copy_const_source` | LEAK 2040B/120 | as above, with a `const` source |
| `string` / `explicit_dispose` | rejected, but no `§28.9` in the diagnostic | the diagnostic says only *"string member `.Dispose` is not yet supported in codegen"* — **it names no rule** (see "diagnostics that name no rule", below) |
| `string` / `explicit_dispose_async` | rejected, but no `§28.9`/`§28.12` in the diagnostic | as above, for `.DisposeAsync` |
| `function` / `copy_local_source` | LEAK 3840B/60 | as `string` / `copy_local_source` |
| `function` / `copy_await_source` | LEAK 3840B/60 | as above |
| `function` / `copy_const_source` | LEAK 3840B/60 | as above |
| `function` / `copy_param_source` | LEAK 3840B/60 | as above, with a parameter source |
| `value_choice` / `await_using_var` | LEAK 2100B/120 | **#7374** — the `await using var` copy of a value-like choice local leaks (new-3) |
| `value_choice` / `copy_await_source` | LEAK 2100B/120 | **#7374** — the `await using var` spelling of the copy (new-3) |
| `value_choice` / `copy_param_source` | LEAK 2100B/120 | **#7357** — the parameter-source copy still leaks |
| `interface` / `copy_param_source` | **CRASH: heap-use-after-free** | the callee's `using var` DISPOSES the BORROWED parameter; the caller then reads freed memory (new-10 / **#7387**) |
| `owning_choice` / `copy_param_source` | **CRASH: heap-use-after-free** | the callee's `using var` copy disposes the block the callee's own binding still owns — the free AND the read are in the callee's frame (new-12) |
| `builtin_collection` / `copy_param_source` | **CRASH: attempting a double free** | the collection's array block is freed twice, both frees in the caller's frame (new-13) |
| `collection_literal` / `copy_param_source` | **CRASH: attempting a double free** | as above (new-13) |
| `object_class` / `await_using_var` | rejected, expected a run | §28.12's "takes §28.10's admission unchanged" (new-2) |
| `object_class` / `copy_await_source` | rejected, expected a run | as above, in the `await using` spelling (new-2) |
| `object_class` / `explicit_dispose_async` | rejected, but no `§8.7`/`§28.12` in the diagnostic | the program is rejected by a **§28.6 borrow** diagnostic instead (*"cannot call `Dispose()` on `x`: it is a borrow (an alias) of `s`, which still owns it"*) — the cell asked for the §8.7 "no such member" rejection |
| `object_string` / `explicit_dispose_async` | compiled, expected rejection | §8.7 (`DisposeAsync` is a member only of a type that declares it) — new-5 |
| `collection_literal` / `using_var` | a §28.9 vacuity warning where none is due | #7092 / `s28_10_0016` |
| `collection_literal` / `await_using_var` | a §28.9 vacuity warning where none is due | as above |
| `collection_literal` / `copy_await_source` | compiled, expected rejection | as `collection_literal` / `copy_local_source` |
| `collection_literal` / `copy_const_source` | compiled, expected rejection | §21.1 (a `const` initializer must not be an owned production) |

**The MISMATCH cells on this tree (8 rows)** — the two spellings of the same
program disagree on the ADMISSION, which §28.12 forbids ("`await using var`
takes §28.10's admission UNCHANGED"). Every one of them is the same shape: the
plain `copy_local_source` spelling is **admitted** (the §28.10 rejection does
not exist yet) and the `copy_await_source` spelling is **rejected** — by §28.3's
must-use error, which is the *neighbouring* rule.

| cell | the disagreement |
|---|---|
| `class` / `copy_await_source` | `copy_local_source` admitted, `copy_await_source` rejected (§28.3) |
| `class_async` / `copy_await_source` | as above |
| `derived_base` / `copy_await_source` | as above |
| `interface` / `copy_await_source` | as above |
| `abstract` / `copy_await_source` | as above |
| `generic_class` / `copy_await_source` | as above |
| `generic_unconstrained` / `copy_await_source` | as above |
| `builtin_collection` / `copy_await_source` | as above |

(The ninth `copy_await_source` cell, `collection_literal`, is a **WRONG** cell
in its own right — both spellings are admitted there, so the two AGREE and
there is no mismatch to report.)

**The KNOWN-RED cells on this tree (2 rows)** — red, attributed to an open
issue, **and still blocking the DONE verdict** (the reviewer's M1: a red cell
is red, whoever owns it):

| cell | observed | issue | fixing commit |
|---|---|---|---|
| `value_choice` / `copy_local_source` | LEAK 2100B/120 | **#7316** — the `using var` copy of a value-like choice local leaks | `b6053a68` (not yet in `origin/main`) |
| `value_choice` / `copy_const_source` | LEAK 2100B/120 | **#7316** — the same, with a `const` source | `b6053a68` (not yet in `origin/main`) |

**The cells that were KNOWN-RED in the grid's first version and are now WRONG.**
The first version demoted five `value_choice` cells to KNOWN-RED with no fixing
commit and no tracker consultation; three of them had **no commit that fixes
them**, so the fix-forward's rule ("a KNOWN-RED row must name the fixing
commit, and a cell whose issue has no fixing commit is a WRONG cell") demotes
them back:

| cell | issue | why it is not a KNOWN-RED row |
|---|---|---|
| `value_choice` / `await_using_var` | #7374 | no fixing commit exists yet |
| `value_choice` / `copy_await_source` | #7374 | no fixing commit exists yet |
| `value_choice` / `copy_param_source` | #7357 | no fixing commit exists yet |

**Diagnostics that name no rule.** Two of the wrong cells are red because the
compiler's diagnostic carries **no `§` marker at all**, so the grid cannot
discriminate the rule the cell claims:

* `string` / `explicit_dispose` ⇒ *"string member `.Dispose` is not yet
  supported in codegen"*;
* `string` / `explicit_dispose_async` ⇒ *"string member `.DisposeAsync` is not
  yet supported in codegen"*.

The REJECTION is right (§28.9: *"None of them is a class, and none has a
`Dispose()` member: `x.Dispose()` over a value of one of these types is a
compile error"*), but the diagnostic makes no claim the reader can check — the
house rule that every diagnostic cite the rule it enforces is not met here.
This is a **compiler** finding, reported to the coordinator; the grid cannot
relax it, because a needle that accepts an unnamed diagnostic is exactly the
generic-token needle the reviewer's H1 forbids.

**THE SPEC QUESTION IS ANSWERED, AND THE COORDINATOR'S MEASUREMENT SAYS WHICH
ANSWER.** The issue that tracks the diagnostic — **#7273** — was filed against
the **v0.344** text, where §28.9 read *"Their `Dispose()` is a no-op (or, for
types that are not classes and therefore have no `Dispose()` at all …)"* and the
coordinator's ruling on it was **WITHDRAWN** (the spec was in tension). **§28.9
AT v0.345 READS DIFFERENTLY AND DECISIVELY**: *"The following types are exempt
from manual disposal (K1.6's runtime-managed exemption). **None of them is a
class, and none has a `Dispose()` member (§8.7): `x.Dispose()` over a value of
one of these types is a compile error**, and a `using var`, a discard or scope
end over such a value disposes nothing …"* — and `string` is the list's FIRST
ENTRY. §8.7 supplies the `DisposeAsync` half: *"`DisposeAsync()` is a member
only of a type that declares or inherits it"*, and a `string` declares and
inherits neither.

**=> THE CELL EXPECTATIONS ARE SETTLED (§28.9 for the `string` head), AND #7273
IS A PLAIN COMPILER DEFECT, NOT A SPEC QUESTION.** THE COORDINATOR POSTED THE
`v0.345` READING, THE `.DisposeAsync` TWIN (WHICH #7273'S TITLE AND BODY DO
NOT NAME), AND THE FACT THAT **THE TWO `string` CELLS ARE RED ON THIS ISSUE**
ON #7273. **#7273 STAYS IN THE `spec-trains` BUCKET** — THE TRACKER HAS **0
DOUBLE-LABELLED AND 0 UNLABELLED** OPEN ISSUES, SO A SECOND BUCKET LABEL WOULD
BREAK THAT INVARIANT. **THE DEPENDENCY HERE IS A CROSS-CAMPAIGN DEPENDENCY,
NOT MEMBERSHIP: CAMPAIGN 1'S GRID CANNOT CLEAR ITS TWO `string` CELLS UNTIL
#7273 LANDS.**

**The `type_token` row is pinned to a DEFERRAL, and #487 will re-author it.**
All nine `type_token` cells are green because the program is rejected with
*"reflection is not yet implemented: 'typeof'"* — the `typeof` deferral
(#487), not any disposal rule. The cells' needle is therefore `§17.10`, and
they measure the deferral, not the head kind: **when #487 lands, every one of
the nine must be re-authored** (the `typeof` right-hand side will then reach
the disposal rules and the cells' expectations become the real ones). The row
is kept as it is — it is a real head kind, and a deferral-pinned row that says
so is better than a row that pretends to measure §28.9.

**The plain-`var` twins.** §28.10 makes the `using var` spelling's RUNTIME
LIFETIME the same as the plain-`var` twin's ("the only effect is the
reassignability restriction"), so the twin is the reference the grid measures
against. The grid measures it per head (`--twins`): every head's twin is CLEAN
except **`object_string`, whose twin LEAKS 2040B/120** — an `Object`-declared
slot holding a runtime-managed string does not release it. That is why the
`object_string` cells are the grid's only `TWIN` cells: the `using var`
spelling inherits the twin's retention and adds none of its own (the defect is
the `Object` slot's, not the `using var`'s — new-11). The other two
exceptions, measured in the same run: **`generic_unconstrained`'s twin leaks
32B/1** (the same generic-head leak as #3482) and **`type_token`'s twin does
not build at all** (CF — the `typeof` deferral, §17.10, so the twin has no
runtime lifetime to compare).

**Dedup against `tests/conformance/rules/cases/` (done FIRST, as the brief
requires).** The corpus carries 18 `s28_10_*` and 29 `s28_12_*` cases. Mapping
them onto the grid's cells (the criterion: a corpus case pins a cell when it
exercises the same head kind and the same binding form):

* **18 of the grid's cells are already pinned by an existing corpus case**; the
  rest are new. The mapping (corpus case → grid cell):
  `s28_10_0001`, `s28_10_0002`, `s28_10_0003`, `s28_10_0004`, `s28_10_0014`,
  `s28_10_0017`, `s28_12_0014` → class/using_var · `s28_10_0007`,
  `s28_10_0008` → string/using_var · `s28_10_0013`, `s28_10_0015` → the
  `copy_local_source` cells · `s28_10_0016` → builtin_collection/using_var ·
  `s28_12_0001` → class_async/await_using_var · `s28_12_0002` → class_async/using_var ·
  `s28_12_0005`, `s28_12_0006` → class_async/explicit_dispose_async ·
  `s28_12_0007` → derived_base/await_using_var · `s28_12_0010` → abstract/await_using_var ·
  `s28_12_0011` → abstract/using_var · `s28_12_0015`, `s28_12_0016`, `s28_12_0027` → string/await_using_var ·
  `s28_12_0018` → function/await_using_var · `s28_12_0019` → class/await_using_var ·
  `s28_12_0020`, `s28_12_0021`, `s28_12_0022` → interface/await_using_var ·
  `s28_12_0023`, `s28_12_0024` → object_class/await_using_var ·
  `s28_12_0025` → derived_base/await_using_var · `s28_12_0026` → object_string/await_using_var ·
  `s28_12_0029` → interface/using_var.
* The corpus's `s28_12_0013` ("a second `DisposeAsync` during the first's
  window") and `s28_12_0014` ("a destructor body's call of an instance method of
  `this`") cover shapes the grid does not have a cell for — they are **not** in
  the 18; a later lane may add cells for them.
* The corpus's `s28_12_0016` … `s28_12_0028` (the runtime-managed, value-typed,
  boxed and `Object`-declared heads) pin cells the grid does NOT have (a
  `boxed value type` head, an `int` head): the grid has no `int` head and no
  `boxed value` head. Those two head kinds are a coverage gap a later lane may
  close.
* The 18 already-covered cells still count in the baseline: the grid runs them,
  so a fix that turns a corpus case green also turns a grid cell green. That is
  the point of the grid being the DONE instrument rather than the corpus.
## Rulings needed

| issue | what it unblocks | asked on |
|---|---|---|
| **#3493** | **RULED, owner-approved 2026-10-27** (verified in the issue's comments). *"Ruling: dispatch on the run-time object, through a `DisposeAsync` dispatch-table slot (this issue's option 2)."* The issue's own words: *"A trailing `DisposeAsync` dispatch entry covers this issue, #5653 (a `Base` that does not implement `IAsyncDisposable`) and #3483's interface shape, with one path instead of three static special cases."* **What it rules:** the *mechanism* of the run-time dispatch — a trailing `DisposeAsync` entry on the `IAsyncDisposable` itab (the #2506 shape), not a per-site static special case. **What it leaves open:** (a) the physical-reclamation half stays with **#3515**, whose two routes (a synthetic vtable slot `DisposeAsync$reclaim` vs a trailing itab entry) are recorded in that issue and are NOT chosen by the ruling; (b) the abstract-class head **#3516** stays gated on #3515; (c) **#7339** (a run-time STRING in an `Object`-typed slot) is not named by the ruling — it is a *value* dispatch, not a `DisposeAsync` dispatch, and needs its own answer; (d) the class-typed head's `DisposeAsync` is already dispatched through the vtable slot (MEASURED on this tree: the program prints `DER_DA` — see the anchor's section above; the fixing commit cannot be named from this tree's history), so the ruling's itab route is for the heads that have no vtable. | 2026-10-27 (already ruled) |

**No other ruling is needed for the campaign.** U1 (the anchor's static half) needs
none: §28.9 (v0.350) already says which values have no `Dispose()`, and §28.10
(v0.350) says the `using var` disposal at scope end is a no-op for them.
**The campaign therefore does not need an owner ruling before U1 can land.**

**Two dependencies on campaign 0** (master plan §3's campaign-0 list, as amended
on `origin/main` `68212aea` — this lane did not request either):

* **(f) kernel pins for the ownership corpus.** The enabler is "wanted by any
  campaign whose DONE grid cites kernel clauses", and this grid's expectations
  quote §28.9, §28.10 and §28.12 — the sections that carry K1.6 (the
  runtime-managed exemption), K2 (no obligation from a borrow) and K3.4 (the
  adoption rejection) — so the grid qualifies in substance. It does **not**
  block the campaign: the grid's expectations are spec sentences today, and (f)
  would add verbatim `rule:` quotes and a `kernel:` header to the *corpus* cases.
  If (f) lands, the grid's expectations can be pinned the same way.
* **(g) the model-vs-compiler differential.** Its findings are "matched against
  open issues and get a campaign label at intake", so the coordinator should
  check whether the `bzcore` model's findings cover this campaign's shapes —
  and in particular the eight sibling defects this lane found (below), which the
  differential may already have predicted.

**The plan's corpus counts for this campaign are stale — MEASURED.** Master plan
§3's campaign-1 entry and §2.3's DONE line both say "baseline has 2 under §28.10
and 5 under §28.12 today" and the §6 table's cell repeats "§28.10: 2, §28.12: 5".
The measured counts are **4 and 6**, and there are **three more divergences in
§28.12 that the baseline has no row for**. The measurement, run on HEAD with the
repo's baseline copied out of the way (`cp tests/conformance/rules/baseline.tsv
<copy>` then `--baseline <copy>`, so the repo's baseline is never written):

```
$ python3 tests/conformance/rules/runner.py build/bezelc --baseline <copy> --filter 28.10
18 cases: 13 ok, 4 known-divergent, 1 unclassified, 0 REGRESSION, 0 UNBANKED
$ python3 tests/conformance/rules/runner.py build/bezelc --baseline <copy> --filter 28.12
29 cases: 7 ok, 6 known-divergent, 16 unclassified, 0 REGRESSION, 0 UNBANKED
```

* **§28.10 — 4 banked `fail` rows** (the plan says 2):
  `s28_10_0009` (divergence), `s28_10_0013` (#5644), `s28_10_0015` (#5582),
  `s28_10_0016` (#7092). One further row, `s28_10_0018`, has no baseline row
  but PASSES ("using var over a value-type right-hand side disposes nothing at
  scope end").
* **§28.12 — 6 banked `fail` rows** (the plan says 5): `s28_12_0003`
  (divergence), `s28_12_0005` (divergence), `s28_12_0006` (divergence),
  `s28_12_0007` (#5653), `s28_12_0008` (#5654), `s28_12_0015` (divergence).
* **§28.12 — 3 UNBANKED failures** (no baseline row at all): `s28_12_0013`
  (expected `second returned / async cleanup / done`, got `async cleanup /
  async cleanup / second returned / done / sync cleanup`), `s28_12_0020` and
  `s28_12_0021` (both expected `body / ASYNC`, got `body / SYNC`).
  => **the true divergence count in scope is 4 (§28.10) and 9 (§28.12)**, and
  the DONE criterion "the `s28_10_*`/`s28_12_*` fail rows re-banked" has nine
  rows to re-bank under §28.12, not five, once the three unbanked ones are
  banked. The other 13 unclassified §28.12 rows PASS.
* **⚠ RE-MEASURED BY THE EXIT REVIEW (2026-10-28): THE "9" ABOVE IS NOW **6**, AND THE TWO
  `s28_12_0020` / `s28_12_0021` ROWS **NOW PASS** — THE ANCHOR FIXED THEM (THE GRID's
  `value_choice/copy_local_source` AND `value_choice/copy_const_source` CELLS WERE KNOWN-RED AT
  `d9f11d15` AND ARE OK AT `e4a55c95` AND AT THE TIP).** **SO: §28.12's IN-SCOPE DIVERGENCE COUNT
  IS **6** — 5 BANKED `fail` ROWS (`s28_12_0003` `s28_12_0005` `s28_12_0006` `s28_12_0008`
  `s28_12_0015`) PLUS `s28_12_0013`, WHICH STILL FAILS WITH NO BASELINE ROW. `s28_12_0007` IS THE
  **KNOWN LAST-STEP RE-BANK** (BANKED `fail`, NOW PASSING) AND **MUST BE RE-BANKED `pass`** —
  IT IS NOT A DIVERGENCE. AND `s28_12_0020` / `s28_12_0021` **MUST BE ADDED AS `pass` ROWS, NOT
  `fail`** — THE EARLIER "EXPECTED `body / ASYNC`, GOT `body / SYNC`" IS NO LONGER TRUE OF THE
  TREE.** THE WHOLE CORPUS ON THE TIP READS **3693 cases, 0 REGRESSION, 6 UNBANKED**, AND ALL SIX
  UNBANKED ROWS ARE "BANKED `fail`, NOW PASSING" — `s28_12_0007` (CAMPAIGN 1's),
  `s22_6_0112`/`s22_6_0113`/`s22_6_0114`/`l9_5_0004` (CAMPAIGN 2's ANCHOR) AND `l9_6_0027`.

This lane did **not** edit `tests/conformance/rules/baseline.tsv` (the brief
forbids it): the three unbanked rows are the coordinator's to bank, and
`runner.py --classify --filter <case-id>` re-banks **per row** (a full
`--classify` rewrites every row's note).

## Questions for the next train

**⚠ THE EXIT REVIEW's FINDING 9: THIS LIST'S PREAMBLE CLAIMED FILED SPEC-QUESTION NUMBERS AND
LISTED NONE. **THE HONEST STATEMENT IS THIS: **EVERY QUESTION IN THIS LIST IS AN **IMPLEMENTATION
DEFECT**, NOT A SPEC QUESTION — EXCEPT WHERE A §-CITATION IS NAMED AS THE GOVERNING SENTENCE, AND
THEN THE QUESTION IS "WHICH SECTION GOVERNS", NOT "WHAT SHOULD IT SAY".** THE ONLY OPEN ISSUE
CARRYING BOTH `spec-question` AND `campaign:using-heads` IS **#7412** ("may a `using var` be the
operand of `throw`?"), **WHICH IS NOT IN THIS LIST** — IT IS A QUESTION FOR THE NEXT TRAIN AND IS
CARRIED BY ITS OWN ISSUE. **THE TRACKER REFUSED THE COORDINATOR's NEW-ISSUE CREATE THIS SESSION, SO
NO NEW SPEC-QUESTION ISSUE EXISTS FOR ANY QUESTION HERE.**

1. **The `Object`-declared `await using` admission.** §28.12 says "`await using var`
   takes §28.10's admission unchanged". §28.10's "Adopting a named owned binding"
   rejection needs a *class or interface* declared type; `Object` is neither
   (§8.7), so the binding is a borrow-pin and the program is admitted. MEASURED:
   the plain-`using var` spelling of the same program is admitted (with the
   auto-drop warning) and the `await using` spelling is **REJECTED** (with the
   §28.3 must-use *error*, "owned value in `s` would be dropped here without
   disposal"). The two spellings disagree; §28.12 forbids that. → **a campaign-1
   implementation defect**, not a spec question, but the hunk to hold out is
   §28.12's "unchanged" sentence if the owner reads it as deciding the rejection.
2. **§28.9's vacuity lint over an `Object`-declared binding.** The lint fires
   ("the RHS is a value-typed/runtime-managed value") on a binding whose runtime
   value is a CLASS instance. §28.9's exempt set is `{string, Type, boxed value
   types, function values}`; `Object` is not on it and the runtime value is a
   class. §28.12: "never by the binding's static type". → a campaign-1
   implementation defect (the lint must classify by the representation).
3. **The `Object`-declared `DisposeAsync` precedence — NOW A MEASURED FINDING, NOT
   A QUESTION (the coordinator, 2026-10-28).** The grid's
   `object_class` / `explicit_dispose_async` cell expects the **§8.7** "no such
   member" rejection. **MEASURED BY THE COORDINATOR: THE SYNC TWIN EMITS BOTH
   RULES** (*"no member `Dispose` on type `Object`: `Dispose()` is class-only
   (§8.7, v0.340) …"* **AND** the §28.6 borrow error), **AND THE ASYNC TWIN
   EMITS ONLY THE §28.6 BORROW ERROR — THE §8.7 TYPE-LEVEL REJECTION IS
   ABSENT ENTIRELY.** **=> IT IS NOT A PRECEDENCE DIFFERENCE, IT IS A MISSING
   CHECK: ONE MECHANISM (THE §8.7 TYPE-LEVEL CHECK FOR `DisposeAsync` IS NOT
   APPLIED ON A TYPE THAT DOES NOT DECLARE OR INHERIT IT) EXPLAINS BOTH #7393's
   DIRECT CASE (THE PROGRAM COMPILES) AND THIS BORROW CASE (THE §8.7 ERROR IS
   MASKED BY THE §28.6 RULE).** THE COORDINATOR POSTED THE MEASUREMENT AND THE
   RECOMMENDED ORDER (TYPE-LEVEL FIRST, THE §28.6 BORROW ERROR SECOND — THE
   COMPILER'S OWN CONVENTION, AS THE SYNC TWIN ALREADY DOES) ON **#7393**.
   §8.7 ALSO REQUIRES THE DIAGNOSTIC TO *"NAME THE DISCHARGE POINTS"*, AND THE
   ASYNC SPELLING'S OUTPUT NAMES NONE.
4. ~~**Whether the `Type` token belongs in the anchor's runtime-managed set.**~~
   #7338 asks for it; §28.9 lists `Type` explicitly, so the answer is yes and no
   train is needed — but §28.12's `await using` sentence does not say what
   happens to a `Type` head, so a ruling would make it explicit. Recorded here so
   the coordinator can decide whether it is worth a train.
   **⇒ STRUCK BY THE EXIT REVIEW's FINDING 8: **THIS QUESTION NAMES **#7338**, AND **#7338 IS
   CLAIMED BY THE ANCHOR's OWN COMMIT** (`e4a55c95 fix #3482 #7338 #7328 #5653 #3516 narrow #3483
   #3515`) — SO IT IS **NOT** HELD OUT, AND A QUESTION THAT NAMES AN ISSUE A LANE HAS TAKEN IS NOT
   A QUESTION FOR THE NEXT TRAIN. IT IS **ANSWERED BY THE ANCHOR** (§28.9 LISTS `Type` EXPLICITLY);
   THE RULING THE QUESTION SOUGHT — THAT §28.12's `await using` SENTENCE DOES NOT SAY WHAT HAPPENS
   TO A `Type` HEAD — REMAINS, AND IT IS A **SPEC-CLARIFICATION** CANDIDATE, NOT A LANE'S HOLD-OUT.
   **⇒ THE COORDINATOR's NOTE: A "QUESTION" THAT NAMES NO LANE'S HOLD-OUT AND IS ANSWERED BY A
   SHIPPED COMMIT IS A NOTE, NOT A TRAIN ITEM.****

## Siblings filed during the campaign

*(**13 of 13 rows are FILED.** The coordinator filed them with exactly one
`campaign:` label each, on 2026-10-28: new-1 → #7389, new-2 → #7390, new-3 →
#7391, new-4 → #7392, new-5 → #7393, new-6 → #7394, new-7 → #7395, new-8 →
#7396, new-9 → #7388, new-10 → #7387, new-11 → #7397, **new-12 → #7415**,
**new-13 → #7416**. The two the entry lane left explicitly to the coordinator
— new-12 and new-13 — were **re-measured by the coordinator independently**
before being filed, and the repros below are the GRID'S OWN CELLS, generated
from `scripts/using_heads_grid.py` so the repro is the campaign's and not a
paraphrase.)*

**=> AND THE **COORDINATOR** HAS FILED **THREE MORE** SINCE THE ENTRY LANE's THIRTEEN — **EACH WITH
EXACTLY ONE `campaign:using-heads` LABEL, EACH FROM A **REVIEWER's MEASUREMENT**, AND EACH WITH A
MINIMAL REPRO AND A §-CITATION IN ITS BODY:**

| # | the defect | the §-citation | where it came from |
|---|---|---|---|
| **#7422** | **THE `owned`-FIELD DISPOSAL PREDICATE COVERS THE ITERATOR-FORMAT HEADS IN A **CROSS**, NOT A SQUARE — `owned IEnumerator<int>?` AND `owned IAsyncEnumerable<int>?` **STILL LEAK THE FRAME**, AND THE ASYNC ONE **ALSO SKIPS THE §29.7 `finally` CHAIN** — **SO IT IS #2623's OWN DEFECT CLASS, ONE HEAD OVER** | §28.5 rule 1, §29.7, §30.7 (v0.345) | the U2/U3 lane's reviewer |
| **#7423** | **A `using var` OVER A **NULL-CONDITIONAL** `?.` READ OF A `Task` — **THE COMPILER CRASHES** (MALFORMED IR: A DEGRADED SLOT LOADED AS `i32` AND PASSED TO A POINTER PARAMETER) **AND WITH A NULLABLE RECEIVER THE PROGRAM **OVERFLOWS THE STACK** AT RUN TIME** | §30.11, §28.10, §30.2 (v0.345) | the U2/U3 lane's reviewer |
| **#7424** | **THE DISPOSAL-INTERFACE CHAIN TABLE OMITS `IOwningCollection<T>` AND `IOwningList<T>` (stdlib §4.2), WHICH EXTEND `IDisposable`, SO §28.12's DECLARATION-SITE BAN DOES NOT REACH THEM** — **AND IT IS NOT A COMPILER LIMITATION: THE COMPILER CAN SEE THE EDGE (`where T: IDisposable` ADMITS `IOwningCollection<Res>`)** | §28.12 (v0.345, spec:7475), stdlib §4.2 | the U4 lane's reviewer |
| **#7463** | **AN `owned` VALUE HELD IN A **LOCAL** SLOT TYPED AT `IOwningCollection<T>`/`IOwningList<T>` IS **NEVER DISPOSED** — THE DESTRUCTOR NEVER RUNS (ASan `11200 byte(s) leaked in 150 allocation(s)`), WHILE THE `IDisposable` TWIN DISPOSES CORRECTLY** — **AND IT IS **PRE-EXISTING** (THE REVIEWER MEASURED THE **IDENTICAL** NUMBERS ON THE BASE ARM) | §28.12, §28.10, §28.9, stdlib §4.2 (v0.345) | the **DISPOSAL-FAMILY** lane's measurement, CONFIRMED BY ITS REVIEWER |

**(#7422's ASYNC SELF-DISPOSAL RESIDUAL IS A **SECOND, DISTINCT** DEFECT AND IS RECORDED AS A COMMENT
ON #7422 ITSELF; **AND THE **SYNC** SELF-DISPOSE TWIN IS **NOT** A SECOND DEFECT — IT IS #7422's OWN
PREDICATE GAP WEARING THE SELF-DISPOSE COSTUME, **SO IT IS **NOT** FILED TWICE.**)

**=> AND THE COORDINATOR HAS FILED **TWO MORE** ON 2026-10-28, BOTH FROM THE **U4 FIX-FORWARD's
REVIEWER's** MEASUREMENTS, BOTH WITH EXACTLY ONE `campaign:using-heads` LABEL, AND **BOTH ARE THE
**SAME** MECHANICAL DERIVATION #7424's RESIDUAL NAMES** (ONE TABLE OF DISPOSAL HEADS, READ BY THREE
PLACES THAT CURRENTLY DISAGREE):

| # | the defect | the §-citation | where it came from |
|---|---|---|---|
| **#7456** | **THE §28.10 VACUITY LINT OVER-FIRES ON AN `IDisposable`/`IAsyncDisposable`/`IOwningCollection<T>`/`IOwningList<T>`-DECLARED BINDING WHOSE OBJECT **HAS** A DESTRUCTOR — THE WARNING CLAIMS "NO DISPOSAL OBLIGATION (§28.9)" WHILE THE SCOPE-END DISPOSAL **DOES** RUN. **THE FIVE SIBLING HEADS (`IEnumerator`, `IOwningEnumerator`, `IOwningEnumerable`, `IAsyncEnumerator`, `IOwningAsyncEnumerator`) DO **NOT** WARN — SO THE TWO TABLES DISAGREE ABOUT EXACTLY THE HEADS THE #7424 LANE TOUCHED** | §28.9, §28.10, §28.12, §8.7, stdlib §2.10, stdlib §4.2 (v0.345) | the U4 fix-forward's reviewer |
| **#7457** | **A `where` BOUND NAMING `IEnumerator<T>`/`IAsyncEnumerator<T>` **ADMITS A RECORD** TYPE ARGUMENT — THE BOUND CHECK NAMES **ONLY** `IDisposable` AND `IAsyncDisposable`, **A HAND-MAINTAINED TWO-HEAD LIST, THE SAME KIND OF DEFECT THE CHAIN-TABLE LANE FIXED — BUT IN A SECOND FILE: THE CHAIN TABLE WAS NEVER TWO-HEAD (IT HAD **TEN** HEADS, EIGHT BEFORE #7424's LANE)** | §9.7, §14.2 rule 3, stdlib §2.10 (v0.345) | the U4 fix-forward's reviewer |

| # | defect | minimal repro | §-citation | campaign |
|---|---|---|---|---|
| new-1 | a direct `DisposeAsync()` on a plain-`var` local leaks the receiver's block | `var x := new AsyncRes(); await x.DisposeAsync();` → `16 byte(s) leaked in 1 allocation(s)`; the `using var` spelling of the same program is clean | §28.12 step 2 (spec lines 7480 / 7482, **quoted**: *"The cascade and the chain are the same regardless of which logical-disposal path ran"* and *"Asynchronous cleanup runs only where the program wrote `await using` … or awaited a direct `DisposeAsync()` call"*), §28.2 (spec lines 6230 / 6244, **quoted**: *"`Dispose()` on any class instance is idempotent: the first call performs the disposal; subsequent calls are no-ops"* and *"**The compiler-inserted free.**"*) — **THE EARLIER WORDING OF BOTH WAS A PARAPHRASE, NOT A QUOTATION; THE HOUSE RULE IS THAT A QUOTED §-SENTENCE IS EVIDENCE AND A PARAPHRASE IS A CLAIM, SO THE SPEC's OWN WORDS ARE GIVEN HERE** | using-heads (U2d, with #1531) |
| new-2 | the `await using` spelling and the plain `using var` spelling disagree on the admission of an `Object`-declared borrow | `var s: Object := new Res(); await using var x: Object := s;` → rejected ("owned value in `s` would be dropped here without disposal"); the `using var` twin compiles | §28.12 ("`await using var` takes §28.10's admission unchanged"), §28.10, §28.3 | using-heads (U1b) |
| new-3 | the `await using` spelling of a value-like choice leaks the payload where the plain-`var` twin does not | `await using var x := VChoice.Note("a" + "b");` ×60 → `2100 byte(s) leaked in 120 allocation(s)`; the plain-`var` twin is clean; the plain `using var` spelling is clean | §28.10 ("the only effect is the reassignability restriction"), §28.12 | using-heads (U1c) |
| new-4 | an `owned IAsyncDisposable?` field's cascade does not free the stored instance | `public owned F: IAsyncDisposable? := null;` + `h.F := new AsyncRes();` → `16 byte(s) leaked in 1 allocation(s)` | §28.5 (the owned-field cascade), §28.12 (the run-time object's `DisposeAsync`) | using-heads (U2a) |
| new-5 | `DisposeAsync()` on an `Object`-typed value compiles | `var x: Object := o.ToString(); await x.DisposeAsync();` → compiles | §8.7 ("`DisposeAsync()` is a member only of a type that declares or inherits it") | using-heads (U2e) |
| new-6 | `record`/`choice` declaring `IAsyncDisposable` are admitted | `public record R: IAsyncDisposable … end R;` and `public choice C: IAsyncDisposable … end C;` → both compile | §28.12 ("a record, enum or choice that lists it … is a compile error at the declaration") | using-heads (U2e, with #7354) |
| new-7 | the `await using` spelling of a `Task` and of an `owned Task` field hang | `await using var x := tcs.Task;` and `h.F := tcs.Task` → both hang under AOT | §28.10, §30.5 | using-heads (U3c, with #6194) |
| new-8 | `await using var` outside an async method is admitted (the §30.2 twin is not) | `public static method Main() … await using var t := o.ToString();` → compiles | §28.12 ("`await using` is valid only inside an async method or async lambda") | using-heads (U1d, with #7326) |
| new-9 | the `using var` copy of a RUNTIME-MANAGED value (a `string`, a function value) leaks the copy's reference | `var src := o.ToString(); using var x := src;` ×60 → `2040 byte(s) leaked in 120 allocation(s)`; the plain-`var` twin is CLEAN; the `await using var` spelling of the same copy leaks too | §28.10 ("the disposal at scope end is a no-op ... and the only effect is the reassignability restriction") | using-heads (U1c, with #7316) |
| new-10 | the `using var` binding of an **INTERFACE-typed** parameter DISPOSES the borrowed parameter — the callee frees it and the CALLER then reads freed memory | `public static method Take(src: IAsyncDisposable) using var x := src; end Take;` called with `var src: IAsyncDisposable := new Res(); Take(src); src.Dispose();` → `DTOR` is printed by the CALLEE and the caller then reads freed memory: `ERROR: AddressSanitizer: heap-use-after-free … freed by thread T0 here: #1 … in bz$P.Program$Take` — the FREE is in `Program$Take`, the READ is in `Program$Main$Resume` | §28.10 ("When the right-hand side of a `using var` declaration is a borrow … the `using var` does **not** assume a disposal obligation … at scope end, no `Dispose()` is called, because the value is not owned by this scope"), §28.2 | using-heads (U1c — a NEW soundness defect; **#7387**, filed; the grid's `interface` / `copy_param_source` cell) |
| new-12 | the `using var` copy of an **OWNED** parameter disposes the block the callee's own binding still owns — the fault is entirely INSIDE the callee | `public static method Take(owned src: OChoice) using var x := src; end Take;` called with `Take(move src);` → `ERROR: AddressSanitizer: heap-use-after-free … freed by thread T0 here: #1 … in bz$G…Program$Take` and the READ is in the SAME frame (`Program$Take`) — the callee frees the block and then reads it. The caller is unaffected: it moved the value away and never touches it again | §28.10 (the `using var` copy of an owned binding must not dispose a block the enclosing binding still owns), §28.2 | using-heads (U1c — **#7415, FILED 2026-10-28 by the coordinator**; the grid's `owning_choice` / `copy_param_source` cell) |
| new-13 | the `using var` copy of a **BUILTIN-COLLECTION** parameter frees the collection's ARRAY block twice — a double free, both frees in the CALLER's frame | `public static method Take(src: Array<int>) using var x := src; end Take;` → `ERROR: AddressSanitizer: attempting double-free on … in bz_array_free` — and BOTH frees are in `Program$Main$Resume` (the collection is freed once by the `using var` copy and once by the caller's own binding) | §28.10 (the `using var` copy of a runtime-managed collection must not take a second disposal obligation on the same array block), §28.2 | using-heads (U1c — **#7416, FILED 2026-10-28 by the coordinator**; the grid's `builtin_collection` and `collection_literal` / `copy_param_source` cells) |
| new-11 | an `Object`-declared slot holding a runtime-managed string does not release it | `var s: Object := o.ToString();` → the plain-`var` twin leaks `2040 byte(s) in 120 allocation(s)`; the `await using var x: Object := o.ToString();` spelling (the corpus's `s28_12_0026`) does not | §28.9 ("the type's lifetime is managed by the runtime without user involvement"), §28.10 | using-heads (the `Object`-slot family; the grid's `object_string` twin) |

## Lane records

*(one block per handback; append, do not rewrite)*

### Entry lane `r36-c1-entry` — 2026-10-28

* **SHA(s):** one commit — this record plus `scripts/using_heads_grid.py` — on
  branch `r36-c1-entry`. The commit is the branch tip; its SHA moves on a
  rebase or an amend, so read it from `git log -1 r36-c1-entry` (the pick
  ledger records the picked SHA). Base: `origin/main` `9be24022` ("Research:
  the ownership model grown to Layer 1"); the measurements were taken on the
  tree's `d9f11d15` ("Plan §4.8: free-at-Dispose must not leave a later flag
  read"), and `git diff --name-only d9f11d15 origin/main` names **no `src/`
  file** (only `master-plan.md` and `docs/research/`), so the compiler under
  test is the same on both.
* **Rows re-measured:** all 24 (§ "Members at entry"), MEASURED evidence quoted
  per row. The membership has since changed: #7327 and #7337 are CLOSED and
  #7374 is new (see "Members at entry").
* **Grid result:** `153 cells: 98 ok, 41 wrong, 5 known-red, 0 undecided, 9
  n/a, 0 skipped` and `NATIVE SANITIZER: 0 check(s) skipped`. The instrument is
  new, so there is no fail-without to prove for the grid itself; the 41 wrong
  cells ARE the fail-without for the members they carry.
* **Fail-without / fail-with for the grid's new cells:** the #7316 fix
  (`b6053a68`) is the one commit that moves a cell, so it is the proof — a
  second worktree was built at that commit and the same grid was run against
  its compiler. The table is in "DONE instrument".
* **Fix-forward:** none — this lane fixes nothing (master plan §2.2 is a
  read-mostly entry check).
* **New instruments:** `scripts/using_heads_grid.py` (the DONE grid; the §2.5
  record update rides in the same commit as the pick that changes it).
* **Not touched:** no normative document (`docs/Bezel_*.md`), no
  `docs/proposals/papers/master-plan.md` (its §6 progress table is the
  coordinator's), and no `tests/conformance/rules/baseline.tsv` row.

### Fix-forward lane `r36-c1-entry` — 2026-10-28 (round 1)

* **SHA(s):** one commit on top of the entry commit `2381f534`, on branch
  `r36-c1-entry`. The entry commit's non-author review returned **ACCEPT-W-NOTES
  on the RECORD** and **"the grid must NOT land as the campaign's DONE
  instrument yet"** — three HIGH defects (H1 the nine `copy_await_source`
  cells are green on §28.3's must-use rather than on §28.10's rule; H2 the
  grid cannot detect §28.12's "`await using var` takes §28.10's admission
  unchanged"; H3 the grid's verdict is not reproducible on ONE build), four
  MEDIUM (M1 the KNOWN-RED demotion was unearned; M2 the grid was wired into
  no gate; M3 the report TSV was malformed; M4 the TSV embedded an absolute
  path) and ten LOW. This lane is that fix-forward.
* **Files changed:** `scripts/using_heads_grid.py` (the instrument),
  `scripts/gate/tests/test_using_heads_grid.py` (NEW — 19 tests; the grid's
  CLASSIFIER is now at every gate, via `scripts/pregate.sh`'s
  `python3 -m unittest discover -s scripts/gate/tests`), and this record.
* **No `src/` change** — the compiler is untouched, so the base and the tip
  measure the same compiler and the "fail-without" is the compiler's, not the
  script's.
* **The H2 decision (the one the brief left to the lane).** The grid
  **COMPARES** the two spellings instead of copying an expectation: each
  head's `using var` / `await using var` pair and its `copy_local_source` /
  `copy_await_source` pair must agree on the ADMISSION, and a disagreement is
  its own verdict (`MISMATCH`), which blocks the DONE verdict like a wrong
  cell. (The alternative — an extra "the two spellings must agree" cell —
  would have added 17 more cells and a second set of expectations to keep in
  step; the comparison reuses the cells that already exist and needs no
  second authority.) The `object_class` head is EXCLUDED from the comparison:
  §28.12's "unchanged" against §28.10's adoption rule over an `Object`-declared
  binding is the OWNER's open question, and the grid must not settle it by
  counting a cell wrong. The record's open question and the held-out hunk are
  unchanged.
* **The needle audit (H1) — every REJ cell's needle, old → new.** The old
  needles were `"using var"` (the §28.3 must-use diagnostic's REMEDY, not a
  rule), `"no member `Dispose`"`, `"no member `DisposeAsync`"`, `"is
  vacuous"`, `"reflection is not yet implemented"` and `"(using var)"` — all
  implementation wordings, and each of them a token that a NEIGHBOURING rule's
  diagnostic can contain. Every one is now a `§` marker naming the rule the
  cell decides, and the needle is matched against the diagnostic that
  **rejected** the program (a warning that cites the same section no longer
  makes a cell green). The complete list is in the commit body.
* **Measured:** see the baseline, the two-run determinism proof and the
  fail-without/fail-with table above; the DONE criterion is **no WRONG, no
  KNOWN-RED, no MISMATCH**.
* **The reviewer's perturbation, re-run after the changes:** two currently-OK
  cells of the `value_choice` head were flipped in a SCRATCH COPY of the
  script (`using_var`'s expected markers and `explicit_dispose`'s needle). The
  grid reported **both as WRONG** — *"output [] != expected
  ['PERTURBED_MARKER', …]"* and *"rejected, but no '§28.10' in the
  diagnostic"* — and the head's count went from `3 ok, 3 wrong, 2 known-red`
  to `1 ok, 5 wrong, 2 known-red`, exit 1. The grid is not a no-op.
* **Not touched:** no normative document (`docs/Bezel_*.md`), no
  `docs/proposals/papers/master-plan.md` (its §6 progress table is the
  coordinator's), no `tests/conformance/rules/baseline.tsv` row, and no issue
  was filed (the two new findings below are for the coordinator to file).

### Grid-discharge lane `r36-c1-gridfix` — 2026-10-28 (rounds 2, 3 and 4)

* **SHA(s):** one commit on branch `r36-c1-gridfix`, on top of the fix-forward
  `3cc672c1` (branch `r36-c1-entry`, untouched by this lane). The commit is the
  branch tip; its SHA moves on a rebase or an amend, so read it from
  `git log -1 r36-c1-gridfix` (the pick ledger records the picked SHA). Base:
  `3cc672c1`.
* **Files changed:** `scripts/using_heads_grid.py` (the instrument),
  `scripts/gate/tests/test_using_heads_grid.py` (the gate's classifier test:
  **19 → 30 tests**), and this record. **No `src/` change** — the compiler is
  untouched, so the base and the tip measure the same compiler. **No issue is
  closed by this commit.**
* **What this lane was for:** the non-author review of `3cc672c1` ruled
  ACCEPT-W-NOTES with **three BLOCKING notes**, and this lane discharges them.
  (D1) a COMPILE `TIMEOUT` was neither blocking nor honestly labelled; (D2)
  the gate test did not guard the load-bearing fixes — four of the reviewer's
  ten mutations SURVIVED; (D3) the record named three CLOSED issues as OPEN
  and misattributed one fixing commit. **A second review of this lane's own
  commit was a narrow REJECT** (D1 and D3 discharged, D2 NOT): four MORE
  mutations survived, and the commit body over-claimed what its mutation
  check had actually killed. **Round 3 (this commit)** adds the four missing
  tests, strengthens the two MISMATCH tests, and states — mutation by
  mutation — which test kills which mutation.
* **D1 — the timeout is a VERDICT, it blocks, and it is not a sanitizer
  skip.** `judge` now counts `TIMEOUT` in the blocking set (a cell whose
  program hung the compiler is UNMEASURED, and a DONE instrument must not
  print YES with an unmeasured cell); the `SKIPPED` verdict is GONE (no code
  path produced it); the cells line reports the timeout count as
  **`0 timeout`** (it used to say "skipped", and the sanitizer line carried
  the SAME number, so the claim on the screen was false); and the sanitizer
  line now has **its own counter** — `asan_skipped_count` measures, per cell,
  whether the executable that was built carries the ASan runtime, and reports
  the cells whose ASan check was skipped. The module docstring, the summary
  and this record now say the same thing. **MEASURED on this tree: `NATIVE
  SANITIZER: 0 check(s) skipped`** — the ASan runtime was available for every
  cell that produced a binary.
* **D2 — the gate test now guards the load-bearing behaviours.** Round 2's
  tests are `test_the_classifier_pins_every_verdict` (the LEAK / CRASH /
  CLEAN / CF verdicts), `test_a_warning_citing_the_section_does_not_make_a_
  rej_cell_green` (the grid's own REJ branch, driven with the `warn_only`
  fake compiler: a §28.10 WARNING beside an unrelated §28.3 ERROR),
  `test_a_mismatch_in_the_run_family_blocks_the_done_verdict` (the agreement
  check in the RUN family, not only the REJ family),
  `test_a_timeout_cell_blocks_the_done_verdict`,
  `test_the_sanitizer_counter_is_its_own_counter` and
  `test_the_summary_names_the_timeout_and_keeps_the_sanitizer_separate`. The
  `warn_only` fixture — which no test used — now emits exactly the pair the
  reviewer described. **Round 3 (the second review's four survivors) adds
  five**: `test_the_exit_status_matches_the_printed_verdict` (the exit status
  is the ONLY thing a caller sees: a grid that prints "DONE: NO" and exits 0
  reports a red campaign as a pass),
  `test_a_stale_executable_is_removed_before_the_compile` (a stale binary in
  the `--out` directory must not be counted as a cell's sanitizer check),
  `test_the_vacuity_rule_is_driven_by_the_grid_own_branch` (the §28.9 VAC /
  NOVAC rule, which governs 36 of the 153 cells, driven through the grid's
  OWN branch with the `vacuous` fake),
  `test_a_correct_section_rejection_makes_the_cell_green` (the REJ branch's
  POSITIVE direction, driven with the `reject_2810` fake) and
  `test_a_mismatch_blocks_the_done_verdict_on_its_own` (the MISMATCH as the
  ONLY blocker: the fixture of the pre-existing MISMATCH test has BOTH cells
  WRONG, so its WRONG blocks independently of the MISMATCH). **Both dead
  fixtures are now live.** The 19 pre-existing test methods are AST-identical
  to the base (`3cc672c1`); the stronger MISMATCH fixture is a NEW test, not
  an edit of the old one. **The lane's own mutation check: NINE load-bearing
  behaviours broken ONE AT A TIME each turn the gate test RED** (the nine RED
  results, and the test each one kills, are in the commit body).
* **D3 — the record's issue states and numbers are now the tracker's.** #7283,
  #7306 and #7322 are **CLOSED** (#7306 at 2026-10-27T14:43:15Z, #7283 at
  14:43:18Z and #7322 at 14:43:21Z — ~8 minutes BEFORE the commit that called
  them open) and the record says so, with the
  fixing commits the issues' own closing comments name: **#7283 → `87fdfa67`**
  (the function-typed shape), **#7306 → `9bb9387f`**, **#7322 → `9bb9387f`**
  for its own part plus `87fdfa67` for the two shapes it was split into. The
  label query's number is now the one true at its stated date (**34** at this
  commit's timestamp: 19 members + #7374 + 12 + 2) with the **live** count
  (**40** on 2026-10-28) quoted separately. The quarantined split is labelled
  **UNSTABLE** and is no longer quoted as a point value. The `e9c564fd`
  citations are gone from the two places that carried them — the record's own
  L4 answer says that commit cannot be named from this tree's history, so the
  record must not cite it either.
* **Measured on this tree** (two runs, into two `--out` directories):
  `153 cells: 82 ok, 43 wrong, 2 known-red, 8 mismatch, 0 undecided, 9 n/a, 0
  timeout` · `NATIVE SANITIZER: 0 check(s) skipped` · `DONE: NO — 53 cell(s)
  block it (wrong + known-red + mismatch + timeout)` — identical in both runs,
  and the two report TSVs are byte-identical on all 144 non-quarantined rows.
  The quarantined row's split differed between the two runs (5 OK/4 WRONG and
  6 OK/3 WRONG), which is exactly why it is no longer quoted as a point value.
  **A third run (round 3, into a third `--out` directory) gives the same DONE
  number**: `153 cells: 82 ok, 43 wrong, 2 known-red, 8 mismatch, 0 undecided,
  9 n/a, 0 timeout` · `NATIVE SANITIZER: 0 check(s) skipped` · `DONE: NO — 53
  cell(s) block it (wrong + known-red + mismatch + timeout)`.
* **Also corrected here, of the same class:** the twins. The record said "every
  head's twin is CLEAN except `object_string`"; MEASURED, `generic_unconstrained`'s
  twin leaks 32B/1 and `type_token`'s twin does not build at all (CF — the
  `typeof` deferral, §17.10). Both are now named.
* **Not touched:** no normative document (`docs/Bezel_*.md`), no
  `docs/proposals/papers/master-plan.md`, no `tests/conformance/rules/baseline.tsv`
  row, no `src/` file, and no issue was filed or closed.

### Anchor lane `r36-c1-anchor` (the U1c ANCHOR) -- 2026-10-28

* **SHA(s):** one commit -- `eb84aca4`, PICKED as `e4a55c95` on the staging
  branch `r30-c1`. Subject: *fix #3482 #7338 #7328 #5653 #3516 narrow #3483
  #3515: one shared using-heads disposal anchor -- the owned-interface return
  and the per-level reclamation chain stay open*. Six files, +1146/-377.
* **WHAT IT IS:** ONE shared authority -- `common::classifyDisposal`
  (`src/common/DisposalKind.{hpp,cpp}`) -- that replaces THREE near-answering
  predicates (the codegen lambdas `typeMayHoldClass` /
  `awaitUsingSlotMayHoldClass`, `MethodOwnership::rhsIsDisposableClassType`,
  and the by-name-only `Emitter::usingVarBuiltinHeadName`) and answers as a
  **KIND** -- NONE / CLASS-SYNC / CLASS-ASYNC / VALUE-LIKE PAYLOAD / MOVE-ONLY
  -- rather than a boolean. The key is the SLOT'S REPRESENTATION, not the
  binding's declared type (spec §28.12: "`await using` selects the
  logical-disposal path by the class of the object the binding **holds** ...
  never by the binding's static type").
* **THE TWO `narrow`s NAME THEIR OPEN FAMILIES:** #3483's is an `owned
  IAsyncDisposable` RETURN (it needs the #3493 ruling's itab route) and
  #3515's is the PER-LEVEL reclamation chain
  (`Emitter::emitBaseDisposeChain`, a file the lane does not own).
* **THE REVIEW (non-author, ACCEPT-W-NOTES) RAISED ONE GATE-BLOCKING
  MUST-FIX:** the corpus row `s28_12_0007` is left UNBANKED (the fix moves it
  from banked `fail` to observed `pass`), and `s28_12_0020` / `s28_12_0021`
  have NO baseline row at all. **THE COORDINATOR RE-BANKS, ON THE FINAL TIP
  OF THE BATCH -- NEVER THE LANE.**
* **AND THE REVIEW CORRECTED THE COORDINATOR's OWN EARLIER CLAIM ABOUT
  #7339 (the retraction is posted on that issue):** the leak **DOES** reproduce
  on BOTH arms -- **58 B in 3 allocations** -- and **IDENTICALLY** for the
  `const` and the `using var` twin, so the `await using` BINDING IS NOT THE
  CAUSE. (The issue records 34 B; the numbers differ, the shape does not.) The
  anchor neither fixes nor worsens it, and **#7339 STAYS OPEN ON ITS OWN
  TERMS** (runtime dispatch on the slot's value).
* **THE U1c "COPIES" LANE IS DISPATCHED ON THIS TIP** (4 members: #7387
  #7388 #7374 #7357 -- a `using var` / `await using var` binding that takes a
  COPY, and the copy's own reference is never armed, never released, or is a
  BORROWED value disposed by the callee). #7339 IS HELD OUT FOR THE OWNER.

### U1c COPIES lane `r36-c1-u1c` — 2026-10-28 (the record was MISSING this block — the exit
### review's FINDING 2; master plan §2.5 wants one block per handback, and this lane's three picks
### carried NO record update)

**COMMITS (picked onto staging):** `6e31b592` fix #7387, `1a8bf0ae` fix #7388, `c5e8992d` fix #7374.
**REVIEWER: ACCEPT-W-NOTES.**

* **#7387 — a `using var` binding of a BORROWED PARAMETER DISPOSED THE CALLEE's BORROW — THE CALLER
  THEN READ FREED MEMORY.** The lane's reviewer measured that this fix ALSO REMOVED A **DOUBLE
  FREE**: `builtin_collection/copy_param_source` AND `collection_literal/copy_param_source` BOTH
  CRASHED WITH *"ERROR: AddressSanitizer: attempting double-free"* ON THE BASE AND NOW EXIT 0 WITH
  NO REPORT. **THE COORDINATOR RE-MEASURED #7416's OWN REPRO INDEPENDENTLY AND CLOSED #7416.**
* **#7388 — the `using var` COPY of a RUNTIME-MANAGED VALUE LEAKED THE COPY's OWN REFERENCE** — THE
  PLAIN-`var` TWIN WAS CLEAN. (THIS IS THE FIX THAT MAKES THE `LlvmBackend_Setup.cpp` COMMENT ABOUT
  THE "`using var` COPY … get NO retain" **FALSE**; THE U2/U3 LANE CORRECTED THAT COMMENT.)
* **#7374 — the `await using var` COPY of a VALUE-LIKE CHOICE LOCAL WAS NEVER ARMED** — THE AWAITED
  SPELLING OF #7316's SHAPE.
* **THE GRID CELLS THIS LANE MOVED **WRONG → OK** (MEASURED BY THE EXIT REVIEW WITH THIS RECORD's
  OWN INSTRUMENT, SIX CELLS): `interface/copy_param_source`, `string/copy_local_source`,
  `string/copy_await_source`, `string/copy_const_source`, `value_choice/await_using_var`,
  `value_choice/copy_await_source`. **THE EXIT REVIEW's SPLIT IS DIFFERENT FROM PLAN §6's ACCOUNT:
  THE **ANCHOR** MOVED 8 (53 → 45) AND **THIS** LANE MOVED 6 (45 → 39).**
* **#7357 IS HELD OUT: THE LEAK IS IN THE SOURCE's **PRODUCER +1** AND IS SYMMETRIC (A CALLEE THAT
  IGNORES ITS PARAMETER LEAKS IDENTICALLY), SO IT IS NOT A COPY DEFECT.**
* **THE `task` GRID ROW IS **QUARANTINED** — #6194's FLAKY HANG (2/20 ON THE BASE, 1/20 ON THE TIP)
  — SO THE CAMPAIGN'S DONE INSTRUMENT **STILL HAS A FLAKY ROW** AND THE CAMPAIGN IS NOT DONE. **THE
  U2/U3 LANE's FIX (BELOW) REMOVES THE DOUBLE FREE ENTIRELY, SO IT **MAY UN-QUARANTINE** THE ROW —
  BUT THAT FIX IS **NOT PICKED** YET.**

### U2/U3 lane `r36-c1-u2u3` — 2026-10-28 (the record was MISSING this block too — the exit review's
### FINDING 2)

**COMMITS: `4823c418` fix #2623, `fc09c0ca` fix #6194, `cdf348ec` (a COMMENT-ONLY COMMIT, NAMES NO
ISSUE ON PURPOSE). **=> PICKED (2026-10-28): `cdf348ec` -> `e239820b2c`, `fc09c0ca` -> `43c436c8ea`,
`4823c418` -> `ad6911e940` — THREE COMMITS, **ZERO CONFLICTS**, REVIEW **ACCEPT-W-NOTES**
(`a75df12462f980988`).** **THE TWO ISSUES CLOSE ON PUSH** (THE LEDGER READS THE SUBJECT LINE ONLY).

* **#2623 — AN `owned` FIELD HOLDING AN **ASYNC ITERATOR** WAS NOT DISPOSED WHEN ITS OWNER WAS.**
  §28.5 (the owned-field cascade) + §29.7 (a partially-consumed iterator's `Dispose` runs the
  pending `finally` and frees the frame) + §30.7 (the async iterator frame's vtable is
  `[0]=td,[1]=Dispose,[2]=MoveNextAsync,[3]=Current,[4]=GetAsyncEnumerator` — the **same slot-1
  Dispose** the sync format has). **THE MECHANISM: `Emitter::isOwnedIteratorInterfaceField`
  RECOGNISED THE **SYNCHRONOUS** ITERATOR-FORMAT HEADS ONLY, SO AN `owned IAsyncEnumerator<T>` FIELD
  MATCHED **NO ARM AT ALL** AND WAS SKIPPED BY THE CASCADE.** THE LANE WIDENED THE CLASSIFIER
  (VIA `common::isIterFormatEnumeratorHead` MINUS `common::isSyncIterFormatEnumeratorHead` — THE
  SYNC `isVtableBackedIEnumerableType` IS DELIBERATELY **NOT** WIDENED, IT DRIVES THE §29.2 `for`
  DESUGARING) AND THE **EXISTING** slot-1 DISPATCH FREES THE FRAME.
* **#6194 — THE `using var` / `await using var` OF A STORED-TASK SOURCE's `Task` PROPERTY **HUNG**
  UNDER AOT.** §30.11 (v0.350): *"The **source owns the task object** its `Task` property exposes. A
  read of the property is a **borrow** of the task … there is no first-read ownership transfer"*;
  *"the source's disposal is the task's single disposal."* **THE DEFECT: THE `using var` SPELLING
  REGISTERED A SCOPE-END DISPOSAL (THE PLAIN-`var` TWIN DOES NOT), THAT DISPOSAL FIRE-AND-FORGOT AND
  **FREED** THE TCS's INNER TASK, THE TCS's OWN SCOPE-END RELEASE THEN READ THE **SAME** INNER TASK
  AGAIN (`bz_tcs_task` → `bz_task_is_error`) — A USE-AFTER-FREE — AND THE FIRST LOCK ON THE FREED
  TASK's `std::mutex` BLOCKS FOREVER, SO THE PROGRAM **HANGS**; UNDER ASAN THE PENDING-TASK VARIANT
  IS A **SEGV**.** THE FIX IS AT `Emitter::registerUsingDisposal` (THE PIN DECISION), AT
  `Emitter::taskLocalOwnsFrame` (THE OWNS-THE-FRAME AUTHORITY) AND AT A NEW
  `Emitter::usingVarRhsIsStoredTaskPropertyBorrow`.
* **THE COMMENT-ONLY COMMIT CORRECTED A STALE COMMENT IN `src/codegen/LlvmBackend_Setup.cpp`** — THE
  ONE THAT SAID A `using var` COPY, A `string?`-DECLARED COPY AND AN `object`-DECLARED COPY ARE
  "all NOT in the set, get NO retain". **IT IS SPLIT INTO THE PART THAT IS STILL TRUE AND THE PARTS
  THAT ARE NOT, EACH NAMING THE FIX THAT CHANGED IT** — **AND THE COORDINATOR's OWN PREMISE WAS
  CORRECTED BY THE LANE: THE `object`-DECLARED HALF's *MEMBERSHIP* CLAIM IS TRUE, BUT ITS
  *OPERATIVE* CLAIM ("get NO retain, AND SO MUST KEEP TAINTING THE SOURCE") IS **FALSE** SINCE
  #2900 — IT RETAINS AND DOES NOT TAINT THE SOURCE.**
* **FIVE ISSUES THE LANE MEASURED AND **DID NOT** CLOSE: #1531, #1603 (a direct `DisposeAsync()`
  needs the §28.12 step-2 **physical reclamation** ACROSS A SUSPEND POINT — a multi-hour change in
  the state machine, and the abandoned-task variant needs A `bz_task_then` CONTINUATION WITH A
  PER-CLASS RECLAIM TRAMPOLINE ON THE GENERIC `CallExpr` PATH), #3471 (the DUPLICATION IS REAL AND
  THE LANE RE-COUNTED IT — `dictTypeOfLocal` 2, `linkedListTypeOfLocal` 2, `listInterfaceTypeOfLocal`
  2, `collectionTypeOfLocal` 2, `taskTypeOfLocal` 1 — BUT THE REPAIR REWIRES THE WHOLE
  BUILTIN-COLLECTION DISPOSAL FAMILY AND THE ISSUE ITSELF DEMANDS A GRID THE COORDINATOR OWNS),
  #7326 and #7354-shape-3 (BOTH **SEMA** SHAPES — **THE NEXT LANE's**).
* **TWO NEW DEFECTS, NOW **ISSUES** (THE TRACKER's NEW-ISSUE CREATE **WORKS** AGAIN, SO THEY ARE
  NO LONGER COMMENTS): **#7422** — THE `owned`-FIELD DISPOSAL PREDICATE COVERS THE ITERATOR-FORMAT
  HEADS IN A **CROSS**, NOT A SQUARE, **SO `owned IEnumerator<int>?` AND `owned
  IAsyncEnumerable<int>?` STILL LEAK THE FRAME, AND THE ASYNC ONE **ALSO SKIPS THE §29.7 `finally`
  CHAIN** (§28.5 rule 1, §29.7, §30.7) — **THE DIRECT SIBLING OF #2623's OWN FIX, ONE HEAD OVER**;
  AND **#7423** — A `using var` OVER A **NULL-CONDITIONAL** `?.` READ OF A `Task` **CRASHES THE
  COMPILER** WITH MALFORMED IR (A DEGRADED SLOT LOADED AS `i32` AND PASSED TO A POINTER PARAMETER)
  **AND, WITH A NULLABLE RECEIVER, **OVERFLOWS THE STACK** AT RUN TIME (§30.11, §28.10, §30.2) —
  **WHICH IS **NOT MERELY UNFIXED** BUT MEMORY-UNSAFE IN TWO DISTINCT WAYS.** **=> BOTH WERE
  MEASURED BY THE LANE's REVIEWER, NOT QUOTED FROM THE LANE's WORD.**

* **THE REVIEWER's MEASUREMENTS THAT CORRECT THE RECORD (2026-10-28):**
  * **THE #2623 LEAK REPRO IS **NOT** VACUOUS, AND ITS SLOPE IS THE NUMBER: **A ONE-SHOT MEASURES
    72 B, AND A **LOOPED** REPRO MEASURES **72 B/ITER EXACTLY** (3600 B / 50 ALLOCS AND 14400 B /
    200 ALLOCS) — **LINEAR, AND **FLAT (ZERO) ON THE FIX ARM.**** **=> THE LANE REPORTED ITERATION 1
    INSTEAD OF THE SLOPE; **THE FIX IS SOUND AND THE ORACLE IS SOUND**, AND **THE LOOPED FORM IS
    THE ONE TO KEEP.**
  * **THE `owning_tcs` ROW's BASE FAILURE IS **INTERMITTENT**, NOT A THIRD TIMEOUT** — **OVER 14
    BASE RUNS IT PASSED 3× AND FAILED 11× — **AND THE COMMIT BODY's CHARACTERISATION OF THE BASE's
    THIRD FAILURE AS A THIRD TIMEOUT IS THEREFORE WRONG.** **=> AND **THE BASE ARM MUST BE MEASURED
    **PER ROW**, BECAUSE THE HANG SHORT-CIRCUITS THE AGGREGATE FILTER** (*"ran 0 of 5 matching tests
    — the run was CUT SHORT …" **— THE HARNESS LINE CONTINUES "(crash/abort); not
    trustworthy."**, SO THE QUOTATION IS **TRUNCATED**"*).
  * **THE #1531 / #1603 **FILE ATTRIBUTION** IN THE LANE's OWN COMMIT BODY IS **IMPRECISE**: THE
    `DisposeAsync` LOWERING LIVES IN `src/codegen/LlvmBackend_Async.cpp` (`emitAsyncDisposeAwaitAndFree`,
    ~6175) AND `LlvmBackend_Disposal.cpp`, AND THE GENERIC `CallExpr` PATH IS
    `LlvmBackend_Calls2.cpp` (`lowerCall` ~8740, `lowerCallDispatch` ~8833) — **NEITHER
    `LlvmBackend_Exprs.cpp` NOR `LlvmBackend_Calls.cpp` CONTAINS `DisposeAsync` LOWERING CODE.** THE
    CONCLUSION ("NOT THIS LANE's") HOLDS; **ONLY THE FILE NAMES ARE WRONG.**

### Fix-forward lane `r36-c1-u4` — 2026-10-28 (the U4 FIX-FORWARD — **PICKED**, TWO CLOSURES, THREE NARROWED)

* **THE LANE's SIX COMMITS, `r36-c1-u4`, base `b19567b1`, reviewed by `abe853d95982e5a07`
  (**ACCEPT-W-NOTES**). **THE PICKED SHAs ON STAGING: `a37947ec` (`narrow #7326 fix #7396`) ·
  `dc0fd710` (`narrow #7354 fix #7394`) · `70e7d059` (`narrow #7424`) · `5794390c`
  (`narrow #7354`) · `f5d9ef22` (`narrow #7326`) · `431fed95` (`narrow #7424`).**
* **=> TWO **TRUE** CLOSURES: **#7394** AND **#7396**. **NO ISSUE IN THIS FAMILY REMAINS OPEN **AS A DEFECT**. **=> **CORRECTION (THE COORDINATOR, 2026-10-28 22:40): **#7326 IS **NO LONGER PARTIAL** — ITS RESIDUAL (THE §28.10 VACUITY LINT's OVER-FIRE, **#7456**) IS **FIXED** BY THE DISPOSAL-FAMILY LANE (`19fd5710`), WHICH IS AN ANCESTOR OF STAGING — **SO #7326 IS **WHOLE**, AND **TWO** ISSUES (NOT THREE) REMAIN OPEN.** **=> **CORRECTION (THE COORDINATOR, 2026-10-28 23:45): **#7354 IS **NOW WHOLE** AS WELL — ITS RESIDUAL (THE **NESTED-RECORD REACH GAP**: `Resolver::checkRecordRestrictions` ITERATING `ctx_.module()->children` — **MODULE-LEVEL ONLY**) IS **THE **VERY** DEFECT #7490's LANE FIXED** (`0c562e95`, PICKED AS `32ca5cf3f1`): THE CHECK NOW WALKS THE MODULE's OWN DECLARATION TREE WITH `forEachTypeAndNested` (#3574, §11.6), SO A `record` **INSIDE A `class`** NO LONGER ESCAPES ANY §9.6/§9.7 RECORD RESTRICTION — AND THE REACH IS **RECURSIVE** (A `record` AT DEPTH TWO IS REACHED TOO).  **=> **ONE** ISSUE (NOT TWO) REMAINS OPEN: **#7424** (THE CHAIN TABLE's MECHANICAL DERIVATION).** **=> **CORRECTION (THE COORDINATOR, 2026-10-29 00:20): **#7424 IS **NOW WHOLE** AS WELL — ITS RESIDUAL (THE **MECHANICAL DERIVATION** OF THE DISPOSAL-INTERFACE CHAIN TABLE INTO `src/common/BuiltinRegistry.*`) IS **NOW MECHANICALLY GUARANTEED** BY THE CHAIN-TABLE LANE's COMMIT `251e575d`: THE NEW RATCHET `scripts/check_disposal_family_chain.py` **PARSES THE STDLIB DOCUMENT**, **TAKES THE TRANSITIVE CLOSURE** OF THE EXTENSION GRAPH FROM `IDisposable`, AND **COMPARES THE DERIVATION AGAINST THREE TRANSCRIPTIONS** OF THE SAME FACT (THE MACHINE-READABLE CODE TABLE, THE `std::array<..., N>` SIZE, AND THE HAND-WRITTEN COMMENT BLOCK) — IT **FAILS CLOSED**, AND IT IS WIRED INTO `scripts/gate_full_cycle.sh`, SO THE GATE's RATCHET SWEEP **DISCOVERS** IT.  **=> **NO** ISSUE REMAINS OPEN IN THIS FAMILY: **#7354** AND **#7424** ARE BOTH **WHOLE**.**

**THE COORDINATOR's SUBJECT AMENDMENT (2026-10-28) — THE REVIEWER's FIRST NOTE, DISCHARGED.**

**THE PROBLEM MEASURED BY THE REVIEWER: `scripts/pick_issues.sh` READS THE **SUBJECT LINE ONLY**, AND
THE LEDGER's `--infer-kind` HEURISTIC CLASSIFIES `narrow` AS `fix` — **SO THE TOOLING CANNOT
DISTINGUISH A FULL CLOSE FROM A PARTIAL ONE. THE ONLY THING THAT KEEPS `narrow` FROM CLOSING IS THE
HUMAN's READING OF THE WORD.** **=> A `fix #N` IN A SUBJECT THAT A LATER COMMIT IN THE SAME BRANCH
CALLS `narrow #N` IS A **FALSE CLOSURE** — AND IT IS THE **WORSE** OF THE TWO ERRORS.**

* **THE REVIEWER's OWN LATER COMMITS SAID `narrow #7354`, `narrow #7326` AND `narrow #7424` — SO THE
  EARLIER `fix` SUBJECTS OVERCLAIMED AGAINST THE LANE's OWN WORD.**
* **=> THE COORDINATOR AMENDED THE **THREE** `fix` SUBJECTS **BY REBASE** (A MESSAGE AMENDMENT — **THE
  PATCHES ARE BYTE-IDENTICAL: ALL SIX `git patch-id --stable` VALUES ARE UNCHANGED**, AND
  `check_commit_trailers.py --range b19567b1..r36-c1-u4` IS **CLEAN (6 commit(s))**). THE THREE
  NEW SUBJECTS ARE:**
  * `narrow #7326 fix #7396: an `await using` outside an async method or async lambda is not rejected (§28.12, v0.345)`
  * `narrow #7354 fix #7394: the disposal-interface declaration-site rejection follows the INTERFACE CHAIN, not a name (§28.12, §9.7, §8.7, v0.345)`
  * `narrow #7424: the built-in disposal chain omitted `IOwningCollection<T>` and `IOwningList<T>`, so a record listing either was admitted (§28.12, v0.345)`
* **=> AND **EACH AMENDED BODY NAMES THE RESIDUAL THAT STAYS OPEN** (MASTER PLAN §4.2's RULE), SO THE
  RECORD IS IN THE COMMIT ITSELF AND NOT ONLY IN THIS BLOCK.
* **=> THE COORDINATOR PICKED ALL SIX WITH `--kind fix`, **THE HOUSE PRACTICE FOR `narrow`
  COMMITS** (SEE `afc0147d1a` `narrow #7316`, `cc7d985553` `narrow #7333`, `b19567b1` `narrow
  #7232`) — **THE REVIEWER HAD ASKED FOR THREE OF THEM TO BE `--kind test`; THE COORDINATOR DEVIATED,
  AND THE REASON IS MEASURED: `5e357660` **REMOVES A REJECTION** (A SHADOWING USER `IDisposable` IS
  NOW ADMITTED WHERE IT WAS REFUSED — THE REVIEWER's F6) AND `428a2dd7` ADDS A CHAIN ROW, SO **NEITHER
  IS TEST-ONLY**, AND **THE LEDGER CREDITS BY SUBJECT, NOT BY KIND** — SO A `--kind test` ON THEM
  WOULD NOT HAVE CHANGED THE CREDITED SET ANYWAY.**

**THE POST-PICK STATE OF THE FIVE ISSUES (THIS IS THE AUTHORITATIVE STATUS; THE "Members at entry"
TABLE ABOVE IS THE **ENTRY** MEASUREMENT, DATED `d9f11d15`, AND IS **NOT** REWRITTEN — A PICK DOES NOT
RE-MEASURE THE ENTRY CHECK).**

| # | post-pick state | what stays open |
|---|---|---|
| **#7396** | **CLOSED** — an `await using` outside an async method or async lambda is now **REFUSED** (§28.12), with the diagnostic at a real location | — |
| **#7394** | **CLOSED** — a `record`/`enum`/`choice` declaring `IAsyncDisposable` is now **REFUSED AT THE DECLARATION**, with the **INTERFACE CHAIN** named (not a name test) | — |
| **#7326** | **WHOLE** — the §28.12 rejection lands and is pinned (four rows), **AND the §28.10 **VACUITY LINT**'s over-fire — THE RESIDUAL, **#7456** — IS FIXED by the disposal-family lane (`19fd5710`)** | — (nothing) |
| **#7354** | **WHOLE** — the **declaration-site** rejection follows the chain, the enum/choice/nested controls are added, **AND the **NESTED-RECORD reach gap** — THE RESIDUAL — IS **FIXED** by the #7490 lane (`0c562e95` → `32ca5cf3f1`): `Resolver::checkRecordRestrictions` NOW WALKS THE MODULE's OWN DECLARATION TREE WITH `forEachTypeAndNested` (#3574, §11.6) INSTEAD OF `ctx_.module()->children` ALONE, SO A `record` **INSIDE A `class`** ESCAPES NO §9.6/§9.7 RECORD RESTRICTION, AT ANY DEPTH** | — (nothing) |
| **#7424** | **WHOLE** — the two missing heads are added and their rows pass, **AND the **MECHANICAL DERIVATION** into `src/common/BuiltinRegistry.*` IS **NOW MECHANICALLY GUARANTEED** by the chain-table lane (`251e575d`): the new ratchet `scripts/check_disposal_family_chain.py` derives the disposal family FROM THE STDLIB DOCUMENT (the transitive closure of the extension graph from `IDisposable`) and compares it against the code table, the `std::array` size and the comment block, failing CLOSED, and the gate's ratchet sweep discovers it | — (nothing) |

**THE REVIEWER's SEVEN FINDINGS — FIVE CONFIRMED FIXED, ONE AN ACCEPTED DEFERRAL, ONE STILL OPEN AND
ALREADY TRACKED.**

* **F1 — THE CHAIN TABLE IS **COMPLETE**: **CONFIRMED FIXED.** THE REVIEWER ENUMERATED THE STDLIB's
  DISPOSAL FAMILY **HIMSELF** (v0.345, **34** `public interface` DECLARATIONS IN THE WHOLE DOCUMENT,
  OF WHICH **TEN** ARE THE FAMILY) AND THE TABLE `kDisposalHeadParents`
  (`src/sema/TypeResolution_Checks.cpp`) NOW LISTS **ALL TEN** WITH THE RIGHT PARENTS — **THE
  DIFFERENCE IS ZERO, NO HEAD IS MISSING.** **THE OTHER **24** STDLIB INTERFACES DO NOT EXTEND
  `IDisposable` — EACH WAS CHECKED.**
* **F2 — THE DUPLICATION WITH `src/common/DisposalKind.cpp` — **ACCEPTED DEFERRAL**, WITH A NEW
  INCONSISTENCY IT LEAVES BEHIND.** THE DEFERRAL IS RECORDED WHERE THE BRIEF ASKED (a cited source
  comment beside the table). **THE REVIEWER MEASURED THE TIP's RULE AS **CORRECT**: `await using` OVER
  AN `IAsyncDisposable` PRINTS `RES-ASYNC-DISPOSE`; THE SYNC SPELLING PRINTS `RES-SYNC-DISPOSE` — THE
  CHAIN REACHES THE ASYNC PATH EVEN THOUGH `DisposalKind.cpp` NAMES ONLY `IAsyncDisposable`.** **BUT
  THE DEFERRAL LEAVES A **FALSE §28.10 VACUITY WARNING** ON **EXACTLY THE TWO HEADS THE LANE JUST
  ADDED** (`IOwningCollection`, `IOwningList`) **AND ON `IDisposable`/`IAsyncDisposable` — WHILE THE
  SIX SIBLING HEADS DO **NOT** WARN — **AND THE WARNING IS DEMONSTRABLY FALSE (THE PROGRAM STILL
  PRINTS `RES-DISPOSED`).** **=> THE COORDINATOR FILED IT AS **#7456**.**
* **F3 — THE NESTED-RECORD REACH GAP — **NOT FIXED, AND IT IS **NOT** THE LANE's.** MEASURED
  INDEPENDENTLY ON BOTH ARMS: A NESTED `record` IS **ADMITTED** ON THE TIP **AND** ON THE BASE; A
  NESTED `enum` IS **ADMITTED ON THE BASE** AND **REJECTED ON THE TIP**. **=> §28.12's SINGLE SENTENCE
  IS NOW APPLIED AT THREE DIFFERENT DEPTHS. **=> IT IS ALREADY TRACKED** (THE COORDINATOR's RESIDUAL
  COMMENT ON #7354, 2026-10-27, WITH THE SAME MECHANISM AND THE SAME FIVE-ROW TABLE), **SO #7354 STAYS
  OPEN AND THE LANE IS RIGHT TO LEAVE IT.**
* **F4 — THE `#7322` ROW WAS VACUOUS — **CONFIRMED FIXED** (BOTH ARMS MEASURED). THE HELPER
  `expectNoMemorySafetyDefect` IS **REMOVED** (A COMMENT RECORDS WHY), THE BINDING MOVED INTO AN
  **`async` LAMBDA INSIDE A PLAIN `Main`** (THE CONTEXT §28.12 ADMITS), AND THE ORACLE IS NOW THE FULL
  `expectRunsClean(…, "[1]\n")`. **THE ROW WAS ALSO **RENAMED** — A DIAGNOSTIC-TEXT RENAME IS A
  BEHAVIOUR FLIP, SO THE REVIEWER CHECKED IT:** OLD
  `the_vacuous_await_using_outside_an_async_method_reads_no_out_of_bounds_spec_28_12` → NEW
  `the_vacuous_await_using_in_an_async_lambda_in_a_plain_main_reads_no_out_of_bounds_spec_28_12`.
  **ARM C (the pre-#7322 backend, the NEW test file): "3 passed, 7 failed"** — the re-spelled row
  FAILS with `AddressSanitizer`/`heap-buffer-overflow`. **ARM D (the same backend, the OLD test file):
  "4 passed, 6 failed"**, **AND THE OLD ROW **PASSES** THERE — **THE VACUITY, MEASURED: ON THE
  PRE-#7322 BACKEND THE OLD ROW WAS GREEN *BECAUSE ITS PROGRAM WAS REJECTED*.**
* **F5 — THE WRONG `stdlib §2.18` CITATION — **CONFIRMED FIXED.** THE SOURCE COMMENT NOW READS
  **`stdlib §2.10`** (`docs/Bezel_Standard_Library.md:525` IS `### 2.10 'IDisposable' and
  'IAsyncDisposable'`; `:532` IS `public interface IAsyncDisposable: IDisposable`; **§2.18 (`:1136`)
  IS `IFormattable`** — WHICH THE OLD CITATION POINTED AT). **=> `check_src_citations.py` PRINTED
  "clean" BOTH BEFORE AND AFTER — IT PROVES **RESOLVABILITY**, NOT WORDING.**
* **F6 — THE UNNAMED ACCEPTANCE FLIP — **CONFIRMED FIXED (THE FLIP IS REAL, CORRECT, AND NAMED).**
  **§9.7 (spec:1409) BANS THE **BUILT-IN** `IDisposable`; §2.3's Disposal-interfaces ROW IS
  **shadowable = Yes**, AND THE TEXT AT SPEC:161 SAYS *"Only the first row's names cannot be shadowed
  … every other always-available name may be shadowed by local declarations."* **=> A USER's OWN
  `IDisposable` EXTENDS NOTHING, SO THE RECORD IMPLEMENTS NO DISPOSAL INTERFACE.** **THE FLIP IS
  NAMED IN `5e357660`'s BODY WITH THE OLD AND NEW EXPECTATIONS SPELLED OUT, AND PINNED BY THE NEW ROW
  `record_listing_a_shadowing_user_idisposable_is_still_admitted_spec_9_7`.**
* **F7 — THE MISSING ENUM/CHOICE CONTROLS — **CONFIRMED FIXED (ALL SIX ROWS).** **ARM B (the source
  REVERTED TO THE NAME-BASED CHECK, THE TEST BINARY REBUILT): "13 passed, 15 failed"**, INCLUDING
  `nested_enum_listing_i_async_disposable_is_rejected_spec_28_12` AND
  `nested_choice_listing_i_async_disposable_is_rejected_spec_28_12`. **THE FOUR ADDED **CONTROL** ROWS
  ARE `[  OK  ]` ON **BOTH** ARMS.**

**THE FAIL-WITHOUT — MECHANICAL, BOTH ARMS, WITH THE **TEST BINARY** REBUILT EVERY TIME.**

| arm | the reverted file | filter | result |
|---|---|---|---|
| **A** | `src/sema/TypeResolution_Checks.cpp` → `af244910^` (the 8-head table) | `--area sema disposal_interface_chain_7354` | **22 passed, 6 failed** — **EXACTLY** the six #7424 REJECT rows |
| **B** | the same file → `a37947ec` (the **PICKED** name-based check; the lane's original `d31cd5aa` is **NOT** on staging, so a reader must use the picked SHA) | the same | **13 passed, 15 failed** — including the two F7 nested rows; **the four F7 CONTROLS ARE `[  OK  ]`** |
| **C** | `src/codegen/LlvmBackend_Setup.cpp` → `9bb9387f^` (pre-#7322), the NEW test file | `--area codegen await_using_vacuous_rhs_7322` | **3 passed, 7 failed** |
| **D** | the same backend, the OLD test file | the same | **4 passed, 6 failed** — **THE OLD ROW PASSES (THE VACUITY, MEASURED)** |

**EVERY REVERTED FILE WAS RESTORED, AND `git status --short` WAS **CLEAN** AFTER EACH ARM AND AT THE
END.**

**THE CORPUS — READ-ONLY, AGAINST A COPY. **THE **LANE TIP** — **NOT** THE PICK: THE FIGURES ARE THE REVIEWER's MEASUREMENT ON THE LANE's OWN TIP, AND THE PICK ITSELF (`a37947ec`) WAS NOT MEASURED: `3693 cases: 3157 ok, 233 known-divergent, 296 unclassified, 0 REGRESSION, 7 UNBANKED`. THE BASE `b19567b1`: `3693 cases: 3157 ok, 234
known-divergent, 296 unclassified, 0 REGRESSION, 6 UNBANKED`.** **=> 0 REGRESSION ON BOTH ARMS, AND
THE UNBANKED SET DIFFERS BY **EXACTLY ONE ROW**: `s28_12_0015` — **BANKED `fail`, AND IT NOW PASSES**,
MOVED BY THE LANE's OWN `a37947ec` — THE **PICKED** COMMIT (ITS CORPUS ROW IS "await using outside an async method is a
compile error"). **=> THAT IS THE EXPECTED CONSEQUENCE OF THE FIX, AND THE COORDINATOR RE-BANKS
`baseline.tsv` **LAST**, BEFORE THE GATE. THE OTHER SIX ARE **PRE-EXISTING** ON THE BASE ARM.**

**THE ASan LEG: `BEZEL_AOT_FORCE_SANITIZE=address scripts/agent_check.sh --area codegen
await_using_vacuous_rhs_7322` → `10 passed, 0 failed`, `NATIVE SANITIZER: 0 check(s) skipped`,
`NATIVE SANITIZER INFRA: 0 build/link/exec break(s)`, `CHECKED TOOLCHAIN: 0 skip(s)`.** (THE VERDICT IS
THE **NUMERAL**; THE PARENTHETICAL IS AN ALWAYS-PRINTED LEGEND.) **`scripts/agent_check.sh --lit` →
`Passed: 2562 (100.00%)`.** **`scripts/check_src_citations.py` → clean** (RESOLVABILITY ONLY). **THE
OTHER TARGETED FILTERS, ALL 0 FAILED: `--area sema record_idisposable_constraint_4755` (5),
`sema_record` (30), `sema_enum_restrictions_nested_3601` (4), `choice_restrictions_3869` (19),
`--area ownership disposal` (22), `--area ownership using_var_choice_copy_7316` (11).**

**THE ADMITTING-TWIN MATRIX — **CONSISTENT, 12/12 CELLS. `await` AND `await using` **AGREE IN EVERY
CELL**:** A PLAIN METHOD (BOTH REJECTED) · AN ASYNC METHOD (BOTH ADMITTED) · A NON-ASYNC LAMBDA IN A
PLAIN METHOD (BOTH REJECTED) · A NON-ASYNC LAMBDA IN AN ASYNC METHOD (BOTH REJECTED) · AN ASYNC LAMBDA
IN A PLAIN METHOD (BOTH ADMITTED) · AN ASYNC LAMBDA IN AN ASYNC METHOD (BOTH ADMITTED) · A NESTED CLASS
METHOD (BOTH REJECTED) · A CONSTRUCTOR (BOTH REJECTED) · A STATIC CONSTRUCTOR (BOTH REJECTED) · A
NON-ASYNC LAMBDA INSIDE AN ASYNC LAMBDA (BOTH REJECTED) · AN ASYNC LAMBDA INSIDE A NON-ASYNC LAMBDA
(BOTH ADMITTED) · A LEGAL `await using` INSIDE `for`/`try`/`finally` IN AN ASYNC METHOD (**STILL
ADMITTED — NO REGRESSION**).

**THE ONE PRE-EXISTING ROW WHOSE **PROGRAM** CHANGED: `ownership_using_var_choice_copy_7316.
await_using_var_choice_copy_is_unchanged` — THE PROGRAM MOVED TO `public static async method Main():
Task` (THE OLD ONE IS NOW REJECTED BY §28.12), **ITS EXPECTATION IS UNCHANGED** (COMPILES, RUNS, EXITS
0, PRINTS `done`), **AND ITS NAME IS UNCHANGED.** NAMED IN `a37947ec`'s BODY — THE **PICKED** COMMIT — WITH THE ⚠ MARKER. **THE
NEEDLE `record `R` may not implement `IDisposable`` + `(§9.7)` IS **PRESERVED BYTE-IDENTICALLY** IN THE
SOURCE FOR THE DIRECT CASE — AND **IT IS NOT A VACUOUS `CHECK-NOT`**: THE LIT TRACK IS **2562 (100.00%)**
AND THE gtest ROW `record_listing_idisposable_is_rejected_spec_9_7` PINS THE SAME TEXT AT A REAL
LOCATION (2,8).**

**THE TWO NEW DEFECTS THE REVIEWER MEASURED — **THE COORDINATOR FILED BOTH ON 2026-10-28** (LANES MAY
NOT FILE).**

| # | the one-line mechanism | where |
|---|---|---|
| **#7456** | **THE §28.10 VACUITY LINT OVER-FIRES ON AN `IDisposable`/`IAsyncDisposable`/`IOwningCollection<T>`/`IOwningList<T>`-DECLARED BINDING WHOSE OBJECT HAS A DESTRUCTOR — THE WARNING CLAIMS "NO DISPOSAL OBLIGATION (§28.9)" WHILE THE SCOPE-END DISPOSAL **DOES** RUN** | `src/common/BuiltinRegistry.cpp`'s `kDisposableBuiltinHeadNames` (**41** names — `std::array<std::string_view, 41>`) IS **MISSING** FOUR HEADS; `common::classifyDisposal` ANSWERS "no obligation"; `MethodOwnership::rhsIsDisposableClassType` (`src/ownership/Ownership.cpp`) BELIEVES IT |
| **#7457** | **A `where` BOUND NAMING **ANY OF THE **EIGHT** HEADS OUTSIDE THE TWO** `IDisposable`/`IAsyncDisposable` **ADMITS A RECORD** TYPE ARGUMENT — THE BOUND CHECK DOES NOT CONSULT THE TRANSITIVE CLOSURE. **THE ISSUE's OWN TABLE MEASURED **TWO** HEADS; THE DISPOSAL-FAMILY LANE's RE-MEASUREMENT SHOWED **EIGHT**: WITH THE BOUND INTERFACE's OWN TYPE ARGUMENT A **CLASS** (THE SHAPE THE ISSUE's OWN `Res` IS MEANT TO BE), **ALL EIGHT** OF `IEnumerator` `IAsyncEnumerator` `IOwningEnumerable` `IOwningEnumerator` `IOwningAsyncEnumerable` `IOwningAsyncEnumerator` `IOwningCollection` `IOwningList` **ADMIT** A RECORD TYPE ARGUMENT. **AND THE ISSUE's OWN TWO HEADS (`IEnumerator<out T>` / `IAsyncEnumerator<out T>`, stdlib §4.12) CARRY **NO** `where T: class` CLAUSE AT ALL — SO THEY **ADMIT** A RECORD TYPE ARGUMENT; THE CLAUSE THAT REJECTS A RECORD FOR THE **SIX** OWNING/ITERATION HEADS IS **THEIRS** (stdlib §4.2 `IOwningCollection<T>` / `IOwningList<T>`, WHERE THE CLAUSE IS ON THE **NEXT** LINE, AND stdlib §4.13's FOUR), NOT THE DISPOSAL RULE's — **WHICH IS WHY THE ISSUE's TABLE UNDERSTATED IT** | THE **CALL SITE** IS `src/sema/TypeResolution_Checks.cpp`'s RECORD/ENUM-AGAINST-A-DISPOSAL-BOUND GUARD, WHICH CALLS `src/sema/TypeRef.hpp`'s `primitiveNeverConformsToBuiltinIface` — **WHICH NAMES **ONLY** `IDisposable` AND `IAsyncDisposable`** — **AND (AT THE U4 TIP `fcc0ee74`) THAT CALL SITE IS `src/sema/TypeResolution_Checks.cpp`'s RECORD/ENUM-AGAINST-A-DISPOSAL-BOUND GUARD AT LINE **13108** — WHICH IS **NOT** THE SITE OF ANY TWO-HEAD NARROWING: THE TWO-HEAD LIST IS IN **ANOTHER FILE** (`src/sema/TypeRef.hpp`, LINE 219 AT THE U4 TIP), AND THE CHAIN TABLE THE #7424 LANE FIXED WAS IN `src/sema/TypeResolution_Checks.cpp` AT LINE **1357** — **11,751 LINES** AWAY (13108 − 1357), **NOT** "~190".  **=> THE "SAME FILE, IN TWO PLACES" CLAIM IS **FALSE** (THE "~190" CAME FROM THE #7457 ISSUE's OWN BODY, WHICH WAS WRONG ON BOTH COUNTS), AND IT IS CORRECTED HERE.  AND SINCE `5e57953d68` (AN ANCESTOR OF STAGING) THE HELPER READS THE **SHARED TEN-HEAD TABLE** (`common::isBuiltinDisposalFamilyHead`), SO THE TWO-HEAD NARROWING NO LONGER EXISTS AT ALL — AT THE LANDING TREE THE CALL SITE IS LINE 13138 AND THE TABLE IS `src/common/BuiltinRegistry.cpp`'s `kBuiltinDisposalFamilyHeads` (LINE 1472), **A DIFFERENT FILE** |

**=> BOTH ARE **PRE-EXISTING** (IDENTICAL ON THE BASE ARM) AND **BOTH ARE THE **SAME
MECHANICAL DERIVATION** #7424's RESIDUAL NAMES. **=> THE COORDINATOR DISPATCHED A **CAMPAIGN-1
DISPOSAL-FAMILY LANE** (`r36-c1-disposal-family`, base `431fed95`) TO CLOSE BOTH.**

**THE REVIEWER's OWN NAMED GAPS (RECORDED, NOT HIDDEN):** IT DID **NOT** RUN THE FULL SUITE (THE
COORDINATOR's GATE); IT DID **NOT** RUN `runner.py --update`/`--classify` (THE CORPUS IS READ-ONLY FOR
A REVIEWER); IT DID **NOT** SWEEP THE OTHER **33** STDLIB INTERFACES' §14.2 CONFORMANCE; IT DID **NOT**
TOUCH `src/codegen/**` BEYOND THE F4 MEASUREMENT ARM (AND THE FILE WAS RESTORED); **IT DID **NOT**
RE-RUN `scripts/using_heads_grid.py` ON BOTH ARMS — **THAT IS THE COORDINATOR's INSTRUMENT, AND THE
COORDINATOR RE-MEASURES IT**; IT DID **NOT** MEASURE THE CORPUS ON THE PICK **CHERRY-PICKED ONTO THE
LIVE TIP**; IT DID **NOT** FILE ANY ISSUE; IT DID **NOT** AMEND ANY `docs/` FILE; IT DID **NOT** VERIFY
THE `s28_12_0015` RE-BANK (THAT IS THE COORDINATOR's LAST STEP). **ITS PROBE DIRECTORY IS LEFT IN PLACE
AT `/var/tmp/bezel-scratch/r36-rev-u4-ff/probes/` FOR THE HARVEST LANE.**

**Lane `r36-c1-disposal-family` (the DISPOSAL-FAMILY DERIVATION — ONE SHARED TABLE FOR THE
TEN DISPOSAL HEADS).**

* **SHAs (picked onto staging):** `2bd38c79` **fix #7456** -> `19fd5710a0`, `d443ff14` **fix #7457**
  -> `5e57953d68`. **TWO NEW CLOSURES: #7456, #7457.**
* **WHAT IT DID.** **THE LANE DELETED THE LOCAL `kDisposalHeadParents` COPY IN
  `src/sema/TypeResolution_Checks.cpp` AND MADE **ONE** SHARED TABLE IN `src/common/BuiltinRegistry.*`
  (`constexpr std::array<BuiltinDisposalFamilyHead, 10> kBuiltinDisposalFamilyHeads`) THE **SINGLE
  HAND-WRITTEN SOURCE OF TRUTH**, READ BY **THREE** PLACES: **THE §28.10 VACUITY LINT**, **THE `where`
  BOUND CHECK**, AND **THE DECLARATION-SITE CHAIN**. **=> ONE TABLE, **TEN** HEADS, INSTEAD OF TWO
  NAMES IN ONE PLACE AND A CHAIN IN ANOTHER.**
* **THE REVIEWER's VERDICT: **ACCEPT-W-NOTES** (`a739babe5503ae460`) — **NO BLOCKING DEFECTS.** **THE
  REVIEWER's CHECKS, ALL MEASURED:**
  * **THE TABLE MOVE IS FAITHFUL.** **ALL **TEN** HEADS' PARENTS WERE CHECKED AGAINST
    `docs/Bezel_Standard_Library.md` **v0.345** — `IDisposable` (§2.10 L528) ·
    `IAsyncDisposable -> IDisposable` (L532) · `IOwningCollection -> IDisposable` (§4.2 L1603) ·
    `IOwningList -> IOwningCollection` (L1612) · `IEnumerator -> IDisposable` (§4.12 L2486) ·
    `IAsyncEnumerator -> IAsyncDisposable` (L2503) · `IOwningEnumerable`/`IOwningEnumerator ->
    IDisposable` (§4.13 L2513/L2517) · `IOwningAsyncEnumerable`/`IOwningAsyncEnumerator ->
    IAsyncDisposable` (L2532/L2536). **=> AND #7424's OWN FILTER `disposal_interface_chain_7354`
    GIVES **28 passed / 0 failed** ON **BOTH** THE BASE ARM AND THE PICK, **IDENTICAL PER ROW** — **SO
    THE DECLARATION-SITE CHAIN's BEHAVIOUR IS UNCHANGED.**
  * **THE "**TWO CALLERS**" CLAIM IS CONFIRMED, AND **STRONGER** THAN THE LANE STATED.**
    `grep -rn classifyDisposal src/` GIVES EXACTLY **TWO** CALL SITES — `src/ownership/Ownership.cpp`
    (THE LINT) AND `src/codegen/LlvmBackend_Setup.cpp` (THE `await using` SLOT CLASSIFICATION). **AND
    `kind` AND `cls` HAVE **NO** READER ANYWHERE OUTSIDE `src/common/DisposalKind.cpp`; THE CODEGEN
    SITE READS **`mayHoldClass` ONLY**, AND `mayHoldClass` IS **TRUE** IN **EVERY** DECLARATION-LESS ARM
    — **=> THE `await using` PATH IS CODEGEN-NEUTRAL, AND **NO SOUNDNESS CHANGE** FOLLOWS.**
  * **THE EXISTING TEST FILE EDIT IS **ADDITIVE**, NOT A FLIP.** `tests/support/
    builtin_registry_snapshot_test.cpp` IS **+11 / −0**: **FOUR** NAMES ARE ADDED TO THE **IN**-LIST OF
    `builtin_registry_snapshot.disposable_builtin_head_name_set_spec_28_3_28_9`, **NOTHING** IS
    REMOVED, AND **BOTH OUT-LISTS ARE UNTOUCHED AND CONTAIN **NO** FAMILY HEAD.** **=> OLD -> NEW:
    `isDisposableBuiltinHeadName("IDisposable" / "IAsyncDisposable" / "IOwningCollection" /
    "IOwningList")` WENT FROM **UNASSERTED AND FALSE** TO **ASSERTED TRUE** — **A STRENGTHENING.**
    (`32 passed / 0 failed`.)**
  * **THE FAIL-WITHOUT IS MECHANICAL ON **BOTH** ARMS, WITH THE **TEST BINARY** REBUILT EACH TIME.**
    **ARM A** (the five `src/` files reverted to `431fed95`): `disposal_family_vacuity_lint_7456`
    **9 passed / 4 failed**, `disposal_family_bound_record_7457` **6 passed / 9 failed** — **ALL
    **THIRTEEN** FAILING ROWS ARE THE LANE's OWN NEW ROWS, AND **NO** PRE-EXISTING ROW STARTS FAILING.**
    **ARM B** (ONLY `src/sema/TypeRef.hpp` REVERTED): THE SAME NINE FAIL, AND THE 7456 FILTER STAYS
    **13/0** — **=> THE TWO ISSUES ARE **INDEPENDENTLY** PINNED.**
  * **THE ASan LEG OVER THE LANE's OWN ROWS — WHICH THE LANE DID **NOT** RUN — WAS RUN BY THE
    REVIEWER: **13/0** AND **15/0** WITH **`NATIVE SANITIZER: 0 check(s) skipped`** AND **`CHECKED
    TOOLCHAIN: 0 skip(s)`** ON BOTH.**
  * **THE CORPUS: **0 REGRESSION**, AND THE PICK's REPORT AND THE BASE's REPORT DIFFER IN **NO** ROW's
    VERDICT.** **BOTH GRIDS UNCHANGED** (c2 **118 cells, 10 wrong**; c1 **153 cells, 96 ok, 31 wrong,
    8 mismatch, 9 n/a, 9 quarantined**, **DONE blocked by 39**) **AND THE c1 GRID's PER-CELL CLASS
    COLUMN DIFFS **EMPTY** BETWEEN THE BASE ARM AND THE PICK.**
  * **THE PRE-EXISTING FAILURE IS CONFIRMED PRE-EXISTING** —
    `generic_method_same_name_tparam_4813.extension_method_spec_26_5` FAILS ON **BOTH** ARMS.
* **THE REVIEWER's **FIVE** NOTES — **THE COORDINATOR's ACTIONS:**
  * **N1 — **#7463's BODY NEEDED A **THIRD** CORRECTION, AND NOW HAS IT.** THE BODY CLAIMED THAT THE
    §28.10 LINT WAS "THE ONLY SIGNAL A USER GOT ON A PROGRAM THAT LEAKS" **FOR THE `owned`-FACTORY
    REPRO IT PRINTS** — **AND THAT IS **FALSE**: `handleLocalDecl`'s **CASE-A** GUARD CARRIES
    `!ld.initializerHasOwnedMarker` (`src/ownership/Ownership.cpp:8192`), **SO THAT REPRO IS **SILENT
    ON **BOTH** ARMS.** **THE CLAIM HOLDS **ONLY** FOR THE **NON-`owned`** FACTORY TWIN, **WHICH
    WARNS ON THE BASE, IS SILENT ON THE PICK, AND LEAKS THE **IDENTICAL** `11200 B / 150 allocs` ON
    **BOTH** ARMS.** **=> #7463's BODY NOW PRINTS **BOTH** SHAPES IN A TABLE, AND #7456's CONSEQUENCE
    PARAGRAPH CARRIES THE SAME CORRECTION.**
  * **N2 — **THE **SPLICED §-QUOTE** IN `src/ownership/Ownership.cpp` IS **FIXED** BY THIS COMMIT.** THE
    LANE's COMMENT PRESENTED *"§28.12: the disposal dispatches 'through any interface — `IDisposable`
    or one deriving from it'"* — **BUT THE SPEC's WORDS AT `:7506` ARE *"through any interface —
    **`IAsyncDisposable`** or one deriving from it … or another such as **`IDisposable`**, which every
    class implements"*, SO THE QUOTED PHRASE **DOES NOT EXIST** IN THE SPEC.** **=> THE COMMENT NOW
    QUOTES §28.12's **SYNCHRONOUS** HALF VERBATIM — *"as the synchronous path selects the destructor
    chain it runs (§28.4) — never by the binding's static type"* — **WHICH IS THE PASSAGE THAT
    GOVERNS THIS DECISION ANYWAY.**
  * **N3 — **THE LANE's OWN GAP (iii) OVERSTATED ITSELF: **A **USER** INTERFACE EXTENDING `IDisposable`
    USED AS A `where` BOUND IS **NOT** UNFIXED — **THE REVIEWER MEASURED IT **REJECTED ON BOTH
    ARMS** (A **RECORD** ARGUMENT AND A **PRIMITIVE** ARGUMENT BOTH REFUSED WITH THE §14.2 MESSAGE),
    **SO THE COORDINATOR DID **NOT** FILE IT AS A DEFECT.** **THE REAL GAP IS ONLY THAT **NO ROW PINS
    IT**.**
  * **N4 — **THE `await using` COVERAGE GAP FOR THE **FOUR NEWLY-DERIVED** HEADS **STANDS**: **THE
    CODEGEN-NEUTRALITY ARGUMENT IS CORRECT BUT **UNPINNED**.**
  * **N5 — **THE "§14.4" ATTRIBUTION FOR THE OWNING HEADS' CLASS-ONLY RULE IS **LOOSE**: **THE RULE
    THAT ACTUALLY REJECTS A RECORD ELEMENT IS **THE STDLIB's OWN `where T: class`** ON THOSE HEADS
    (stdlib §4.2 :1603/:1612, §4.13 :2513/:2517/:2532/:2536), **APPLIED AT THE INSTANTIATION SITE
    THROUGH §14.2.** **=> #7457's BODY IS AMENDED.**
* **THE COORDINATOR FILED **#7463** — **THE HARD LEAK** THE LANE MEASURED (AN `owned` VALUE IN A LOCAL
  SLOT TYPED AT `IOwningCollection<T>`/`IOwningList<T>` IS **NEVER DISPOSED**; ASan `11200 B in 150
  allocs`; THE DESTRUCTOR NEVER RUNS) — **AND #7463's BODY NOW RECORDS **THE CORRECTED CAUSAL
  STORY** AND THE REVIEWER's RULING THAT **THE LINT WAS SIMPLY **WRONG** ABOUT THE OBLIGATION, **BUT
  THAT A WARNING OF A **DIFFERENT KIND** OVER THE LEAKING SHAPE **WOULD HAVE BEEN **DEFENSIBLE**.**
* **THE REVIEWER's NAMED GAPS, **OWED**: **THE `codegen`/`frontend`/`module` AREAS WERE **NOT** RUN
  EXHAUSTIVELY**, **AND THE **lit** TRACK WAS **NOT** RUN.** (§28.9, §28.10, §28.12, §9.7, §14.2, §8.7;
  stdlib §2.10/§4.2/§4.12/§4.13, **v0.345**.)

### Chain-table lane `r36-c1-chain-table` — 2026-10-29 (the #7424 RESIDUAL — the disposal chain table's MECHANICAL DERIVATION)

* **SHAs:** `251e575dc547e07705813c1e1b157d4bde5d211e`, branch `r36-c1-chain-table`, rebased onto
  the live staging tip `6a6d7acd`.
* **Files:** `scripts/check_disposal_family_chain.py` (NEW, 506 lines), `scripts/gate_full_cycle.sh`
  (+37) and `src/common/BuiltinRegistry.cpp` (+12, **COMMENT ONLY** — THE TABLE's TEN HEADS, ITS TEN
  CHAIN PARENTS AND THE `std::array<..., N>` SIZE ARE UNCHANGED, AND THE PREPROCESSED TRANSLATION
  UNIT IS BYTE-IDENTICAL).
* **THE RESIDUAL:** `kBuiltinDisposalFamilyHeads` WAS A **HAND-WRITTEN TRANSCRIPTION** OF
  `docs/Bezel_Standard_Library.md`. **A HAND-MAINTAINED MIRROR OF A DOCUMENT DRIFTS** — A **FUTURE**
  STDLIB INTERFACE EXTENDING `IDisposable` WOULD SILENTLY MISS, AND A RECORD / ENUM / CHOICE LISTING
  IT WOULD BE **ADMITTED** WHERE §28.12 REQUIRES A REJECTION.
* **THE REMEDY (A RATCHET, NOT A CODE GENERATOR):** THE NEW CHECKER **PARSES THE STDLIB DOCUMENT**,
  TAKES THE **TRANSITIVE CLOSURE** OF THE EXTENSION GRAPH FROM `IDisposable`, AND COMPARES THE
  DERIVATION AGAINST **THREE** TRANSCRIPTIONS OF THE SAME FACT (THE MACHINE-READABLE CODE TABLE, THE
  `std::array` SIZE, AND THE HAND-WRITTEN COMMENT BLOCK). **THE CHAIN PARENT IS THE BASE THAT IS
  ITSELF IN THE FAMILY** — NOT THE LITERAL FIRST BASE — SO `IOwningList<T>`'s PARENT IS
  `IOwningCollection<T>` ALTHOUGH ITS FIRST BASE IS `IReadOnlyList<T>`. `where` CONSTRAINTS ARE NOT
  EXTENSION CLAUSES AND ARE DISCARDED. **IT FAILS CLOSED.**
* **THE DERIVATION, MEASURED:** **34** `public interface` DECLARATIONS IN THE DOCUMENT; **EXACTLY 10**
  ARE IN THE DISPOSAL FAMILY, AND THE TABLE AGREES WITH THE DERIVATION ON EVERY HEAD AND EVERY CHAIN
  PARENT.
* **THE RATCHET IS DISCOVERED BY THE GATE:** IT IS WIRED INTO `scripts/gate_full_cycle.sh` (SELF-TEST
  FIRST, THEN `--strict`), AND `scripts/ratchet_sweep.sh` DERIVES ITS CHECK LIST FROM THE GATE's
  LITERAL PATHS — **SO THE CHECK COUNT GROWS AND THE CHECKER MUST BE GREEN OR THE GATE's RATCHET
  PHASE IS RED.**
* **FAIL-CLOSED, PROVEN BY MUTATION:** REMOVING A TABLE ROW, INSERTING A FAKE
  `public interface IFakeFutureDisposable: IDisposable` INTO THE STDLIB DOCUMENT, AND THE
  `--self-test`'s FOUR MUTATIONS (A REMOVED MEMBER, A CHANGED PARENT, A DELETED COMMENT ROW, A WRONG
  § ATTRIBUTION) **EACH TURN IT RED.**
* **NOTHING ELSE MOVES:** THE COMPILER's `.text` AND `.rodata` ARE IDENTICAL ON BOTH ARMS; THE c1
  GRID's 153 CELLS AND THE c2 GRID's 118 CELLS ARE UNCHANGED; THE CORPUS IS UNCHANGED (THE REPORT
  FILES ARE SHA-IDENTICAL); `absorbing_sites` STAYS **0** AND `checked_calls` / `support_calls` DO NOT
  MOVE.
* **THE RESIDUALS — FILED BY THE COORDINATOR:** **#7496** (`src/common/BuiltinRegistry.hpp` CARRIES
  A **SECOND, DIFFERENTLY-FORMATTED MIRROR** OF THE SAME COMMENT BLOCK, WHICH THE RATCHET DOES **NOT**
  COVER) AND **#7497** (`src/common/DisposalKind.cpp`'s BY-NAME SPECIAL CASE IS A **SECOND
  HAND-WRITTEN LIST** KEYED BY THE SAME INTERFACE NAMES).

## Exit review

**⚠ A COORDINATOR's NOTE ON THIS SECTION (2026-10-28).** IT IS PASTED **VERBATIM** FROM THE
REVIEWER's HAND-BACK, INCLUDING ITS OWN `### MODEL` TRAILER, AND IT IS THE REVIEWER's TEXT, NOT THE
COORDINATOR's. **THE LINE NUMBERS IT CITES ("Line 568 reads …") REFER TO THE RECORD **AS IT WAS
BEFORE** THE COORDINATOR's EDITION OF 2026-10-28 — THE EDITION ADDED TEXT ABOVE THIS SECTION, SO
**EVERY LINE NUMBER IN IT HAS DRIFTED** (BY +60 OR SO). THE REVIEWER's *SUBSTANCE* IS UNCHANGED AND
ITS FILE/SHA REFERENCES (`d9f11d15`, `e4a55c95`, `2250daa52`, the cell names, the corpus row ids)
ARE STILL EXACT. **THE COORDINATOR's EDITION APPLIED THE REVIEWER's SIXTEEN ACTION LINES
(`c1-exit-review-notes.md`) AS FOLLOWS: F1 (the DONE number, now 39 at the tip), F2 (the two missing
lane-record blocks — the U1c copies lane and the U2/U3 lane), F3 (the §28.12 count 9 → 6), F5 (the
anchor's "Closes 8" marked as a prediction), F6 (the label count re-dated), F7 (three paraphrases
marked and the spec's own words given), F8 (question 4 struck), F9 (the questions preamble corrected),
F11 (the grid's `object_class` vacuity blind spot recorded) and F12 (the coverage arithmetic 225 →
216 and the gap 72 → 81). **F4 (CLOSING #7316 BY MEASUREMENT) AND F10 (THE TRACKER's COUNTERS) ARE
TRACKER ACTIONS, NOT RECORD EDITS, AND ARE NOT IN THIS FILE.***

*Reviewer:* a fresh non-author reviewer — **not** a campaign-1 lane, not the author of any commit in
this campaign. *Model:* `MiniMax-M3.1-Flash` (1M context). *Date:* 2026-10-28.
*Trees measured:* the staging tip **`2250daa52`** ("fix #6825 #7284 #7290 #7293 #7294 #7295 #7299
#7300 #7323 narrow #6812 #7372"), built from scratch at `/mnt/fast/worktrees/r36-rev-c1-exit`; the
record's own baseline tree **`d9f11d15`**; the anchor's tree **`e4a55c95`**. Scratch and reports:
`/var/tmp/bezel-scratch/r36-rev-c1-exit/` (`report.tsv` = tip, `report-base.tsv` = `d9f11d15`,
`report-anc.tsv` = `e4a55c95`, `corpus.log`, `grid.log`, `gridbase.log`, `gridanc.log`).
*Every claim below is labelled **MEASURED** (I ran it), **QUOTED** (I read it in a file, an issue
or a commit) or **HYPOTHESIS**.*

#### ⚠ ONE CORRECTION TO THIS EXIT REVIEW — **2026-10-28, THE U4 LANE's REVIEWER**

**THE `(A9)` SECTION BELOW SAID *"#7354 REPRODUCES IN **TWO** OF ITS FOUR SHAPES"* AND NAMED THE
SECOND AS *"A GENERIC METHOD WITH `where T: IDisposable` **COMPILES**"*.** **THAT SECOND HALF IS
**WRONG**, AND IT IS **STRUCK IN PLACE**:**

* **§28.12 (v0.350), `docs/Bezel_Language_Spec.md:8368` (line 7475 at v0.345), QUOTED:** *"`where T: IDisposable` **admits
  it and every interface extending it** (`IAsyncEnumerator<T>` among them)."*
* **#7354's OWN ISSUE BODY SAYS SO: ITS SHAPE 1 IS *"required: admitted"*.**
* **=> THE BOUND ADMITTING `IAsyncDisposable` (AND `IAsyncEnumerator<int>`) IS THE **SPECIFIED**
  BEHAVIOUR, NOT A DEFECT. **#7354's REMAINING CONTENT IS **SHAPE 3 ALONE** — THE `record`'s
  DECLARATION-SITE BAN — **AND THE `choice`'s IS **#7394's**.**
* **THE REVIEWER MEASURED ALL FOUR SHAPES ON BOTH ARMS. **THE OTHER THREE HOLD AS THE SPEC AND THE
  ISSUE REQUIRE.**

### (A) THE AUDIT — is the record true?

**(A1) THE GRID — THE RECORD'S HEADLINE NUMBER IS TRUE FOR ITS OWN TREE AND STALE FOR THE
CAMPAIGN.** MEASURED on **`2250daa52`** (the current tip), with the command the brief names:

```
$ python3 scripts/using_heads_grid.py --bezelc build/bezelc --out <scratch> --jobs 6 \
      --timeout 60 --run-timeout 120 --report <scratch>.tsv
153 cells: 96 ok, 31 wrong, 0 known-red, 8 mismatch, 0 undecided, 9 n/a, 0 timeout
QUARANTINED: 9 cell(s) held out of the DONE count (pending #6194): 5 OK, 4 WRONG
NATIVE SANITIZER: 0 check(s) skipped
DONE: NO — 39 cell(s) block it (wrong + known-red + mismatch + timeout)
```

(EXIT = 1. **The DONE line is `39`, not the record's `53`.** The campaign-2 anchor that landed on
the tip did **not** move a cell: the plan's own measurement on `c5e8992d` also reads 39.)

MEASURED on **`d9f11d15`** — the tree the record's baseline block names — with the **same (tip)**
grid script, because the script did not exist at `d9f11d15` (it was added by `65f30f7e`):

```
153 cells: 82 ok, 43 wrong, 2 known-red, 8 mismatch, 0 undecided, 9 n/a, 0 timeout
QUARANTINED: 9 cell(s) held out of the DONE count (pending #6194): 3 OK, 6 WRONG
NATIVE SANITIZER: 0 check(s) skipped
DONE: NO — 53 cell(s) block it (wrong + known-red + mismatch + timeout)
```

**=> THE RECORD'S QUOTES OF THE DONE LINE ARE CHARACTER-FOR-CHARACTER EXACT — FOR THEIR OWN
TREE.** The record quotes the line five times; I checked each:

| the record's quote | where | tree it belongs to | my verdict |
|---|---|---|---|
| `DONE: NO — 53 cell(s) block it (wrong + known-red + mismatch + timeout)` | the baseline block | `d9f11d15` | **MEASURED EXACT** (em dash included) |
| the same, `53` | the fail-without table's **base** arm | `d9f11d15` | **MEASURED EXACT** |
| `DONE: NO — 51 cell(s) block it (wrong + known-red + mismatch + timeout)` | the fail-without table's **fix** arm (`b6053a68`) | `b6053a68` | **NOT RE-MEASURED** (not an ancestor of the tip; I did not build it). Its arithmetic is consistent (84 ok + 43 wrong + 0 known-red + 8 mismatch = 135; 43+0+8+0 = 51). |
| the same, `53` | the grid-discharge lane's own block | that lane's tree | **NOT RE-MEASURED** directly; that tree's compiler is the `d9f11d15` compiler, and my `d9f11d15` measurement reads the same 53. |
| the same, `53` | the grid-discharge lane's third run | that lane's tree | as above. |

**(A2) THE CELL CENSUS AT `d9f11d15` IS EXACT — I DIFFED IT CELL-FOR-CELL.** MEASURED: the
record's 43-row wrong-cell table is **identical, 43/43, name for name**, to the `WRONG` set of my
`d9f11d15` report; the record's **8 MISMATCH** rows are exactly the eight `*/copy_await_source`
cells (`class`, `class_async`, `derived_base`, `interface`, `abstract`, `generic_class`,
`generic_unconstrained`, `builtin_collection`); the record's **2 KNOWN-RED** rows are exactly
`value_choice/copy_local_source` and `value_choice/copy_const_source`, with the same detail
(`LEAK: 2100B/120 [#7316, fixed by b6053a68]`). The record's "the quarantined row's split is
UNSTABLE and is deliberately not quoted as a point value" is also right: I measured `3 OK / 6 WRONG`
on the base run and `5 OK / 4 WRONG` on the tip run, against the record's `5/4`, `6/3` and `4/5`.
**The grid section of this record is the strongest part of it.**

**(A3) THE FAIL-WITHOUT, PER LANE, FROM THE CAMPAIGN'S OWN INSTRUMENT — AND THE MOVE IS SPLIT
DIFFERENTLY FROM THE PLAN'S ACCOUNT.** MEASURED by running the same grid on three trees:

| tree | cells | DONE |
|---|---|---|
| `d9f11d15` (the record's baseline) | 82 ok, 43 wrong, **2 known-red**, 8 mismatch | **53** |
| `e4a55c95` (the anchor) | 90 ok, 37 wrong, **0 known-red**, 8 mismatch | **45** |
| `2250daa52` (the tip) | 96 ok, 31 wrong, **0 known-red**, 8 mismatch | **39** |

* The **ANCHOR** moved **8** cells wrong/known-red → OK: `abstract/await_using_var`,
  `derived_base/await_using_var`, `generic_unconstrained/{using_var, await_using_var,
  copy_param_source}`, `interface/await_using_var`, `value_choice/{copy_local_source,
  copy_const_source}` (53 → 45).
* The **U1c COPIES LANE** moved **6** cells wrong → OK: `interface/copy_param_source`,
  `string/{copy_local_source, copy_await_source, copy_const_source}`,
  `value_choice/{await_using_var, copy_await_source}` (45 → 39).

**=> THIS IS THE FAIL-WITHOUT THE BRIEF ASKS FOR, PER LANE, AND IT IS STRONGER THAN A SYNTHETIC
REVERT: each lane's cells are RED on the tree before it and GREEN on the tree after it, measured
with the campaign's own DONE instrument.** The `value_choice` copy cells the copies lane was
supposed to own are green at the tip; the `interface/copy_param_source` cell is green at the tip and
CRASHES (heap-use-after-free) at `d9f11d15`; the anchor's five cells are green at the anchor's tree
and red at `d9f11d15`.

**=> FINDING 1 (HIGH): the record's DONE number is stale, and the record does not say so.**
Line 568 reads "=> **53 cells block the DONE verdict** … **That is the campaign's DONE number**".
MEASURED, the campaign's DONE number at the tip is **39**. The record's header dates its numbers to
`d9f11d15`, but the DONE-instrument section does not carry that date next to the number it calls the
campaign's, and no lane block after the anchor updates it. This is the D3 class the grid's own
review note exists to eliminate, in the one number the campaign is judged by.

**=> FINDING 2 (HIGH): the record carries NO lane block for the U1c COPIES LANE, and none for the
U2/U3 lane.** MEASURED: the record's last commit is `e3e6bb38` (2026-10-28 08:01:34 +13:00); the
copies lane's three commits (`6e31b592`, `1a8bf0ae`, `c5e8992d`) are dated 08:17:20 +13:00 — **after**
the record was last written — and `git show --stat` on all three touches **no record file**, which
master plan §2.5 requires ("the record is updated **in the same commit** as the pick that changes
it"). The U2/U3 lane (`4823c418`, `fc09c0ca`, `cdf348ec`) has handed back and is under review; its
block is not there either. The record is therefore missing two lane records and the whole
45 → 39 half of the campaign's story.

**(A4) THE CORPUS ROWS IN SCOPE.** MEASURED with the corpus runner, read-only, on a **copy** of
`tests/conformance/rules/baseline.tsv` (`--baseline <copy>`; `--update` and `--classify` were never
run; the repo's baseline is byte-identical to the copy afterwards, md5 `d0613037…`):

```
$ python3 tests/conformance/rules/runner.py build/bezelc --baseline <copy> --filter 28.10
18 cases: 13 ok, 4 known-divergent, 1 unclassified, 0 REGRESSION, 0 UNBANKED
$ python3 tests/conformance/rules/runner.py build/bezelc --baseline <copy> --filter 28.12
29 cases: 7 ok, 5 known-divergent, 16 unclassified, 0 REGRESSION, 1 UNBANKED
$ python3 tests/conformance/rules/runner.py build/bezelc --baseline <copy>
3693 cases: 3157 ok, 234 known-divergent, 296 unclassified, 0 REGRESSION, 6 UNBANKED
```

* **§28.10: the record's `4` is TRUE** (4 banked-`fail` rows: `s28_10_0009`, `s28_10_0013`,
  `s28_10_0015`, `s28_10_0016`).
* **§28.12: the record's `9` is STALE.** MEASURED today: **5 known-divergent** (`s28_12_0003`,
  `s28_12_0005`, `s28_12_0006`, `s28_12_0008`, `s28_12_0015`) **+ 1 UNBANKED** (`s28_12_0007` — a
  row still banked `fail` whose case now **passes**) **+ 1 unbanked failure** (`s28_12_0013`, which
  still fails with the record's own expected/got). `s28_12_0020` and `s28_12_0021` now **pass**
  (the anchor fixed them) and have no baseline row.
  **=> FINDING 3 (MEDIUM): the record's "the true divergence count in scope is 4 (§28.10) and 9
  (§28.12)" is true for `d9f11d15` and is now 4 and 6. The one `UNBANKED` row is the KNOWN
  LAST-STEP RE-BANK the brief names — `s28_12_0007` — and it is NOT a defect; the two rows the
  coordinator is to add (`s28_12_0020`, `s28_12_0021`) must be banked `pass`, not `fail`.**
* The six `UNBANKED` rows of the whole corpus are all "banked `fail`, now passing": `s28_12_0007`
  (campaign 1's), `s22_6_0112/0113/0114` and `l9_5_0004` (campaign 2's anchor), `l9_6_0027`
  (`62735ebc`).

**(A5) THE ISSUE STATES — EVERY ISSUE THE RECORD NAMES, RE-CHECKED.** MEASURED with
`gh issue view <N> --json state,closedAt` (and the label list) for **all 24 members, all 13
siblings, the anchor's seven, and every issue the record's tables cite**:

* **The 24 entry members: the record's census is TRUE.** 19 OPEN, 5 CLOSED — **#7283**
  (2026-10-27T14:43:18Z), **#7306** (14:43:15Z), **#7322** (14:43:21Z), **#7327** (11:51:16Z),
  **#7337** (11:49:03Z) — exactly the five the record names, with the record's timestamps.
* **The coordinator's D3 correction is RIGHT.** MEASURED from the issues' own closing comments:
  **#7283 → `87fdfa67`** (and `9bb9387f`), **#7306 → `9bb9387f`**, **#7322 → `9bb9387f`** for its
  own part and `87fdfa67` for the two shapes it split into, **#7327 → `87fdfa67`** (its closing
  comment names `87fdfa67`, pushed to `origin/main` as `b04c9f9b`), **#7337 → `87fdfa67`**. The
  record's D3 block is true as written.
* **#7416** (new-13) is CLOSED, and its closing comment names the fixing commit (`b920a546`, picked
  as `6e31b59228`) with the coordinator's own ASan measurement — **the D3 class is satisfied there
  too.**
* **The U2/U3 lane's two members are still OPEN** (#2623, #6194) — MEASURED, and its three commits
  are **not** ancestors of the tip (`4823c418`, `fc09c0ca`, `cdf348ec`: "NOT ancestor"), so the
  tracker has not been told anything they did.
* **The anchor's five `fix`-named issues are still OPEN** (#3482, #7338, #7328, #5653, #3516), and
  so are the copies lane's three (#7387, #7388, #7374): their closing keywords fire only when
  `r30-c1` reaches the default branch. Not a defect — but the tracker states will move at the merge
  and the record's issue table will need a pass then.
* **=> FINDING 4 (MEDIUM): #7316 is measurably FIXED and will never close by itself.** MEASURED:
  the grid's `value_choice/copy_local_source` and `value_choice/copy_const_source` cells were
  KNOWN-RED `[#7316, fixed by b6053a68]` at `d9f11d15` and are **OK** at `e4a55c95` and at the tip
  — the ANCHOR closed them. But no commit's **subject** names #7316 (the anchor's body mentions it
  once, in a citation), so the closing keyword never fires and **#7316 is still OPEN**. The
  coordinator should close it by measurement, naming the commit that fixed it.
* **=> FINDING 5 (MEDIUM): the record's "Closes 8" in the anchor's one-page contradicts the
  tracker and the record's own lane block.** QUOTED: "**Closes 8:** #3482 … #7338 … #7339, #3483,
  … #7328, #5653, #3516, #3515". MEASURED: **#7339, #3483 and #3515 are all OPEN**, and the
  record's own anchor lane block says "#3483's is an `owned IAsyncDisposable` RETURN … #3515's is
  the PER-LEVEL reclamation chain" and "#7339 IS HELD OUT FOR THE OWNER". The one-page section is
  the entry lane's **prediction**; the record never marks it as superseded, so a reader meets
  "Closes 8" and then a tracker that says three of the eight are open.

**(A6) THE LABEL COUNTS — LIVE, NOT MEMORY.** MEASURED now:
`gh issue list --label campaign:using-heads --state open --limit 200` → **39**; `--state closed` →
**4**; all → 43. The record distinguishes the two counts and the distinction is honoured: the
**"at this commit's timestamp"** count of **34** is arithmetically 19 + 1 (#7374) + 12
(#7386–#7397) + 2 (#5355, #7146) ✓, and the **"LIVE, measured on 2026-10-28"** count of **40**
was TRUE at the commit that wrote it (`5ea3e73d`, 2026-10-27T18:59:31Z — before #7416 closed at
19:18:52Z). **=> FINDING 6 (LOW): the "LIVE" number is one stale today (39, not 40) — a dated
number reading as a current one. The label count is a property of a moment, not of the campaign;
the record should date it as a timestamp rather than as "LIVE".**

**(A7) THE RULINGS — EVERY QUOTED §-SENTENCE CHECKED AGAINST THE SPEC, WITH THE NEIGHBOURS.**

* **The #3493 ruling is VERBATIM.** MEASURED from the issue's own last comment: *"Ruling: dispatch
  on the run-time object, through a `DisposeAsync` dispatch-table slot (this issue's option 2)."*
  Owner-approved 2026-10-27 ✓, and the record's account of what it leaves open (the reclamation
  half with #3515, #3516 gated on it, #7339 not named) matches the issue's text ✓. #3493 is OPEN
  ✓, as the record says.
* **§28.9 (v0.350), line 8051 — VERBATIM, neighbours included.** The record quotes "None of them
  is a class, and none has a `Dispose()` member: `x.Dispose()` over a value of one of these types
  is a compile error" ✓ and the neighbouring clause "the type's lifetime is managed by the runtime
  without user involvement" ✓ (the record cites it for new-11), and "`string` — the list's FIRST
  ENTRY" ✓ (the record's #7273 answer says so; MEASURED: `string` is the first bullet, `Type` the
  second — which also settles the record's question 4's premise that "§28.9 lists `Type`
  explicitly" ✓).
* **§28.12 (v0.350), line 8399 — VERBATIM**: "`await using` selects the logical-disposal path by
  the class of the object the binding **holds** … **never by the binding's static type**" ✓, and
  the record's reading of it (the static type decides only what is known before run time) is the
  sentence's own next clause ✓.
* **§28.12 (v0.350), line 8397 — VERBATIM**: "`await using var` takes §28.10's admission
  unchanged" ✓, and the neighbour sentence the record's question 1 depends on is there too ("Over a
  borrow right-hand side … the binding assumes no obligation, so scope exit disposes nothing").
* **§28.10 (v0.350), line 8169 — VERBATIM**: "the disposal at scope end is a no-op … and the only
  effect is the reassignability restriction" ✓.
* **§28.10 (v0.350), the borrow paragraph — VERBATIM**: "the `using var` does **not** assume a
  disposal obligation …: at scope end, no `Dispose()` is called, because the value is not owned by
  this scope" ✓ (the record's new-10 citation is exact).
* **§8.7 (v0.350), line 1260 — VERBATIM**: "`DisposeAsync()` is a member only of a type that
  declares or inherits it" ✓, **and the neighbour the record's question 3 leans on is there**:
  "the diagnostic naming the discharge points" ✓.
* **§28.12 (v0.350), line 8368 — VERBATIM**: "only a class implements it — a record, enum or
  choice that lists it, or lists an interface extending it, is a compile error at the declaration"
  ✓ (the record's new-6).
* **§28.12 (v0.350), line 8395 — VERBATIM**: "`await using` is valid only inside an async method
  or async lambda" ✓.
* **=> FINDING 7 (MEDIUM): three of the record's "quoted" §-sentences are PARAPHRASES presented as
  quotations.** (a) new-1's **§28.2** "the free there is logical disposal followed by reclamation:
  the idempotent `Dispose()`, a no-op where the flag is already set, then the release of the block"
  — **no such sentence is in the spec**; §28.2 says "`Dispose()` on any class instance is
  idempotent: the first call performs the disposal; subsequent calls are no-ops" (line 6230) and
  "**The compiler-inserted free.**" (line 6244). (b) new-1's **§28.12 step 2** "a **direct
  `DisposeAsync()` call** awaited outside `await using` … the cascade and the chain follow exactly
  as if `await using` had issued it" — the spec says "The cascade and the chain are the same
  regardless of which logical-disposal path ran" (line 7480) and "Asynchronous cleanup runs only
  where the program wrote `await using` … or awaited a direct `DisposeAsync()` call" (line 7482).
  (c) #2624's **§29.1** "Disposing the enumerator (`break`, exhaustion, or explicit `Dispose()`)
  disposes any outstanding yielded value the iterator still owns" — the spec (line 7771) reads
  "Disposing the enumerator (`break` from a `for` or `await for` loop, exhaustion, or
  `Dispose()`/`DisposeAsync()` on a manually-held enumerator) disposes any outstanding yielded value
  the iterator **owns**". **All three rulings' OUTCOMES survive** (and (b)'s replacement sentence
  is stronger), but the record must either quote the spec verbatim or mark the sentence a
  paraphrase. The house rule is that a quoted §-sentence is evidence; a paraphrase is a claim.

**(A8) THE QUESTIONS FOR THE NEXT TRAIN — THE HOLD-OUT, AND THE FILING.**

* **Question 3 (#7393) is HELD OUT** ✓ MEASURED: no commit anywhere claims #7393 (`git log --all
  --grep="#7393"` reaches only the two record commits that *mention* it).
* **Question 1 (#7390's shape) is HELD OUT** ✓ MEASURED: no commit claims #7390, and the grid's
  `object_class/await_using_var` and `object_class/copy_await_source` cells are still WRONG at the
  tip ("rejected, expected a run"), so the shape is unfixed.
* **=> FINDING 8 (MEDIUM): question 4 is NOT held out.** It names **#7338**, and **#7338 is
  claimed by the anchor's own commit** (`e4a55c95 fix #3482 #7338 #7328 #5653 #3516 narrow #3483
  #3515`) — MEASURED. The record's question 4 says "the answer is yes and no train is needed", so
  the question is answered; it should be struck from the "Questions for the next train" list or
  marked as answered by the anchor, because as it stands it names an issue a lane has taken.
* **=> FINDING 9 (LOW): the section's preamble claims filed spec-question numbers and lists none.**
  QUOTED: "*(filed spec-question numbers — the coordinator files; this lane cannot.)*". MEASURED:
  the only OPEN issue carrying both `spec-question` and `campaign:using-heads` is **#7412** ("may a
  `using var` be the operand of `throw`?"), and **#7412 is not in the record's four**. Master plan
  §1.5 asks for the question to be *filed* and the hunk held out; nothing of the four is filed.

**(A9) THE LANE RECORDS' CLAIMS — WHAT I RE-MEASURED AND WHAT I DID NOT.**

* **RE-MEASURED (the anchor, the U1c copies lane):** the fail-without for both, in the campaign's
  own DONE instrument, as set out in (A3). This is a *stronger* measurement than a synthetic
  revert of the `src/` files, because the cells are the campaign's own and they are RED on the
  pre-lane tree and GREEN on the post-lane tree.
* **RE-MEASURED (the members I named for the next lane):** **#7326 REPRODUCES** — `await using
  var t := o.ToString();` in a non-async `static method Main()` **compiles** (rc = 0), with only the
  §28.9 vacuity warning, against §28.12 (v0.350): "`await using` is valid only inside an async
  method or async lambda". **#7354 REPRODUCES in **ONE** of its four shapes** — a `record` declaring
  `IAsyncDisposable` **compiles** (against §28.12: "a record, enum or choice that lists it … is a
  compile error at the declaration").
  **⚠ CORRECTED 2026-10-28 BY THE U4 LANE's REVIEWER (its §4): **THE SECOND HALF OF THIS SENTENCE
  — "a generic method with `where T: IDisposable` **compiles**" — WAS **NOT** A REPRODUCTION, AND IT
  IS **STRUCK**.** **§28.12 (v0.350, `docs/Bezel_Language_Spec.md:8368`) SAYS THE BOUND **ADMITS**
  THE INTERFACE AND EVERY INTERFACE EXTENDING IT: *"where T: IDisposable` admits it and every
  interface extending it (`IAsyncEnumerator<T>` among them)"* — **AND #7354's OWN ISSUE BODY SAYS
  SO: ITS SHAPE 1 IS *"required: admitted"*.** **=> THE BOUND ADMITTING `IAsyncDisposable` IS THE
  **SPECIFIED** BEHAVIOUR, NOT A DEFECT, AND #7354's REMAINING CONTENT IS **SHAPE 3 ALONE** (THE
  RECORD'S DECLARATION-SITE BAN; **THE CHOICE'S IS #7394's**).** **THE REVIEWER MEASURED IT ON
  BOTH ARMS AND MEASURED THE OTHER THREE SHAPES AS HOLDING: THE BOUND ADMITS; `d.Dispose()` ON A
  VALUE HELD AT `IAsyncDisposable` RUNS THE DESTRUCTOR; AND `using var` OVER AN `IAsyncDisposable`
  TAKES THE SYNCHRONOUS PATH.****
* **NOT RE-MEASURED:** the other 22 members' per-row repros (they are QUOTED from the record); the
  copies lane's and the anchor's gtest rows under `scripts/agent_check.sh`; the U2/U3 lane's
  fail-without (it is under a separate review); the grid's 17-mutation check (I ran the gate's own
  30 grid tests instead, inside the 1093 below); the `b6053a68` arm of the fail-without table; the
  `object_string` twin leak and #2624's repro.

**(A10) THE CHECKERS (all run on `2250daa52`).**

* `python3 -m unittest discover -s scripts/gate/tests` → **Ran 1093 tests … OK** (includes the grid
  classifier's 30 tests — MEASURED: `scripts/gate/tests/test_using_heads_grid.py` holds 30 `def
  test_`).
* `python3 scripts/pregate_stale_needles.py` → `stale_needles: 7 removed literal(s) in src/, 2 gone
  from HEAD, 0 test hit(s)` — **no candidate**, exit 0.
* `bash scripts/check_docs.sh` → `check_docs: OK` (PIN clean, VERSION clean at v0.345; one
  **advisory**: "1 §-refs to undefined section numbers: [spec] §4.12").
* `python3 scripts/check_commit_trailers.py --range origin/main..HEAD` → `clean (29 commit(s))`.
* `python3 scripts/check_toolchain_skip_surface.py --list` → `--- checked_calls=3642
  support_calls=3659 absorbing_sites=0 ---`, and the plain run prints a **FAILURE**:

  > "checked_calls ROSE: baseline 3626, tree has 3642 (+16) … it is paired here with
  > `absorbing_sites` not falling … Either bank a drop in `absorbing_sites` alongside the rise, or
  > verify the new call site does not absorb a toolchain SKIP."

  **=> FINDING 10 (HIGH, gate-facing): the toolchain-SKIP ratchet is RED on the staging tip.**
  MEASURED: `origin/main` (`e7d55d02`) reads OK at 3627/3644/0; the staging branch's baseline says
  3626/3643/0 (`dbe770ee`, a re-bank that is NOT on `origin/main`) and the tip measures
  3642/3659/0. The rise is the branch's own lane commits — MEASURED per commit: `e4a55c95` +3,
  `e44ba7aa` +5, `6e31b592` +7, `1a8bf0ae` +6, `c5e8992d` +5. **`scripts/ratchet_sweep.sh` on the
  tip: `1 of 23 RED — do NOT launch a gate`**, and that one RED is this checker; `pregate.sh:113`
  runs the sweep, so **pregate will block the gate** until the coordinator banks or disposes of it.
  Two things make that a decision rather than a formality: (i) `--update` **refuses** a
  `checked_calls` rise unless `absorbing_sites` strictly falls, and it is already **0** — while the
  checker's own rule text says "checked_calls may rise, but only when `absorbing_sites` did **not**
  rise", which is satisfied here; the message demands a strict *fall* that is impossible from 0.
  (ii) `scripts/gate_full_cycle.sh:825` runs the same checker with `--strict` and keys its
  toolchain-skip phase on the **exit code**, which is **0 even on a failure** (as the brief warns)
  — so the cycle's own phase would report GREEN on this RED. The ratchet needs the coordinator's
  decision (bank with `--force` and the rule-text justification, or fix the checker's rule), and the
  gate phase's blindness to a RED ratchet is worth its own issue.

**(A11) TWO COVERAGE HOLES IN THE DONE INSTRUMENT — THE CAMPAIGN'S OWN DEFECTS THAT THE GRID
CANNOT SEE.**

* **=> FINDING 11 (MEDIUM): the grid has NO cell for the record's own question 2.** MEASURED: the
  §28.9 vacuity warning **fires wrongly** on `var s: Object := new Res(); using var x: Object := s;`
  — "the RHS is a value-typed/runtime-managed value" — at **both** `d9f11d15` and `2250daa52`,
  while the runtime value is a CLASS instance (§28.9's exempt set is `{string, Type, boxed value
  types, function values}`; §28.12: "never by the binding's static type"). The grid's
  `object_class/using_var` cell is **GREEN** because its expectation is `('RUN', ['DTOR'])` — a
  plain RUN, which does not look at the warning. The defect the record's question 2 describes is
  therefore invisible to the DONE verdict: §2.3's "the DONE instrument is green" can be satisfied
  while this defect stands. The `object_class` head's cells need `NOVAC`, not `RUN`.
* **=> FINDING 12 (MEDIUM): the grid has no `async enumerator` head, although the plan's head-kind
  list has one and #2623 is a member.** MEASURED: the grid's 17 heads are `abstract,
  builtin_collection, class, class_async, collection_literal, derived_base, function,
  generic_class, generic_unconstrained, interface, object_class, object_string, owning_choice,
  string, task, type_token, value_choice` — **no async-iterator head**, so #2623's head kind is
  not measured at all. The record's coverage-gap section counts the gap as **72** cells (the eight
  Object-declared heads × 9) and says the DONE number "covers 153 of the **225** cells the master
  plan's head-kind list implies"; the plan's list (campaign 1's "Entry check") reads `{class,
  derived-behind-base, interface, abstract, generic `T` (class-constrained / unconstrained), string,
  function value, value-like choice, owning choice, `Object` holding each of these, `Type`,
  built-in collection, **async enumerator**, `Task`}` — **ten** heads before "Object holding each of
  these", **ten** Object-expandable heads, plus `Type`, built-in collection, async enumerator and
  `Task` — i.e. **24** heads × 9 = **216** cells, and the grid's seventeenth head
  (`collection_literal`) is one the plan does not name. So the record's **225** counts the grid's
  extra head as if the plan had named it, and in exchange drops the plan's `async enumerator` head
  from the gap: the true gap is **81** cells (nine missing heads × 9), not 72.

**(A12) §2.3's CORPUS LINE, READ LITERALLY.** QUOTED (master plan §2.3): "every rule-corpus case
citing the campaign's sections is `pass`, or its `fail` row carries an `issue:` header pointing at
an OPEN issue labelled for another campaign or a ruling". MEASURED: the baseline's third column is a
free-text note, and **five of the nine in-scope `fail` rows carry only the note `divergence`** —
`s28_10_0009`, `s28_12_0003`, `s28_12_0005`, `s28_12_0006`, `s28_12_0015` — with no issue number.
`scripts/check_toolchain_skip_surface.py` aside, that line does not read as met today.

### THE VERDICT ON THE CAMPAIGN

**NOT CLOSED.** MEASURED, with the failing line named: master plan §2.3's **"the DONE instrument
is green"** fails — 39 cells block it (31 WRONG + 8 MISMATCH, and the `task` row is quarantined);
**"every member at entry is closed by a `fix #N` commit or re-scoped"** fails — 19 of the 24 entry
members are OPEN in the tracker, and of the anchor's and the copies lane's eight `fix`-named
issues none has closed yet (their closing keywords fire at the merge), with #7316 fixed but
**unclaimed** (Finding 4); and the corpus line is not met in the form §2.3 writes it (A12). The
record itself is the best-documented artefact in the run — its grid census is exact, its D3
corrections are true, its quotes are nearly all verbatim — but it is **three commits and 39 cells
behind its own campaign**, and its two missing lane blocks are the reason.

**THE RESIDUALS, NAMED.** (1) the DONE number 53 → 39 and the two missing lane blocks
(Findings 1, 2); (2) the stale §28.12 divergence count 9 → 6 plus the pending re-bank of
`s28_12_0007` and the two new `pass` rows (Finding 3); (3) the "Closes 8" contradiction (Finding
5); (4) the three paraphrased §-sentences (Finding 7); (5) question 4's hold-out breach and the
unfiled questions (Findings 8, 9); (6) the two DONE-instrument coverage holes (Findings 11, 12);
(7) the RED toolchain-SKIP ratchet the gate cannot see (Finding 10).

### (B) THE NEXT LANE

**THE RULING: THE NEXT LANE IS **NOT** CAMPAIGN 2's CONTINUATION AND **NOT** A NEW CAMPAIGN 3.**
It is **campaign 1's own next lane**, because campaign 1's DONE lines FAIL, and master plan §2.4
says so in terms: "The coordinator then flips §6 to `DONE` or, **if a DONE line fails, to `ACTIVE`
with the failing line as the next lane**." The reasoning, on the plan's own rules:

* **A new campaign 3 is ruled out twice over.** Master plan §3's campaign-3 row says its
  dependency is "**campaign 1 (shares `registerUsingDisposal`)**", and campaign 1 is not DONE; and
  §1.2's WIP rule allows a **second** ACTIVE campaign only in the **other track**, "**Never
  three**" — campaign 3 is an **ownership**-track campaign, the same track as campaign 1, so it
  cannot be the second ACTIVE campaign while campaign 1 is ACTIVE. Campaign 3's declared file set
  (`LlvmBackend_Assign/_Calls2/_Calls3/_Iterators/_Setup/_Stmts.cpp`) also overlaps campaign 1's
  `LlvmBackend_Setup.cpp` and campaign 2's.
* **Campaign 2's continuation is ruled out as the next dispatch.** Campaign 2 already has its
  anchor landed (`2250daa52`) and **three instance lanes in flight** (L5b, B3a, B4a), which hold
  `src/sema/ExprTypecheck_Calls.cpp`, `ExprTypecheck_Exprs.cpp`, `ExprTypecheck_impl.hpp`,
  `src/sema/Formattability.hpp`, `src/common/BuiltinRegistry.*`, `src/codegen/LlvmBackend_Calls.cpp`,
  `LlvmBackend_Exprs.cpp` and `LlvmBackend_Setup.cpp` (MEASURED by diffing each branch against
  `origin/main`). A further campaign-2 lane would collide with its own lanes, and campaign 2 is
  not waiting on the pool.
* **Campaign 1's remaining lanes are otherwise blocked**, so the only dispatchable lane is the
  sema one: the **U3 lane is in flight** (`r36-c1-u2u3`, handback under review, not picked) and
  holds `LlvmBackend_Setup.cpp`, `LlvmBackend_impl.hpp` and `LlvmBackend_Disposal.cpp`; the
  **U2a** lane (#3483, the #3493 itab route) wants `LlvmBackend_Setup.cpp` +
  `LlvmBackend_Async.cpp`; the **U2c** lane (#3515, the per-level reclamation chain) wants
  `LlvmBackend_Disposal.cpp` (`Emitter::emitBaseDisposeChain`); the **U2d** lane (#1531, #1603,
  #7389) is an async/disposal lane and is therefore **serialised** behind the in-flight U3 lane by
  the one-disposal-lane rule; the **U0** lane (#3471) is the record's own "last" cleanup and wants
  `LlvmBackend_Setup.cpp`; and **#7357** is correctly held out (its leak is in the source's
  producer +1 and is symmetric, so no copy-shaped fix can close it).

**THE NEXT LANE — NAMED, DISPATCHABLE, AND SPECIFIED.**

* **Campaign:** 1 (`using-heads`), sub-family **U1d + U2e** — the record's own lane-plan item 5,
  "**U1d + U2e lane (Sonnet, sema)** — the `await using` admission and the `IAsyncDisposable`
  conformance. Members: #7326, #7354. Not a disposal lane, so it may run beside the ownership lanes
  once the anchor lands." The anchor has landed and been reviewed ✓.
* **Branch / worktree:** `r36-c1-u1d-u2e` on the NVMe (a free worktree; do not reuse a lane's).
* **Members (all OPEN, none claimed by a commit, none a `spec-question`/`spec-defect` — MEASURED):**
  **#7326** (`await using` outside an async method is admitted; §28.12, v0.345), **#7354**
  (`IAsyncDisposable` conformance: the `where T: IDisposable` bound, the `Dispose()` member, and
  the record/enum/choice declaration rejections; §28.12 + stdlib §2.10, v0.345), plus the two
  filed siblings that are the same two shapes — **#7396** (new-8, the filed twin of #7326) and
  **#7394** (new-6, the filed twin of #7354's record/choice shape). **#7412 is excluded**: it
  carries `spec-question` and is therefore undispatchable.
* **Hot files (declared, and disjoint from every lane in flight):**
  `src/sema/ExprTypecheck.cpp` — the `await using` admission (the `isAwaitUsing` site, where
  `Symbol.hpp`'s flag is set) — and `src/sema/TypeResolution.cpp` / `TypeResolution_Checks.cpp` /
  `TypeResolution_Names.cpp` — the `IAsyncDisposable` conformance walk. **The lane must NOT touch**
  `src/sema/ExprTypecheck_impl.hpp`, `ExprTypecheck_Calls.cpp`, `ExprTypecheck_Exprs.cpp`,
  `src/sema/Formattability.hpp`, `src/common/BuiltinRegistry.*` (campaign 2's in-flight sets),
  `src/codegen/LlvmBackend_Setup.cpp`, `LlvmBackend_impl.hpp`, `LlvmBackend_Disposal.cpp` (the
  in-flight U3 lane), `src/common/DisposalKind.*`, `src/ownership/Ownership.cpp` (the anchor's),
  or `scripts/using_heads_grid.py` (the DONE instrument — the coordinator's; the lane proposes cells
  to the coordinator rather than editing the instrument itself).
* **Tier:** **Sonnet** (the record's own tier for this lane; it is a sema admission lane, not a
  disposal lane, and the two members are admission/conformance rules, not memory-safety defects).
* **THE FIRST MEASUREMENT THE LANE MUST MAKE — three things, in this order:**
  1. **Re-measure the members on the tip and record each verdict**: the #7326 program (a
     non-async `Main` with `await using var t := o.ToString();`) and the four #7354 shapes
     (`where T: IDisposable`; `var d: IAsyncDisposable := move r; d.Dispose();`; a `record`
     declaring `IAsyncDisposable`; a `choice` declaring `IAsyncDisposable`) — under the ASan AOT
     path, with the specific diagnostic asserted, not just an exit code. **I have already
     measured three of them at the tip** (Finding A9): #7326 compiles (rc = 0), the `record`
     shape compiles, and the `where T: IDisposable` shape compiles. The lane re-measures and
     records all four.
  2. **The site-availability check (master plan §4.7 rule 7's file-disjointness, done BEFORE any
     edit)**: prove that the `await using` async-context rejection can be implemented in
     `src/sema/ExprTypecheck.cpp` alone and the `IAsyncDisposable` declaration-site rejection in
     `src/sema/TypeResolution*.cpp` alone, i.e. **without** `ExprTypecheck_impl.hpp` or any of
     campaign 2's files. **If either shape cannot be fixed without touching a file another lane
     holds, the lane stops and reports that, and the lane re-scopes to U2e alone** (its site is
     `TypeResolution*.cpp` either way).
  3. **Re-derive the DONE baseline, not copy it**: run the grid on the tip BEFORE the lane starts
     and confirm the number it inherits is **`DONE: NO — 39 cell(s) block it (wrong + known-red +
     mismatch + timeout)`** (MEASURED on `2250daa52`; the record's `53` is stale — Finding 1), and
     add to the campaign record the lane's own **proposed** grid cells — a `REJ` cell over an
     `await using` in a non-async body (needle **§28.12**) and `REJ` cells for the record/choice
     declaration (needle **§28.12**) — with the note that the `object_class` head's cells must
     become `NOVAC` rather than `RUN` (Finding 11) and that the grid still has no
     `async enumerator` head (Finding 12).

**THE NEXT CAMPAIGN'S FIRST LANE — NAMED, CONTINGENT.** Master plan §2.4 requires the exit review
to name it, so it is named here; **it is not dispatchable today** and must not be dispatched until
campaign 1 exits.

* **Campaign:** **3 — `enumerable-heads`** (ownership track; §29.2, §29.11, §28.6, §21.4/§21.5,
  §26.5; stdlib §5, §4; label `campaign:enumerable-heads`).
* **First lane:** the **E0 ANCHOR lane** — "E0 (Opus) alone first" (master plan §3's lane split),
  one bridge keyed on the **runtime representation**, consulted at every face.
* **Tier:** **Opus** (an anchor lane; master plan §1.3 "one **anchor lane** (Opus, always) until
  the anchor lands and is reviewed").
* **Hot files (master plan §3, campaign 3's "Shared code path"):** `wrapAsIEnumerableForTarget`
  and its gate, `lowerIEnumerableFor`, `wrapArrayAsIOwningEnumerator`,
  `registerIterableSourceDisposal`, the §5 terminal arms' minting gate and `freeFreshInputTemp`,
  and the getter-frame faces (`propBlockGetterReturnKinds_`, `kPropGetterReturnMint`) — spread over
  `src/codegen/LlvmBackend_Assign.cpp`, `LlvmBackend_Calls2.cpp`, `LlvmBackend_Calls3.cpp`,
  `LlvmBackend_Iterators.cpp`, `LlvmBackend_Setup.cpp`, `LlvmBackend_Stmts.cpp`. **Note the
  collision:** `LlvmBackend_Setup.cpp` is held by campaign 1's U3 lane and by campaign 2's lanes
  today; campaign 3's anchor must be re-scoped to the `_Calls2/_Calls3/_Iterators/_Assign/_Stmts`
  files, or wait for campaign 1's exit.
* **First members (dispatchable once campaign 1 exits):** the E0 anchor's own instance members —
  **#6393** (a bare slot pointer with no +1), **#6409** and **#6451** (the getter frames).
  MEASURED: all three are OPEN, carry `campaign:enumerable-heads`, carry **no**
  `spec-question`/`spec-defect`, and are claimed by no commit. **#6387 is EXCLUDED** — MEASURED, it
  is already claimed by `a59b3e5d narrow #6387` (2026-10-18), which is on `origin/main` and on the
  tip, so it is not an unclaimed member. **#2331** ("RULING NEEDED") is excluded — it needs an owner
  ruling. The plan's two spec-question candidates, **#4740** and **#2331**, are OPEN but carry only
  `bug` + `campaign:enumerable-heads`; the coordinator should label them `spec-question` before they
  are dispatched (127 issues currently carry `campaign:enumerable-heads`; 0 of them carry a spec
  label).
* **THE FIRST MEASUREMENT:** the **entry check** (master plan §2.2) — re-measure all 125 members on
  HEAD (G's 19 are 60–105 days old; #3469 is the known moved one) and run the **face ×
  representation grid** once on HEAD to record the baseline: faces {local, return, getter (block /
  arrow / virtual / interface-dispatched), argument (owned formal / borrowed formal / interface
  formal), field store, `for` (with `break`, with `await`/`yield` in the body), `_ :=` discard,
  interpolation hole, extension receiver} × representations {Array family, Set/Dictionary snapshot,
  sorted snapshot, lazy pipeline (eager and lazy terminals), sync iterator frame, async iterator
  frame, user class implementing `IEnumerable<T>`, string as `IEnumerable<char>`,
  `IOwningEnumerable<T>` heads, imported each}; every cell correct or rejected, never a wild jump.
  The DONE line is: the grid green under ASan; the §29 corpus `fail` rows (7) and the stdlib §5 row
  (1) re-banked; `enumerable_grid_spellings.baseline` **lower**; and the `itmap$` wild-jump
  signature absent from every cell's ASan log.

### WHAT I DID **NOT** CHECK

* The **`b6053a68` arm** of the record's fail-with-out table (its `DONE: NO — 51 …` quote) — not
  built, not re-measured.
* The **grid-discharge lane's own tree** (its two `53` quotes) — not built; I measured `d9f11d15`
  with the tip's script instead and read the same 53.
* The grid's **17-mutation check** — not re-run; I ran the gate's own 30 grid tests (inside the
  1093) and the instrument's own three-tree behaviour.
* The **copies lane's and the anchor's gtest rows** under `scripts/agent_check.sh` — not run; the
  fail-without was measured through the grid, cell for cell, which is the campaign's own instrument.
* The **U2/U3 lane's** fail-without (`4823c418`, `fc09c0ca`) — not re-measured; it is under a
  separate review. Its issue states and its non-picked status I did check.
* **22 of the 24 members' per-row repros** — QUOTED from the record, not re-measured (only #7326
  and **one** of #7354's four shapes, above — **THE COUNT WAS "two" UNTIL 2026-10-28, WHEN THE U4
  LANE's REVIEWER SHOWED THAT THE SECOND OF THE TWO WAS NOT A REPRODUCTION AT ALL: A
  `where T: IDisposable` BOUND ADMITTING `IAsyncDisposable` IS **REQUIRED** BY §28.12 (v0.345,
  spec:7475) AND BY #7354's OWN ISSUE BODY.**).
* The `object_string` twin leak, #2624's repro, the §29.1 ruling's outcome, the `type_token`
  deferral row, the coverage-gap arithmetic's *intent*, the `9 n/a` cells' reasons, and the
  `--twins` variant of the grid (I ran the brief's command without `--twins`; the record's baseline
  used `--twins` and the cell count is 153 either way).
* **Campaign 2's** grid, record, and lanes' correctness — read only for the file sets and the
  `campaign:builtin-call-admission` label counts.
* The **full suite** and the **gate** — not run (the coordinator's).
* `scripts/ratchet_sweep.sh` — **run to completion**: `1 of 23 RED — do NOT launch a gate`, the one
  RED being `check_toolchain_skip_surface` (Finding 10). The other 22 ratchets are GREEN.
* The **lit track** — not run (`agent_check.sh --lit` is opt-in since R30 and the brief does not ask
  for it).

### MODEL

`model=MiniMax-M3.1-Flash (1M context)`
