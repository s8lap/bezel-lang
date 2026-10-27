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
* **Already closed before the anchor (the anchor generalises, it does not newly
  close): 6** — #7322, #7306 (#9bb9387f narrow #7322), #7327, #7283, #7337
  (`87fdfa67 fix #7337, #7327`) and #3493's dispatch half (`e9c564fd fix #3494 /
  narrow #3493`).
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
those cells red (a leak) or red twice (a double free / UAF). The grid's
`NATIVE SANITIZER: 0 check(s) skipped` line is what makes that verdict real.


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
* The tracker label query **agrees with nothing yet**: `gh api 'repos/s8lap/bezel-lang/issues?state=open&labels=campaign:using-heads&per_page=100&page=1'`
  returns `[]` (0 issues) — the §1.6 labelling sweep has not been applied. The
  labels themselves exist (`campaign:using-heads` is in `gh label list`). Membership
  above is from the TSV, as the brief directs.

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
| 3493 | soundness | **moved — the dispatch half is fixed** | With a `virtual` base `DisposeAsync`, `await using var u: Base := new Der();` prints `DER_DA`: the derived override IS awaited (`e9c564fd`, "fix #3494 / narrow #3493"). The residual is #3515 (reclamation), which the fix's commit body names. |
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
**24** members (the TSV this lane read). The label sweep has since run, and
`gh issue view` over `campaign:using-heads` shows **23 open**: **#7327 and
#7337 are CLOSED** (both were in the TSV; both measured "does not reproduce"
here, which is consistent with their closure), and **#7374 is NEW** — the
`await using var` copy of a value-like choice local, filed off the #7316
review and labelled with this campaign. The grid's `value_choice` /
`copy_await_source` cell is #7374's exact spelling, and it is red.

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
   new-9, new-10. Grid rows: the `copy_local_source`, `copy_await_source`,
   `copy_const_source` and `copy_param_source` rows of every head. **Start with
   new-10**: the `using var` copy of a PARAMETER disposes the borrowed
   parameter (a heap-use-after-free, measured with a 20-line repro), and it is
   the one cell in the family that is a memory-safety defect rather than a
   leak. The `value_like choice` copy cells are #7316/#7374/#7357.
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
    [--filter class,string] [--jobs 10] [--timeout 60] [--run-timeout 90] [--report FILE] [--twins]
```

`--filter` is a comma-separated list of SUBSTRINGS matched against the head name,
the form name and the head's label (so `--filter class` selects every head whose
name or label contains `class`). Exit status 0 when every cell matches, 1 when at
least one cell is wrong, 2 on a usage/infrastructure error (a filter that matches
no cell is a usage error, not a pass). The summary prints
`NATIVE SANITIZER: <n> check(s) skipped`; **n must be 0** — a cell whose program
could not be linked under ASan is counted as skipped.

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
  so its expectation is DERIVED from the plain copy's rather than re-typed —
  the two spellings cannot drift apart — but it is a separate cell, because
  the await spelling has its own defect (#7374: it is admitted but never
  armed, so the copy's reference is never released).

17 × 9 = **153 cells**, 9 of which are N/A (the form does not exist for that
head — e.g. `owned` is inadmissible on an `Object` field, §28.1), so **144
runnable cells**.

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
  KNOWN-RED with that issue, and is NOT counted as WRONG.** The campaign's DONE
  criterion is "no WRONG cell". The KNOWN-RED rows today are the four
  `value_choice` cells (#7316, #7357, #7374) — see the table below.

**Baseline on HEAD (`d9f11d15`), measured:**

```
153 cells: 98 ok, 41 wrong, 5 known-red, 0 undecided, 9 n/a, 0 skipped
NATIVE SANITIZER: 0 check(s) skipped
```

=> **41 cells are WRONG on HEAD** (plus 5 KNOWN-RED), of the 144 runnable ones.
That is the campaign's DONE number: the campaign is DONE when it is **0 WRONG**.

**The fail-without / fail-with proof for the grid's new cells.** The grid is
new, so it has no fail-without of its own; the #7316 fix (`b6053a68`, "narrow
#7316: a `using var` copy of a value-like choice local releases the payload at
scope end") is the one commit that moves a cell, so it is the proof. A second
worktree was built at that commit and the SAME grid was run against its
compiler:

| cell | base (`d9f11d15`) | fix (`b6053a68`) |
|---|---|---|
| `value_choice` / `copy_local_source` | KNOWN-RED `#7316` | **OK** |
| `value_choice` / `copy_const_source` | KNOWN-RED `#7316` | **OK** |
| `value_choice` / `copy_await_source` | KNOWN-RED `#7374` | KNOWN-RED `#7374` |
| `value_choice` / `copy_param_source` | KNOWN-RED `#7357` | KNOWN-RED `#7357` |
| `value_choice` / `await_using_var` | KNOWN-RED `#7374` | KNOWN-RED `#7374` |

