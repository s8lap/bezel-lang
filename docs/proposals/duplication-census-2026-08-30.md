# A fresh duplication census — 2026-08-30

**Status:** audit, read-only. No `src/` change was made. **Base:** `main` @ `907ce0f`, spec
v0.286. Written by lane A3 of Run 9 (the deduplication close-out).

**Why this document exists.** `docs/proposals/imported-symbol-audit-2026-08-13.md` and
`docs/proposals/member-symbol-audit-2026-08-13.md` (with
`docs/proposals/imported-symbol-programme-status-2026-08-14.md` as the handoff) drove a large,
successful fix programme — but they are now 17 days and dozens of commits stale, and the
programme's own issue tracker reads as "complete" (every umbrella issue closed) in a way that
does not match the code. This document supersedes nothing normative; it is a working snapshot
for cycles 2+ of Run 9 to dispatch from, replacing guesswork with today's measurements.

**On authorship**: this document went through two independent write-ups in the same worktree
slot before this final pass (see §6's process note) — at least one other agent instance was
dispatched with the identical brief and landed in `.claude/lane-pool/lane-3` concurrently with
this one, and both filed near-duplicate GitHub issues before the collision was noticed and
cleaned up. This final version merges the two passes' content, adds the `LocalDecl`-only walker
family finding (hotspot 8 / issue #3591, absent from both earlier passes), and reconciles the
issue set to its current, deduplicated state. Treat the whole document as this run's single
lane-A3 output, not as competing drafts.

---

## 1. What the 2026-08-13/14 audits got wrong (or: why re-verify before dispatching)

**Their own headline claim — "reliably right that something was broken and reliably wrong about
the mechanism" — held up. Their PRECISION did not.** Re-walking the member-symbol audit's §6
(fifteen "drifted pairs"), its "comments that assert an invariant their own code violates" (seven
items), and its "always-null / dead code" list, against HEAD `907ce0f`:

| verdict | count of 15 §6 rows |
|---|---|
| **LANDED** (fully fixed since the audit) | **8** — rows 2, 3, 4, 5, 6, 11, 13, 14 |
| **STILL LIVE** (unchanged or worse) | **3** — rows 1, 9, 12 |
| **PARTIALLY LANDED** | **4** — rows 7, 8, 10, plus row 15 not fully re-derived |

**Six of the seven** "comments that assert an invariant their own code violates" are now
**LANDED** — either the comment was corrected to state the true invariant, or the underlying
code was fixed to match what the comment always claimed. **The seventh is STILL LIVE**:
`LlvmBackend_Setup.cpp:2523`, `// Only getters — properties with no getter (setter-only, §22.9)
skip.` sits directly above `auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl); if
(!pd) continue;` (verified by reading the code, not just grepping the comment text — an earlier
pass of this same census mistakenly marked this LANDED by only checking that the comment string
was unchanged, without checking what it labels). This is a decl-presence check mislabeled as a
getter-presence check, inside an exported-accessor-generation loop — an imported class's
property accessor generation silently `continue`s past every property (getter or not) instead
of only setter-only ones. This is a genuinely different picture from the
programme-status doc's "Wave F — ... currently unowned" framing (§3, that document): a
substantial fraction of Wave F's material was in fact absorbed into the M-family fixes (#3230,
#3189, #3194, #3176, #3141, #2931, #3152's CG33 slice) without ever being tracked as "Wave F"
work. **Do not re-import the "Wave F is untouched" framing into a new dispatch — verify each row
fresh, as this document does below.**

**Every M-family umbrella issue referenced by the programme-status doc's §6 "Open issues,
grouped" list is CLOSED** (`#3162 #2884 #3183 #3164 #3170 #3177 #3178 #3174 #3188 #3189 #3182
#3179 #3185 #3187 #3167 #3169 #3173 #3171 #3165 #3175 #3160 #3181 #3159 #3161 #3184 #3168 #3190
#2837 #3199 #3204 #3218 #3209 #3223 #3219 #3221` — checked via `gh issue view <N> --json state`
on 2026-08-30). **Closed-family ≠ every row fixed** is confirmed directly by this census: rows 1,
9, 12 of §6 are still live under closed umbrella issues, and rows 7, 8, 10 partially landed under
them. Treat every closed M-family issue as "the umbrella is closed, not every site is fixed" —
the same lesson the audits' own §4 recorded about themselves, now recurring one level up.

**One further correction, this time to a brief handed to THIS lane, not to the 2026-08-13
audits**: a claimed 6-site "AST-only `recordIsPositional` re-derivation" (§4 below, hotspot 6)
did not survive verification — 5 of 6 named sites operate on local-only AST walks where the
question of "imported vs local" cannot arise, and the 6th was already fixed by #3141. The real
finding (9 correct-but-duplicated re-derivations, not 6 broken ones) is filed as #3600 (after superseding an earlier, incorrect #3594 — see §6). **The
discipline that caught this — re-derive every count, don't trust a hand-off — applied to a
hand-off is worth stating explicitly, since it is easy to extend trust to a same-run
coordinator's brief that you would rightly withhold from a 17-day-old document.**

---

## 2. §6 drifted-pairs re-verification (member-symbol-audit-2026-08-13.md)

Each row re-measured against HEAD `907ce0f`. Recipe given where non-trivial; trivial `grep`
recipes omitted for space (see the commit history / issue numbers for the exact commands used).

| # | pair | 2026-08-13 verdict | **2026-08-30 verdict** | evidence |
|---|---|---|---|---|
| 1 | `recordMemberType`/`recordFieldType` (codegen) vs `storageComponentType` (sema) | both arms broken | **STILL LIVE** | `recordFieldType`'s gate `if (!fs->posParam && !fs->decl)` still excludes an imported *positional* field (non-null synthetic `posParam`, null `type`, #2516) from the `importedMemberType` fallback; the property arm still has no fallback at all. `storageComponentType` (Symbol.hpp:926, SymbolCollection.cpp:1051) is fully both-sources. Codegen's copy is the live gap. |
| 2 | `propertyHasSetter` (codegen) vs `propertyDeclHasSetter` (sema) — abstract Auto property | disagree | **LANDED** | `LlvmBackend_impl.hpp:3180-3210`'s comment: "added in this commit so the LOCAL path and the IMPORTED path answer identically" — both now exclude `abstract` from having a setter. |
| 3 | `Emitter::isAutoProperty`/`isAutoPropertyStorage` (codegen, no fallback) vs sema's twins | drifted | **LANDED** | The codegen functions are GONE — `grep -rn 'isAutoProperty\b\|isAutoPropertyStorage\b' src/codegen/` finds only comments. `LlvmBackend_impl.hpp:5107-5115`: "The definitions now live ONCE, in `sema/Symbol.hpp`" (fix #3152 CG33). |
| 4 | `sema::isAutoPropertySymbol` (15 callers) vs `isAutoPropertyStorageSymbol` (1) | fix landed, sweep didn't | **LANDED** | `grep -rho 'isAutoPropertySymbol' src/ \| wc -l` → **1** (a historical comment); `isAutoPropertyStorageSymbol` → **84**. `isAutoPropertySymbol` the function no longer exists. |
| 5 | `isOwnedTempOperand`'s two arms | 13 lines apart, one fixed | **LANDED** | `LlvmBackend_Exprs.cpp:7284-7310`, fix #3189 comment: "this arm is drift #5 of the audit... Both arms now go through the one both-sources accessor" (`sema::methodReturnsOwned`). |
| 6 | `Setup.cpp` `returnOwned` twins, 40/36 lines apart | one fixed, sibling not | **LANDED** | Both sites (`Setup.cpp:17293`, `:28363`) now call `sema::methodReturnsOwned`, fix #3189. |
| 7 | `buildRecordValue` (Exprs2.cpp) vs `emitOwnFieldInit` (Setup.cpp) initializer lookup | record copy lacks imported fallback | **PARTIALLY LANDED** | `buildRecordValue` (Exprs2.cpp:8005) gained a fallback, but a NARROWER one: `ctx_.importedConstValue`/`foldImportedConstText` — **constant-foldable initializers only** (comment at :8130-8146 cites issue #328). `emitOwnFieldInit` (Setup.cpp:10553) uses the FULL parsed-AST-expr replay (`importedInstanceFieldInitExpr`, #2874/#2524) — any initializer expression. A non-constant-foldable imported-record body initializer (`new Array()`, `DateTime.Now`) is still silently uninitialized; the comment calls this "a documented, pre-existing limitation, not newly regressed." |
| 8 | four copies of the const-fold resolver closure | diverged | **PARTIALLY LANDED** | The 4 CLASS-MEMBER-CONST closures the audit named (`TypeResolution_Checks.cpp:1012`, both `TypeResolution_Names.cpp` copies at `:1461`/`:1521`, `ExprTypecheck.cpp:9312`) ALL now carry the fix-#3230 `ConstMemberSymbol::intValue` fallback. But **two sibling closures the audit did not count** — `ExprTypecheck.cpp`'s `localConstDivByZeroTC` (:9385) and `foldLocalConstArraySize` (:9469), both resolving LOCAL const references — still lack any imported fallback. The family is 6 copies, not 4; #3230 fixed the 4 it targeted and missed 2 it didn't know about. |
| 9 | `propertyAccessRunsUserCode`'s call sites | 2 sites, one bare `if(pd)` | **STILL LIVE, and now 3 sites** | `ExprTypecheck_Exprs.cpp:2681` and `:3374` both gate on bare `if (auto* pd = ...) if (propertyAccessRunsUserCode(pd))` — null `pd` (imported) ⇒ narrowing NOT invalidated (unsound). Only the indexer site (`:3247`) is explicitly conservative (`pd ? f(pd) : true`, with a comment explaining why). Two unsound sites now, not one. |
| 10 | 5 copies of "is this member pure" in PureChecker.cpp | 3 read modifiers, 4 AST-only | **PARTIALLY LANDED** | The MEASURED M8 defect (pure ctor chaining to an imported impure base) is fixed: `callTargetIsPure`'s `ConstructorSymbol` arm now calls `constructorIsPure(cs)`, a both-sources accessor (fix #3174, comment at `PureChecker.cpp:260-263`: "This site was already both-sources; it is routed here so the chain-purity site below cannot drift away from it again"). `MemberSymbol::isPure`, however, is STILL import-only — `grep -n '\.isPure = ' src/sema/SymbolCollection.cpp` finds nothing; only `ModuleLoader.cpp` sets it (`:735`, `:4311`). The duplication persists (a redundant `->modifiers` loop covers most local call sites), but the audit's specific §4.3 claim ("assigned only in ModuleLoader... not a both-sources channel") is still accurate. |
| 11 | four copies of "find ctor by arity, read `params[i].modifier`" | 1 fixed, 3 open-code it | **LANDED** | `grep -n 'params.size() == nArgs' src/ownership/Ownership_Checks.cpp` finds only 2 hits, both inside COMMENTS describing the pre-fix history (`:3848`, `:13161`). `borrowSourceForExpr`, `scopedFormalAt`, `verifiedCallBoundedFormalAt` all now call `resolveCtorParams`. |
| 12 | `findIndexer` (filters `indexerCandidateWireable`) vs `classIndexerMatching` (does not) | this is why itable route is safe, vtable route isn't | **STILL LIVE, unchanged** | `LlvmBackend_Assign.cpp:162-181` (`findIndexer`) still calls `indexerCandidateWireable`; `LlvmBackend_impl.hpp:6804-6819` (`classIndexerMatching`) still does not. Byte-for-byte the same shape as 2026-08-13. |
| 13 | `resolveMemberAccess` (imported arm) vs `resolveNullCondMember` (no imported arm) | MEASURED: `.`→42, `?.`→1 | **LANDED — RE-MEASURED** | Rebuilt `bezelc` from HEAD, re-ran the ORIGINAL audit probe files (`/var/tmp/bezel-scratch/member-audit/probes/m13.{lib,app,local}.bzl`, unmodified): single-module `h?.N ?? 99` → **42**; two-module (`--import`) → **42**. Previously 1. |
| 14 | `checkArgsAgainstParams` (local) vs `checkImportedCallArity` (imported, "silently returns on any named argument") | one function, two answers | **LANDED** | `checkImportedCallArity` (`ExprTypecheck_Calls.cpp:10669`) now calls `bindArgsToParams` — the shared §22.5 binder, which explicitly tracks `sawNamed` and validates named-argument binding for both local and imported callees. |
| 15 | free-var scan: `typeNamesFreeVar` (walks `ast::Type`) vs `extSignatureUsesFreeVar` (walks `ast::MethodDecl`) | same question, 40 lines apart, different AST shape | **NOT FULLY RE-DERIVED — flagged STILL LIVE by inspection** | Both functions still exist as described (`ExprTypecheck.cpp:7386-7431`). `extSignatureUsesFreeVar` still takes `const ast::MethodDecl* md` directly and would be null for an imported extension method's `MethodSymbol::decl`. I did not verify the caller-side guard or confirm reachability for an imported extension target — a fix lane should re-derive this one from scratch rather than trust either verdict. |

### The "always-null / dead code" item

`Setup.cpp:5827`'s original citation (`buildImportedGenericVtable`, a `dynamic_cast<PropertyDecl*>
(pimpl->decl)` refusal gate claimed dead because `PropertySymbol::decl` is never set on import) —
**DISPUTED, not settled here.** The function has grown substantially (now spans
`LlvmBackend_Setup.cpp:6450-~6950`); I found a structurally similar gate at `:6819-6820` inside
an `exporterDefinesAccessor` lambda, but its CURRENT comment (`:6811-6815`) argues the opposite of
the original claim — that a local-body indexer genuinely CAN reach `pimpl` here (a local override
atop an imported base) and the refusal is a "true mirror rather than defensiveness." This
contradicts the audit's "fails open, dead code" framing. **Needs a two-module probe to settle**
(construct a local class overriding an imported generic base's indexer, instantiate the derived
generic, and check whether `pimpl` is ever local at this call site) — not done here for time.
Do not carry forward either verdict without re-measuring.

### Dead channels (member-audit §4.3)

- **`MemberSymbol::contractRecords`** — STILL a dead channel. `grep -rn '\.contractRecords\b\|->contractRecords\b' src/` finds only the two writes (`ModuleLoader.cpp:736`, `:4312`) and the field declaration (`Symbol.hpp:593`); zero reads. (Distinct from `contractRecordsCarried`, a different bool field that IS read.) An imported member's §22.16 contracts cross the boundary and are consulted by nothing.
- **`ctx.importedEventIsStatic`** / **`ctx.importedGenericConstraints`** — not re-verified this pass; treat as unconfirmed.

---

## 3. Programme-status doc (2026-08-14) — wave residuals

Per the doc's §3 "the plan, in order": M9 (Wave C) and the M2/M6/M13/M4/M8/M5-adjacent fixes
(#3189/#3194/etc.) are the ones this census independently re-touched via §2 above — confirmed
landed or partially landed as detailed there. Wave D (M7, M14) and Wave E (the schema tail) were
**not** re-verified in this pass — the doc's own issue list for them (#3162/#2884 blockers, the
M7/M14 conversions) all show CLOSED on `gh issue view`, but per §1's general warning, closed ≠
fully landed and these need their own fresh probe pass before being trusted, which this census
did not have budget for. Flag as **UNVERIFIED, re-check before dispatching a fix lane against
Wave D/E claims specifically.**

---

## 4. Hotspots for cycles 2+ (measured today, with recipe)

### Hotspot 1 — await-site containment walk (issue **#3587**)
4 independent expression-level copies (`Emitter::exprContainsAwait`, `countAwaitsInExpr` —
`LlvmBackend_Async.cpp`; `MethodOwnership::exprContainsAwait` — `Ownership_Checks.cpp`;
`recordAwaitsInExpr` — `CFG.cpp`), covering 16-17 of 35 `ast::*Expr` kinds each, plus ~5
statement-level siblings. **Behaviour DOES currently disagree** on `ast::BindExpr` (only
`CFG.cpp` handles it) but the disagreement is **inert** — sema rejects `await` inside a binding
atom outright (#2122), verified by direct probe (`bindawait.bzl`, loud reject at compile time).
Landmine, not live. Separability: moderate — a shared visitor with a callback per leaf action
(dispose-check / count / record) would collapse all 4; the ~5 statement-level siblings are a
second, smaller unification.

### Hotspot 2 — §28.9 trivial-dispose classification (issue **#3588**)
4 copies, ~470 lines (`common::isNonTrivialDisposeClass` 121L, `PureChecker::isTrivialDisposeClass`
88L, `ExprTypecheck.cpp`'s inline `trivialDisposeProducer` walk ~200L,
`Ownership.cpp::rhsIsDisposableClassType` 62L, self-described as "COARSER"). **The fourth copy's
own commit message says a fix to the shared copy did NOT propagate** — measured, not assumed
(fix #3191's comment). Separability: hard — `rhsIsDisposableClassType`'s coarser relationship
needs to become an explicit, documented narrowing rather than a fifth body; §28.9 v0.237's third
disqualifier (owning inline arrays) has already drifted once (#2666) between these four.

### Hotspot 3 — LINQ operator name list (issue **#3589**, extends open #3444)
10 hand-spellings, 6 of them 37-44 names. **Behaviour DISAGREES**: measured the 44-name set
(`ExprTypecheck_Calls.cpp:5263`) against the 37-name set (`:16970`) — 9 names only in the
44-set, 2 only in the 37-set (`First`/`Last` — real LINQ operators, missing from the 44-set).
Two independently-recorded `MinOrNull`/`MaxOrNull` transcription gaps already found in this
family (comment at `:17270-17277`). Separability: needs care — collapsing before reconciling
against stdlib §5/§5.1 could silently change acceptance at up to 9 call sites; the divergent
44-vs-37 split may encode a deliberate narrower-subset decision at some call sites that a naive
union would erase.

### Hotspot 4 — `checkImportedCallArity` (LANDED, no action)
Previously drifted from `checkArgsAgainstParams`; now both route through `bindArgsToParams`.
Confirmed no residual gap in named-argument handling.

### Hotspot 5 — `MemberSymbol::isPure` import-only channel
`isPure` is set ONLY by `ModuleLoader.cpp` (`:735`, `:4311`); `SymbolCollection.cpp` never sets
it for a local declaration. `callTargetIsPure`'s redundant `->modifiers` loop currently covers
most call sites, so this is not measured as live-broken, but it is exactly the "populate it
locally too" fix the member-audit's §4.3 recommended and never landed. Low urgency; good
small-scope pick (one accessor, one write site) for a future cycle.

### Hotspot 6 — `recordIsPositional` re-derivation (issue **#3600**, REVISED from original brief)
Canonical accessor: `Emitter::recordIsPositional` (`LlvmBackend_impl.hpp:6072`), whose header
comment documents a MEASURED cross-module `ToString` miscompile (f1175c6). **A brief describing
6 "AST-only, answer-no-for-import" sites did NOT hold up** — 5 of 6 operate on guaranteed-local
AST walks (per-module top-declaration/serialization-eligibility visitors that never see an
imported type's node), the 6th was already fixed by #3141. What DOES remain: **~9 correct,
independent re-derivations of the same both-sources logic** (`PureChecker.cpp` x2,
`SymbolCollection.cpp` x3, `ExprTypecheck_Exprs.cpp`, `ExprTypecheck_Calls.cpp` x2) — currently
harmless because each one happens to check the symbol-level field first, but exactly the shape
that produced f1175c6 the one time a copy got the order wrong. Separability: easy — promote the
accessor into `sema/Symbol.hpp`, route ~9 call sites through it. Low urgency (nothing broken
today) but cheap.

### Hotspot 7 — three dead write-only maps + `contractRecords` + `isPure` (issue **#3579**, extended)
`ensuresResultSymbol_` (never read anywhere), `genericInstInitStateGV_` /
`genericInstInitExcGV_` / `genericInstInitOwnerGV_` (written at 3 consecutive lines,
`Setup.cpp:1788-1790`, read nowhere) added as a comment to #3579, which already tracks
`smIterFinallyAtState_`/`smIterDisposalsAtState_`/`smIterEnumFramesAtState_`. `contractRecords`
(§2 above) is a further, unfiled dead channel of the same shape — smaller scope than #3579's
sweep, worth adding there too rather than a fifth issue.

### Hotspot 8 — the `LocalDecl`-only walker family has NO ratchet (issue **#3591**)
A different SHAPE of duplication than hotspots 1-6 above (not "N copies answer the same
question," but "N copies each independently forget the same second case"): §21.1's runtime
`const` local is `ast::LocalConstDecl`, a DIFFERENT AST node from `ast::LocalDecl` (the `var`
form), with differently-named fields. A statement walker spelled
`dynamic_cast<const ast::LocalDecl*>(s)` with no sibling `LocalConstDecl` arm silently skips
every `const` local — not a null-deref, a silent no-op for a whole binding form. Measured
today: **105** such casts in `src/`, of which **76** have no `LocalConstDecl` mention within
±12 lines either side (recipe: per-file line scan, window = 12 lines each side, `bare` if
`'LocalConstDecl' not in window` — an EXPOSURE UPPER BOUND, not a defect count; each bare site
needs individual triage). Per-file: `Ownership_Checks.cpp` 22, `LlvmBackend_Setup.cpp` 16,
`Ownership.cpp` 11, `LlvmBackend_Async.cpp` 5, `LlvmBackend_Iterators.cpp` 4, then single digits
across 12 more files. **Six prior instances of exactly this shape have already been found and
fixed one at a time**, each its own issue, each fixing only its own consumer: #2802, #3375,
#3402, #3416, #3417 (CLOSED), #3427 (OPEN, most recent, itself a follow-up on #3402's own
"not applicable" rationale being wrong for three more shapes). The fix pattern is recorded
in-source: `LlvmBackend_Setup.cpp:24776` (#2802's comment) — widen the candidate vector from
`const ast::LocalDecl*` to `const ast::Node*`; a second site (`:23682-23689`) self-counts as
*"the FIFTH instance of that LocalDecl-only pattern."* **Unlike this census's other hotspots,
this family has three living siblings WITH gate-enforced ratchets**
(`scripts/check_imported_symbol_reads.py`, `check_enumerable_grid_spellings.py`,
`check_deprecated_jit_oracle.py`, all confirmed present and wired into `gate_full_cycle.sh`)
**and this one has none** — nothing currently stops a 7th/8th/9th instance from being written
tomorrow. Separability: N/A (this is a detector to write, not a call site to unify) —
recommended as a `check_localdecl_only_walkers.py` modelled on the three existing scripts, with
a checked-in exact-match baseline (not a ceiling, per the deprecated-JIT-oracle ratchet's own
"count below baseline fails as stale headroom" rule).

---

## 5. Do NOT touch — deliberately load-bearing duplication

- **`LlvmBackend_Setup.cpp` (~line 15650-15730 region) and `LlvmBackend_Calls2.cpp`'s
  `GetAsyncEnumerator` lowering** carry a matched self-alias guard for `await using var en :=
  src.GetAsyncEnumerator()` where `src` is a named local of an owned-param async-iterator type
  (fix #2356/#2485). The two sides handle the NAMED-receiver case (Setup.cpp, scope-end
  disposal collision) and the UNBOUND-temporary case (Calls2.cpp, inline disposal) respectively
  — unifying them without preserving both classification arms turns a correctly-guarded
  double-free-prevention into a real double free. (The original brief cited exact lines
  `Setup.cpp:15482` / `Calls2.cpp:7905`; I confirmed the GENERAL pairing and its rationale but
  not those exact two line numbers — re-locate by searching both files for `GetAsyncEnumerator`
  before touching either.)
- **`LlvmBackend_Calls2.cpp:10969-11060`** (fix #2821, §14.1/#783 soundness gate) — a decl-cast
  check here is not incidental AST-only-ness; decl-*presence* (whether an imported generic
  method's body could be reconstructed importer-side at all) is genuinely the predicate's
  subject. (Original brief cited `:9256/:9377/:9922`; current line numbers are `:10969-11060` —
  file has grown since the brief was written. Same finding, re-verify the exact lines before
  acting.)
- **`isArrayBackedCollectionName`** (`BuiltinRegistry.hpp:133`, `bz_array_t*`-backed receivers,
  includes `Set`/`OwningSet`/`SortedSet`/`FrozenArray`) vs **`isArrayEnumeratorWrapSourceName`**
  (`BuiltinRegistry.hpp:107-124`, the dense-array-backing subset for the §29.2 enumerator wrap
  decision, explicitly EXCLUDES `Set`/`SortedSet`). The header comment states the reason
  directly: a `Set<T>` over a value/record/string element is a hash table (snapshotted, not
  wrapped); `SortedSet<T>` is excluded because the snapshot is unordered and would silently
  break stdlib §4.18's ordered-iteration contract. Two different questions over overlapping name
  sets — collapsing them would either wrap a hash-table receiver as if it were a dense array
  (crash) or silently unorder a `SortedSet` iteration (spec violation).

---

## 6. Issues — filed and cross-referenced

⚠ **Process note.** A mid-session context loss caused this census to be produced (and this
section written) twice, independently, without the second pass knowing about the first. The
first pass (commit `be374c3`) had already discovered that a SIBLING Run 9 lane filed three of
these five findings ~10 hours earlier (#3587 await-site, #3588 §28.9, #3589 LINQ) and
cross-referenced them instead of duplicating; it also filed #3594 for the `recordIsPositional`
finding using the SAME wrong 6-site framing this document's §1 flags, then a later pass refuted
its own #3594 and filed #3600 in its place. The second pass (this one, unaware of any of that)
independently re-derived the same four findings from scratch and briefly created duplicate
issues #3595/#3596/#3598 (now closed in favor of #3587/#3588/#3589) and a duplicate comment on
#3579 (now deleted). The table below reflects the SURVIVING, correct issue set after that
cleanup.

| # | title | verdict |
|---|---|---|
| [#3587](../../issues/3587) | await-site walk is 4 copies, BindExpr-drifted | latent landmine, not live — independently confirmed by both census passes |
| [#3588](../../issues/3588) | §28.9 trivial-dispose is a self-documented 4-copy family | live duplication, one measured propagation failure |
| [#3589](../../issues/3589) | LINQ operator name list, 10 spellings, 44≠37 sets | live divergence, extends open #3444 |
| [#3600](../../issues/3600) | `recordIsPositional` re-derived ~9x (all correct today) | REVISED from a 6-site "broken" claim (both this census's own §1 and the now-closed #3594 made the same mistake); filed as the corrected, narrower finding |
| [#3579](../../issues/3579) (comment) | 4 more write-only dead members added to existing dead-map issue | extends scope, not a new issue — added once, a duplicate follow-up comment from the second pass was deleted |
| [#3591](../../issues/3591) | `LocalDecl`-only walker family (76 bare sites measured) has NO ratchet, unlike 3 sibling duplication families | tooling gap, not a call-site unification — see hotspot 8 above |

**Also closed as duplicates during cleanup** (same findings, filed by other agent instances that
landed in this same worktree slot within minutes of each other — see the process note above):
#3592/#3595 (await-site, → #3587), #3593/#3596 (§28.9 dispose, → #3588), #3594 (recordIsPositional,
refuted, → #3600), #3598 (LINQ list, → #3589). **Unrelated findings from the same collision,
outside this census's scope but worth the coordinator's awareness**: #3590 (tooling: seam
ratchets raisable by `--force`), #3597 (`checkPureBodies` one-level-nested walk escapes §22.15 at
depth 2), #3599 (`collectNested` stops at a non-class nested type, §19.6 leaf-visibility escape),
#3601 (§10.5 enum restrictions never reach a nested enum) — all filed 2026-08-29 17:13-17:15,
genuinely new bugs, not duplication-census material.

---

## 7. Ranked shortlist for cycles 2+ (confidence noted)

1. **Row 1 (recordFieldType/recordMemberType import fallback gap)** — HIGH confidence, live,
   measured gate logic unchanged since 2026-08-13. Concrete: an imported positional field's type
   is still unrecoverable through the field arm; the property arm has no fallback at all. Likely
   silent-wrong-data or a null-deref downstream. Good first pick — narrow, well-understood.
2. **Row 12 (`classIndexerMatching` missing `indexerCandidateWireable` filter)** — HIGH
   confidence, live, unchanged. The audit already explains why this matters ("precisely why the
   itable route is safe and the vtable route is not") — worth a probe to determine if the vtable
   route is currently reachable with an unwireable indexer candidate.
3. **Row 9 (`propertyAccessRunsUserCode` unsound narrowing, now 2 of 3 sites)** — MEDIUM-HIGH
   confidence. Not memory-unsafe (a missed narrowing invalidation is a correctness/soundness bug
   in the type-narrowing sense, not a crash), but got WORSE since 2026-08-13 (one more unsound
   site than previously described), suggesting active development is adding sites without
   noticing the pattern.
4. **Issue #3588 (§28.9 four-copy family)** — HIGH confidence this is real and worth unifying;
   HIGH difficulty (470 lines, self-documented propagation failure, needs the full §6b exclusion
   diff before landing). Large but well-scoped — good candidate for a dedicated multi-fix lane
   rather than a quick pick.
5. **Row 8 residual (2 local-const-fold closures missing the #3230 fallback)** — MEDIUM
   confidence on reachability (needs a probe: does a local `const`'s initializer or a
   `[N]T` array size ever legally reference an imported class-member const through
   `lookupName`'s scope-chain path, as opposed to the already-fixed `ts->members`/`baseTypes`
   walk?) but LOW difficulty if reachable — the fix is copy-pasting fix #3230's fallback into
   two more closures.
6. **Row 7 residual (buildRecordValue's narrower constant-foldable-only imported fallback)** —
   MEDIUM confidence this is worth closing (the gap is explicitly documented as a known,
   accepted limitation, not a silent regression) but a real gap: a non-foldable imported-record
   body initializer is silently zero. Scope: extend `buildRecordValue`'s fallback to also try
   `importedInstanceFieldInitExpr` the way `emitOwnFieldInit` does, for the case
   `foldImportedConstText` returns null.
7. **Issue #3589 (LINQ name list, 10 copies)** — MEDIUM confidence on the unify-worthiness (real
   drift, but reconciling 44-vs-37 against stdlib §5/§5.1 first is real work and a mis-union
   could introduce a false accept/reject at any of ~9 call sites). Do the stdlib reconciliation
   as its own step before touching any call site.
8. **Row 15 (free-var scan, `ast::Type` vs `ast::MethodDecl`)** — LOW confidence, not
   independently re-derived this pass. Needs its own verification pass before it's picked.
9. **The `Setup.cpp:5827`-area dead-code dispute (buildImportedGenericVtable)** — LOW confidence
   pending a probe; genuinely disputed between the 2026-08-13 audit's "dead, fails open" claim
   and the current comment's "true mirror, reachable" claim. Worth 20 minutes with a two-module
   local-override-of-imported-generic-indexer probe before doing anything else with it.
10. **Issue #3600 (`recordIsPositional` ~9x)** — HIGH confidence this is real duplication, LOW
    urgency (nothing broken today) — good "cheap, structural, retires a family" pick when the
    queue needs a fast win between two harder items.
11. **Issue #3591 (`LocalDecl`-only walker family — write the fourth ratchet)** — HIGHEST
    confidence of everything in this list, because it is a detector script (zero `src/` risk),
    not a behavior change, and its fix pattern is already proven six times over (#2802, #3375,
    #3402, #3416, #3417, #3427). Arguably the single best next pick in this whole census: unlike
    hotspots 1-6/10 above (which each retire ONE family of duplicate call sites), this prevents
    the NEXT six leaks instead of fixing one more — and it is the one duplication family in this
    run with measured population (105/76) and zero ratchet coverage, where its three sibling
    families all have gate-enforced scripts already.
