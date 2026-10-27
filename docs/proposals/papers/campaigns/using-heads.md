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
  **34**. **The LIVE query, measured on 2026-10-28, returns 40**: six issues
  have been labelled since — **#7405, #7406, #7412, #7415, #7416 and #7417**.
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
| #2624 | none isolated by this lane | **blocked on a ruling.** The repro ran and produced the CORRECT behaviour, and the spec does not name the pending per-advance Task: §29.1's iterator protocol says *"Disposing the enumerator (`break`, exhaustion, or explicit `Dispose()`) disposes any outstanding yielded value the iterator still owns"* — the per-advance Task is not a yielded value, so whether it is part of the disposal obligation is not decided by the text. The ISSUE is OPEN. |
| #7283 | `87fdfa67 fix #7337, #7327` (the function-typed `await using` shape; #7327 is its re-file) | **not a defect at HEAD.** A function value is runtime-managed, and §28.9 says *"None of them is a class, and none has a `Dispose()` member: `x.Dispose()` over a value of one of these types is a compile error"*, so the `await using` spelling of it carries no disposal obligation, and §28.12's "`await using var` takes §28.10's admission unchanged" admits it. **The ISSUE is CLOSED** (2026-10-27T14:43:18Z), fixed by **`87fdfa67`** — the commit is named in the issue's own closing comment. |
| #7306 | `9bb9387f narrow #7322` (the vacuous `await using` shape) | as #7283, for a string (also runtime-managed, §28.9). **The ISSUE is CLOSED** (2026-10-27T14:43:15Z), fixed by **`9bb9387f`**. |
| #7322 | `9bb9387f narrow #7322` (its own part) and `87fdfa67` (the two shapes it was split into: #7327 and #7337) | as #7283; the commit names this issue. **The ISSUE is CLOSED** (2026-10-27T14:43:21Z). |
| #7327 | `87fdfa67 fix #7337, #7327` | **closed by the fix** — the commit is in `origin/main`. The ISSUE is CLOSED. |
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
gate: it compiles 153 programs and runs them under ASan, which is far too
slow for a gate cycle, and its DONE verdict is the campaign's, not the
repository's. What IS at the gate is the grid's **classifier**:
`scripts/gate/tests/test_using_heads_grid.py` (30 tests) is discovered by
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
* **The `task` row is QUARANTINED out of the DONE count** until #6194 lands.
  #6194's hang makes the same program on the SAME compiler hang on one run
  and complete on the next, so a DONE verdict that included the row would
  flip run-to-run. The quarantined cells are counted separately and reported
  on their own line; they never make the verdict better and never make it
  worse.
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
of the **135** cells the count covers (153 − 9 N/A − 9 quarantined). That is
the campaign's DONE number: the campaign is DONE when it is **0** (no WRONG,
no KNOWN-RED, no MISMATCH, no TIMEOUT).

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
`Object`-declared shapes stand for the eight). This lane adds none of them: it
is a read-mostly entry check and the fix-forward's scope was the instrument,
not the coverage.

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
none: §28.9 (v0.345) already says which values have no `Dispose()`, and §28.10
(v0.345) says the `using var` disposal at scope end is a no-op for them.
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

This lane did **not** edit `tests/conformance/rules/baseline.tsv` (the brief
forbids it): the three unbanked rows are the coordinator's to bank, and
`runner.py --classify --filter <case-id>` re-banks **per row** (a full
`--classify` rewrites every row's note).

## Questions for the next train

*(filed spec-question numbers — the coordinator files; this lane cannot.)*

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
4. **Whether the `Type` token belongs in the anchor's runtime-managed set.**
   #7338 asks for it; §28.9 lists `Type` explicitly, so the answer is yes and no
   train is needed — but §28.12's `await using` sentence does not say what
   happens to a `Type` head, so a ruling would make it explicit. Recorded here so
   the coordinator can decide whether it is worth a train.

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

| # | defect | minimal repro | §-citation | campaign |
|---|---|---|---|---|
| new-1 | a direct `DisposeAsync()` on a plain-`var` local leaks the receiver's block | `var x := new AsyncRes(); await x.DisposeAsync();` → `16 byte(s) leaked in 1 allocation(s)`; the `using var` spelling of the same program is clean | §28.12 step 2 ("a **direct `DisposeAsync()` call** awaited outside `await using` … the cascade and the chain follow exactly as if `await using` had issued it"), §28.2 ("the free there is logical disposal followed by reclamation: the idempotent `Dispose()`, a no-op where the flag is already set, **then the release of the block**") | using-heads (U2d, with #1531) |
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

## Exit review

*(to be written by a fresh reviewer who is not a campaign lane -- master plan §2.4.)*