(the whole grids: base `153 cells: 98 ok, 41 wrong, 5 known-red, 0 undecided, 9
n/a, 0 skipped`; fix `153 cells: 101 ok, 40 wrong, 3 known-red, 0 undecided, 9
n/a, 0 skipped`)

=> the grid **discriminates the `var` source from the `const` source, and both
from the `await using` spelling and from the parameter source**: the fix turns
the two it covers green and leaves the two it does not (#7374, #7357) red.
The `const` source and the `await using` spelling were unpinned by anything
before this lane; they are now cells.

(Three `task` cells flip between the two builds — `task/using_var`,
`task/await_using_var` and `task/field_store` hang on one build and not on
the other. That is #6194's hang, and it means the `Task` row of the grid is
**unstable until #6194 lands**; a DONE run must treat the `Task` row as
"expected unstable" until then. The other 150 cells are stable across the two
builds.)

**The wrong cells on HEAD, with the member each belongs to:**

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
| `function` / `copy_local_source` | LEAK 3840B/60 | as above |
| `function` / `copy_await_source` | LEAK 3840B/60 | as above |
| `function` / `copy_const_source` | LEAK 3840B/60 | as above |
| `function` / `copy_param_source` | LEAK 3840B/60 | as above, with a parameter source |
| `interface` / `copy_param_source` | **CRASH: heap-use-after-free** | the callee disposes a BORROWED parameter (new-10) |
| `owning_choice` / `copy_param_source` | **CRASH: heap-use-after-free** | as above (new-10) |
| `builtin_collection` / `copy_param_source` | **CRASH: double free** | as above (new-10) |
| `collection_literal` / `copy_param_source` | **CRASH: double free** | as above (new-10) |
| `task` / `copy_param_source` | CRASH: SEGV | #6194's family (new-10) |
| `object_class` / `await_using_var` | rejected, expected a run | §28.12's "takes §28.10's admission unchanged" (new-2) |
| `object_class` / `copy_await_source` | rejected, expected a run | as above, in the `await using` spelling (new-2) |
| `object_string` / `explicit_dispose_async` | compiled, expected rejection | §8.7 (`DisposeAsync` is a member only of a type that declares it) — new-5 |
| `collection_literal` / `using_var` | a §28.9 vacuity warning where none is due | #7092 / `s28_10_0016` |
| `collection_literal` / `await_using_var` | a §28.9 vacuity warning where none is due | as above |
| `collection_literal` / `copy_const_source` | compiled, expected rejection | §21.1 (a `const` initializer must not be an owned production) |
| `task` / `using_var` | run timeout (hang) | #6194 |
| `task` / `copy_const_source` | run timeout (hang) | #6194 |
| `task` / `field_store` | run timeout (hang) | #6194 |

**The KNOWN-RED cells on HEAD** (red, attributed to an open issue, not counted
wrong):

| cell | observed | issue |
|---|---|---|
| `value_choice` / `await_using_var` | LEAK 2100B/120 | **#7374** — the `await using var` copy of a value-like choice local leaks |
| `value_choice` / `copy_local_source` | LEAK 2100B/120 | **#7316** — the `using var` copy of a value-like choice local leaks (fixed on `b6053a68`, not yet in `origin/main`) |
| `value_choice` / `copy_const_source` | LEAK 2100B/120 | **#7316** — the same, with a `const` source |
| `value_choice` / `copy_await_source` | LEAK 2100B/120 | **#7374** — the `await using var` spelling of the copy |
| `value_choice` / `copy_param_source` | LEAK 2100B/120 | **#7357** — the parameter-source copy still leaks |

**The plain-`var` twins.** §28.10 makes the `using var` spelling's RUNTIME
LIFETIME the same as the plain-`var` twin's ("the only effect is the
reassignability restriction"), so the twin is the reference the grid measures
against. The grid measures it per head (`--twins`): every head's twin is CLEAN
except **`object_string`, whose twin LEAKS 2040B/120** — an `Object`-declared
slot holding a runtime-managed string does not release it. That is why the
`object_string` cells are the grid's only `TWIN` cells: the `using var`
spelling inherits the twin's retention and adds none of its own (the defect is
the `Object` slot's, not the `using var`'s — new-11). `generic_unconstrained`'s
twin leaks 32B/1 (the same generic-head leak as #3482).

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
| **#3493** | **RULED, owner-approved 2026-10-27** (verified in the issue's comments). *"Ruling: dispatch on the run-time object, through a `DisposeAsync` dispatch-table slot (this issue's option 2)."* The issue's own words: *"A trailing `DisposeAsync` dispatch entry covers this issue, #5653 (a `Base` that does not implement `IAsyncDisposable`) and #3483's interface shape, with one path instead of three static special cases."* **What it rules:** the *mechanism* of the run-time dispatch — a trailing `DisposeAsync` entry on the `IAsyncDisposable` itab (the #2506 shape), not a per-site static special case. **What it leaves open:** (a) the physical-reclamation half stays with **#3515**, whose two routes (a synthetic vtable slot `DisposeAsync$reclaim` vs a trailing itab entry) are recorded in that issue and are NOT chosen by the ruling; (b) the abstract-class head **#3516** stays gated on #3515; (c) **#7339** (a run-time STRING in an `Object`-typed slot) is not named by the ruling — it is a *value* dispatch, not a `DisposeAsync` dispatch, and needs its own answer; (d) the class-typed head's `DisposeAsync` is already dispatched through the vtable slot (`e9c564fd`), so the ruling's itab route is for the heads that have no vtable. | 2026-10-27 (already ruled) |

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
3. **Whether the `Type` token belongs in the anchor's runtime-managed set.**
   #7338 asks for it; §28.9 lists `Type` explicitly, so the answer is yes and no
   train is needed — but §28.12's `await using` sentence does not say what
   happens to a `Type` head, so a ruling would make it explicit. Recorded here so
   the coordinator can decide whether it is worth a train.

## Siblings filed during the campaign

*(none filed yet — this is the entry record; the rows below are the entry lane's
findings, which the coordinator files with exactly one `campaign:` label.)*

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
| new-10 | the `using var` copy of a PARAMETER DISPOSES the borrowed parameter — a use-after-free | `public static method Take(src: IAsyncDisposable) using var x := src; end Take;` called with `var src: IAsyncDisposable := new Res(); Take(src); src.Dispose();` → `DTOR` is printed by the CALLEE and the caller then reads freed memory: `ERROR: AddressSanitizer: heap-use-after-free ... freed by thread T0 here: #1 ... in bz$P.Program$Take` | §28.10 ("When the right-hand side of a `using var` declaration is a borrow ... the `using var` does **not** assume a disposal obligation ... at scope end, no `Dispose()` is called, because the value is not owned by this scope"), §28.2 | using-heads (U1c — a NEW soundness defect; the grid's `interface`, `owning_choice` and `builtin_collection` `copy_param_source` cells) |
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

## Exit review

*(to be written by a fresh reviewer who is not a campaign lane — master plan §2.4.)*
