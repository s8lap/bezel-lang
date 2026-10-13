# The `MemberSymbol` half of the imported-symbol seam — a complete triage

**Status:** audit, read-only. No `src/` change was made.
**Base:** `main` @ `137f58f`, spec v0.252. **Umbrella:** #3152. **This half:** #3149.
**Companion:** `docs/proposals/papers/imported-symbol-audit-2026-08-13.md` (the `ast::TypeDecl` half).

⚠ **Provenance is marked per row.** The `ast::TypeDecl` audit's §5 table was later found **wrong
in six rows** (#3168) — sites it recorded as broken were already both-sources, and two of those
pairs had silently *drifted*, which was the more interesting finding. So this document separates
what was **MEASURED** with a real two-module build from what is **reading-derived**.

- **§2 is measured.** Ten two-module probes, `build/bezelc -c lib.bzl -o lib.o` then
  `--import lib.o app.bzl`, each against a character-identical single-module twin. Sources in
  `/var/tmp/bezel-scratch/member-audit/probes/`.
- **Appendix A's per-site table is reading-derived** unless a row is tagged **[M]**. Treat it as
  a work list, not as findings. **A fix lane verifies first** (lane contract §6).

---

## 1. Headline

| verdict | count | meaning |
|---|---|---|
| **UNIFY** | **110** | reads a fact the member symbol carries, or could carry. **These are the bugs.** |
| **NEEDS-AST** | **34** | dereferences a statement body, an initializer expression, or an argument list. Leave alone. |
| **ALREADY-CORRECT** | **206** | has a symbol-side fallback, or no imported symbol reaches it. |
| *not a site* | **10** | the grep matched prose inside a comment. |

**360 sites**, measured by balanced-paren parse of every
`dynamic_cast<[const] ast::{Property,Method,Field,Constructor,Destructor,Event,Const,Operator,Extension}Decl*>`
whose argument ends in `->decl` / `.decl`. (`LocalDecl` 34 + `LocalConstDecl` 4 are excluded:
locals are never imported.)

### What this refutes in #3149's framing

| #3149 says | measured on this base | note |
|---|---|---|
| "~570 casts" | **471** same-line `.decl` casts of any AST node; **398** by balanced-paren parse; **360** member-kind | the issue's `grep -c` counted the *cast* lines, not the operands |
| "`TypeDecl` ~159" | **77** by the same parse (**94** total `TypeDecl` casts) | the type half landed 10 of 12 families this run |
| "`isAutoPropertySymbol` … 31 call sites" | **16** live callers in `src/` (+ decl + definition = 18) | 31 counts `src/` + `tests/` (51 total occurrences) |
| "five ad-hoc loader side tables" | **eleven** | `returnTypes_`, `returnOwned_`, `fieldTypes_`, `methodGenericConstraints_`, `propHasSetter_`, `propSetterVisibility_`, `propIsAutoStorage_`, `propHasGetter_`, `eventIsStatic_`, `eventIsAsync_`, `constValues_` |
| "Fix `isAutoPropertySymbol` first" | **already half-done, and stalled there** | the both-sources twin `isAutoPropertyStorageSymbol` landed this run and has **1** consumer; the AST-only original still has **15** |
| "`MemberSymbol::decl` set in 4 places" | **five** — `ExtensionSymbol::decl` is also set (`ModuleLoader.cpp:2773`) | that fifth is what makes 5 sites ALREADY-CORRECT |

**The last row is the important one.** The recommended first fix has landed as an accessor and
was applied at one site out of sixteen. Per CLAUDE.md's #1462 warning, that partial state is
the *dangerous* one, not a safe waypoint.

### Two refutations that change the priority order

**(a) Site count badly overstates the property family.** #3149 leads with `PropertyDecl` at 96
casts. But #2400 / #2415 / #2461 / #2478 already carried the accessor facts, and **probe M4
measured that an imported computed property's setter is honoured correctly** across the
boundary. Of the 104 `PropertyDecl` sites, most are ALREADY-CORRECT. The property *count* is
the largest; the property *defect rate* is not.

**(b) Several of the highest-severity codegen sites are unreachable today, behind loud rejects.**
Probes M5 and M9 both died on `not yet supported` diagnostics (#3162 imported primary-ctor
construction; #2884 imported generic property access) *before* reaching the site under test. A
large part of the codegen UNIFY set is **latent**, gated behind an imported-generic path that is
itself refused. This did not arise in the type half, because a `TypeSymbol` is reachable by many
more paths than a member symbol is. **A lane must confirm reachability before writing the fix**,
or it will write a regression test that cannot fail.

That said, the *sema and ownership* families are emphatically **not** latent — §2 measures four
of them crashing or producing wrong values on ordinary, non-generic imports.

---

## 2. MEASURED — ten two-module probes

Every row is `build/bezelc -c lib.bzl -o lib.o` then `--import lib.o app.bzl`, against a
single-module twin that differs only in the module boundary.

| # | what | single-module twin | imported | verdict |
|---|---|---|---|---|
| **M11** | unmarked call to an `owned` formal | **rejected**, §22.3 "this call consumes `r`" | **compiles, SIGSEGV; ASan: `heap-use-after-free`** | ownership unsoundness → **#3176** |
| **M7** | `pure constructor` chaining to an impure base ctor | **rejected**, §22.15 chain-purity | **compiles clean, exit 0** | §22.15 false accept → **#3174** |
| **M1** | `=` on a body record whose auto-property is class-typed | **rejected**, §9.2 "not auto-hashable" | **compiles and runs** | §9.2 false accept |
| **M2** | the same, one level of nesting out (local record, imported component) | **rejected**, §9.2 | §9.2 diagnostic **absent** (build then failed on unrelated #1386) | the wrong answer propagates into a LOCAL type |
| **M3** | `ToString()` of that record | `M3Local.BadM3L` (qualified name — §8.5 correct) | **`BadM3(R: DepM3.ResM3)`** | silent wrong DATA at runtime, no diagnostic |
| **M13** | `(h?.N ?? 99)` on an imported auto-property | **42** | **1** | silent wrong DATA → **#3177** |
| **M12** | §22.6 overload ranking, `Pick("s")` | **2** | **rejected**, §5.1 conversion error | false reject → **#3178** |
| **M14** | imported optional parameter, `WithDefault(1)` | **8** | **rejected**, "supplies 1 argument(s) but the parameter list has 2" (span `0:0`) | false reject → **#3178** |
| **M4** | assignment through an imported *computed* property setter | `42` / `3` | `42` / `3` | **ALREADY-CORRECT** — the #2400/#2415 side tables work |
| **M8** | `pure` body inside an `extension` block (single-module) | class-method twin **rejected**, §22.15 | extension form **compiles, binary produced** | §22.15 never checked → **#3175** |

### Attempted and inconclusive — recorded so it is not re-derived

- **M5** (`owned` primary-ctor parameter of an imported class, expecting a capture-slot leak):
  blocked by #3162's loud reject of imported primary-ctor construction. The gap is real in the
  data (§4.2) but **not reachable today**.
- **M9** (imported generic class's `string` auto-property, expecting the disposer leak):
  blocked by #2884's loud reject of imported-generic property access.
- **M15** (`for var x in importedEnumerable`): the **single-module twin fails identically**
  (`not yet implemented: codegen for this for statement`), so the probe does not isolate the
  module boundary. Group F's Stmts:7175 claim is neither confirmed nor refuted here; a lane
  needs a `for-in` shape that works locally first.

---

## 3. The root cause, precisely

`ModuleLoader` assigns `sema::MemberSymbol::decl` in exactly **four** places, and only when the
owning type is GENERIC and its body source text was exported (schema v5 body re-parse):

| ModuleLoader.cpp | member |
|---|---|
| `:1491` | constructor (`importedCs->decl = parsedCd`) |
| `:1713` | destructor |
| `:2031` | method |
| `:2409` | **static** field only |

Plus a fifth, on a different symbol class: `ExtensionSymbol::decl = decl.get()` (`:2773`) — the
loader synthesizes an `ast::ExtensionDecl` for an imported `extension` block. #3149's table omits
it; it is what makes five sites in `ExprTypecheck*.cpp` correct. Note the asymmetry: the block
gets a `decl`, its **member** `MethodSymbol`s get `extensionDecl` and **not** `decl`. One object,
two decl channels, opposite answers — which is exactly why `ExprTypecheck.cpp:6843` is broken.

For a **non-generic** imported type every member symbol has `decl == nullptr`.
`PropertySymbol::decl` (including indexers) is **never** set on any path.
`EnumConstantSymbol`, `EventSymbol`, `ConstMemberSymbol` and instance `FieldSymbol` decls are
never set either.

So the cast is not a null-deref risk — every site guards. It is a **silent answer change**: the
`false` branch is written as "not an auto-property" / "no `owned` formal" / "not `abstract`" /
"returns nothing owned", and each of those is a *substantive claim* that is simply wrong across
a module boundary.

The 2026-08-06 taxonomy's three shapes are all present, and the third dominates here:

- **positive-list missing** → loud reject (M5: `checkImportedCallArity`'s exact-arity rule).
- **negative-list "not X ⇒ assume Z"** → silent accept (M1: `!decl → not an owned sink`).
- **a correct predicate fed by an empty source** → invisible (M3: `isAutoPropertySymbol` returns
  `false` for every imported property, so `isAutoEqualityBearingBodyRecord` inspects *no*
  components and returns `true` — measured, §2/M1).

---

## 4. What crosses the boundary today

### 4.1 Already carried — a site reaching for these is UNIFY, not blocked

`SemaContext` publishes, keyed by the member symbol: `importedReturnType`, `importedReturnOwned`,
`importedMemberType`, `importedGenericConstraints`, `importedPropertyHasSetter`,
`importedPropertySetterVisibility`, `importedPropertyIsAutoStorage`, `importedPropertyHasGetter`,
`importedEventIsStatic`, `importedEventIsAsync`, `importedConstValue`,
`importedInstanceFieldInitText`, `importedInstanceFieldInitExpr`.

On the symbols: `MemberSymbol::{visibility, modifiers, parent, isPure, contractRecords}`,
`MethodSymbol::{genericParams, params, isOperator, operatorToken, extensionDecl}`,
`ConstructorSymbol::{params, isStatic}`, `PropertySymbol::{isIndexer, indexerParams}`,
`FieldSymbol::{posParam, posParamBodyRef, posParamInitRef}`, `ParameterSymbol::{modifier,
name, resolvedDeclType}`, `EnumConstantSymbol::{value, serializedName}`.

**Ninety-four of the 110 UNIFY sites need nothing new.** They need an accessor call.

### 4.2 Genuinely missing — the schema-blocked set

| fact | needed by | why it is missing |
|---|---|---|
| a positional field's **`owned` marker** | `OwnershipFacts.cpp:49`, `Sendability.cpp:209`, `TypeResolution_Checks.cpp:7930`, `PureChecker.cpp:156`, `LlvmBackend_impl.hpp:8083` | `MetaMember` has no field for it; the builder's positional branch writes only `type`/`isPositional`; `makeImportedParam()` returns a default `ast::Param` whose `modifier` is `""`. **LATENT** — §9.7 forbids an `owned` record field and the class primary-ctor path is refused by #3162. Already filed as #3164 |
| `serializedWireName` / `serializedSkip` / `serializedOptional` for a field or property | `LlvmBackend_Serialization.cpp:156, :182` | no wire field. The `EnumConstantSymbol::serializedName` precedent (#3140) is the exact shape |
| an `owned` bit for a §9.8 **component** | `Serialization.cpp:156` back-edge classification | same |
| `ParameterSymbol::hasDefault` | `checkImportedCallArity:9659`, `resolveOverload:1077/1085/1103`, `impl.hpp:9780` | **on the wire already** (`MetaParam::hasDefault`/`defaultText`, decoded at `ModuleMetadata.cpp:1136`) and **dropped by `ModuleLoader::convertParams`** — `grep hasDefault src/module/ModuleLoader.cpp` returns nothing. No format change needed |
| a typed integer value on `ConstMemberSymbol` | `ExprTypecheck.cpp:7954/7981/8001/8085`, `TypeResolution_Names.cpp:1409/1442` | `importedConstValue` carries *text* only. `EnumConstantSymbol::value` is the template |
| "declared bodyless" (`MethodBodyKind::Abstract` with no modifier) | `DefiniteAssignment.cpp:89` | no wire bit; narrow residue |
| "the body contains `yield`" | `LlvmBackend_Calls2.cpp:8442, :8514` | unanswerable importer-side for a method whose body text was not exported |
| a STATIC property's Auto body form | `LlvmBackend_impl.hpp:4510` | the bit exists (`MetaMember::isAutoProperty`) but `ModuleMetadataBuilder.cpp:1116-1118` **force-clears it for any `static` member** — the fact is deliberately destroyed on the wire |
| the leading `base(...)` chain-call argument texts | `LlvmBackend_Exprs2.cpp:2695` | argument **expressions** — genuinely NEEDS-AST unless carried as text |

### 4.3 Carried and never read — dead channels

- **`MemberSymbol::contractRecords`** (#2054) is written at `ModuleLoader.cpp:407` and `:2790`
  and **read nowhere in `src/`** (verified: the other `grep` hits are `contractRecordsCarried`, a
  different field on `MetaTypeDecl`). An imported member's §22.16 contracts cross the boundary,
  land on the symbol, and are consulted by nothing.
- **`MemberSymbol::isPure`** is assigned **only** in `ModuleLoader`. `SymbolCollection` never
  sets it, so it is an *imported-only* channel, not a both-sources one. Any unification must
  populate it locally too — that alone would collapse the five drifted copies of "is this member
  pure" into one field read.
- **`ctx.importedEventIsStatic`** (#2421) is read only by codegen. The two sema sites asking the
  identical question (`ExprTypecheck_Exprs.cpp:5632`, `ExprTypecheck.cpp:11537`) still read
  `ed->isStatic`.
- **`ctx.importedGenericConstraints`** (#2391) is read only by `checkOverrideSignatureMatch`,
  never by the call-site §14.2 constraint check.

---

## 5. The pattern that works — reuse these, do not write a twelfth side table

Eleven loader side tables exist, each added by its own bug, none making the next consumer
correct. **A twelfth is the option that has already failed eleven times.** Where a fact is
genuinely absent (§4.2), the shape is: a `MetaMember` field + a symbol field + **one**
both-sources accessor. The templates:

- **`sema::storageComponentType`** (`Symbol.hpp`, un-`static`'d by #3145) — positional `Param`
  type, then body `FieldDecl`/`PropertyDecl` type, then `ctx.importedMemberType`. Its own header
  records why it was made shared: being file-local *is* why two more copies grew elsewhere.
- **`sema::isAutoPropertyStorageSymbol`** — the both-sources twin of `isAutoPropertySymbol`.
- **`sema::formalParamModifier` / `formalParamName` / `formalParamCount`** (#2659/#2374) — the
  `CtorFormals` shape: AST formals when there are any, `ParameterSymbol` otherwise.
- **`sema::anyRecordStorageField` / `recordPositionalFieldSymbols`**, `sema::enumDeclaredValues` /
  `enumConstantWireName`, `sema::builtinExceptionBaseOf`, `src/sema/GenericParams.hpp`.

### Three rules for the sweep

⚠ **Do not partially unify.** #1462 attempted exactly this kind of centralisation partially and
**turned a leak into a SIGSEGV**; it had to be reverted. A family lands whole. This audit found
the partial state already present twice: `isAutoPropertyStorageSymbol` at 1 of 16 call sites, and
`isAutoProperty` (codegen) at 7 sites with no fallback beside `isAutoPropertyStorage` at 16 with
one — *filtering the same member list*, so an imported auto-property gets a struct slot from one
and is skipped by the other's release walk.

⚠ **An accessor must consult both sources, not switch on which one exists.** Every drift in §6
came from two copies of one predicate reading different sources.

⚠ **Some sites are load-bearing AS decl-casts.** `LlvmBackend_Setup.cpp:15482` is deliberately
byte-identical to the lowering's own gate at `LlvmBackend_Calls2.cpp:7905`; that identity is what
keeps `eligible ⟹ slot-nulled` exact. **Unifying it alone converts a safe leak into a double
free.** Likewise `LlvmBackend_Calls2.cpp:9256` / `:9377` / `:9922`, where decl-*presence* is
genuinely the predicate's subject (#2821: "no live `ast::MethodDecl` ⇒ only the exporter's
placeholder-compiled function exists").

---

## 6. Drifted pairs — one question, two predicates, different answers

The most useful thing the type-half audit produced was its blast-radius ranking; the most useful
thing this one produced is this list. Each row is a live inconsistency, most of them independent
of the import seam.

| # | the two copies | how they disagree |
|---|---|---|
| 1 | `Emitter::recordMemberType` (`LlvmBackend_String.cpp:2627`) vs `sema::storageComponentType` | **both arms**. The property arm has no `!decl` fallback at all. The field arm delegates to `recordFieldType`, whose fallback is gated `if (!fs->posParam && !fs->decl)` — and an imported *positional* field has a **non-null synthetic `posParam` with a null `type`** (#2516), so the gate is false and the fallback never runs. **The #2874 comment directly above that gate names the exact case the condition excludes.** |
| 2 | `Emitter::propertyHasSetter` (`impl.hpp:2693`) vs `propertyDeclHasSetter` (`TypeResolution_Checks.cpp:3064`) | on an **`abstract` Auto** property: codegen says `true`, sema says `false` (§12.5 shorthand = get-only). `ModuleMetadataBuilder.cpp:1126` deliberately mirrors *codegen's* answer, so the exporter bakes codegen's verdict into the artifact. Sema's comment calls codegen's answer "SPURIOUS" and excuses it as harmless for vtable allocation — but the same predicate reserves **interface itable setter slots** at `Setup.cpp:3540/3617`, and a reserved-but-unfillable slot is filled with `ConstantPointerNull`: the #2387 SIGSEGV shape |
| 3 | `Emitter::isAutoProperty` (7 callers, **no** fallback) vs `isAutoPropertyStorage` (16 callers, fallback) | they filter the **same member list**. `collectInstanceStorage` gives an imported auto-property a `fieldIndex_` slot; `emitDisposerCore`'s three release walks (`Disposal.cpp:1009/1037/1073`) then skip it. The slot exists, is stored into with a `+1`, and is never released |
| 4 | `sema::isAutoPropertySymbol` (15 live callers) vs `sema::isAutoPropertyStorageSymbol` (1) | the fix landed and the sweep did not. Two functions **13 lines apart** with near-identical names, one wrong for imports |
| 5 | `isOwnedTempOperand`'s two arms (`LlvmBackend_Exprs.cpp:6650` vs `:6663`) | **13 lines apart in the same lambda**. The CallExpr arm has the `importedReturnOwned` fallback and a comment explaining why; the operator arm above it does not |
| 6 | `Setup.cpp:14844` vs `:14884`, and `:24486` vs `:24522` | same fact (`returnOwned`), same function family, 40 and 36 lines apart. One arm fixed, its sibling not |
| 7 | `buildRecordValue`'s initializer lookup (`Exprs2.cpp:6916-6920`) vs `emitOwnFieldInit`'s (`Setup.cpp:9510-9537`) | line-for-line identical except the class one grew the imported fallbacks (#2874/#2524) and the record one did not |
| 8 | four copies of the const-fold resolver closure (`TypeResolution_Checks.cpp:918`, `Names.cpp:1402`, `Names.cpp:1435`, `ExprTypecheck.cpp:7946/7988`) | already diverged: Checks returns `nullopt` on a name match with no decl, the two Names copies keep walking the base chain. Only the Names pair walks `baseTypes`; only the ExprTC pair sees locals; **none** consults `importedConstValue`. `Names.cpp:1429`'s comment calls its copy "a verbatim copy … matching the file's existing style" |
| 9 | `propertyAccessRunsUserCode`'s two call sites (`Exprs.cpp:2760` vs `:2844`) | `:2760` writes `pd ? f(pd) : true` and explains that unknown must be conservative; `:2844` writes bare `f(pd)` inside `if (pd)`, so unknown ⇒ `false`. The function header calls the decl-less case "conservatively left alone" — but "left alone" here means **keeping** a narrowing that may no longer hold |
| 10 | five copies of "is this member `pure`" in `PureChecker.cpp` | `:187`, `:195`, `:447` read `isPure` → AST → `modifiers`; `:377`, `:985`, `:1025`, `:1172` read the AST **only**. A sixth lives at `TypeResolution_Checks.cpp:8765` and also omits `isPure` |
| 11 | four copies of "find the ctor by arity, read `params[i].modifier`" | `resolveCtorParams` (`:152`) was fixed by #2659 with a symbol fallback. `borrowSourceForExpr` (`:3799`), `scopedFormalAt` (`:12045`) and `verifiedCallBoundedFormalAt` (`:13427`) still open-code `d && d->params.size() == nArgs` — so they do not merely mis-answer for an imported ctor, **they never match one at all** |
| 12 | `findIndexer` (filters through `indexerCandidateWireable`) vs `classIndexerMatching` (does not) | which is precisely why the itable indexer route is safe and the vtable route is not |
| 13 | `resolveMemberAccess` (imported arm since #503) vs `resolveNullCondMember` (**no imported arm**) | **MEASURED, §2/M13**: `.` gives 42, `?.` gives 1 |
| 14 | `checkArgsAgainstParams` (local) vs `checkImportedCallArity` (imported) | one function, two answers to "is this call well-formed". The imported one **silently returns** on any named argument |
| 15 | the free-var scan in `ExprTypecheck.cpp:6787-6805` (walks **TypeRefs**, handles `importedName`) vs `:6842` (asks the same question through `ast::MethodDecl`) | **in the same function**, 40 lines apart |

### Comments that assert an invariant their own code violates

Worth its own list, because each one actively prevents a reader from finding the bug.

- `TypeResolution_Checks.cpp:4187` — *"`impl` is always a LOCAL class member"*. The witness BFS
  40 lines above (`:4111-4134`) explicitly walks `c->baseTypes`, which crosses modules. `:4269`
  says the same. The sibling comment at `:4878` **in the same function** states the opposite and
  correct thing.
- `PureChecker.cpp:1133` — *"Extension methods … are visited separately below"*. There is no such
  loop; that comment is the file's only occurrence of the string. **MEASURED (§2/M8)** → #3175.
- `Setup.cpp:4663` — *"imported const w/o AST decl — decline (loud)"*. Neither search that feeds
  it ever visits `ctx_.importedSymbols()`, so it is not declined, it is never found.
- `Setup.cpp:2174` — *"Only getters — properties with no getter … skip"*, above what is actually
  a decl-presence check.
- `Symbol.hpp:848` — says `isAutoPropertyStorageSymbol` mirrors codegen's helper *"minus that
  helper's deliberate interface-property widening"*. There is no such widening; the two bodies
  are semantically identical. The comment reads as a licence to drift.
- `Ownership_Checks.cpp:9422` — justifies treating a null param list as non-borrowing because
  "every stdlib builtin … OWNs it". An imported **user** method lands in the same null bucket,
  which the reasoning never considers.
- `Serialization.cpp:235` — labelled *"Fallback (no decl — e.g. imported field)"*, but its
  enclosing function early-returns on `!ts->decl`. It is dead for an imported *type* and live
  only for a member inherited from an imported base — the one case it handles least well.

### Always-null / dead code

- `Setup.cpp:5827` — `dynamic_cast<PropertyDecl*>(pimpl->decl)` in `buildImportedGenericVtable`.
  `PropertySymbol::decl` is never set on import and this function only ever sees imported
  members, so the documented refusal is **dead code**. The comment argues carefully for why the
  refusal is correct without noticing the expression can never be true. It fails *open*: the day
  a local property reaches here, the function re-enters itself and builds a second vtable — the
  hazard #2906 removed from `specializedVtableFor` and explicitly did not remove here.
- Ten of the 360 "sites" are prose inside comments (`Setup.cpp:2679`, `:6030`;
  `Calls2.cpp:1932`; `impl.hpp:4520`, `:4581`; `Exprs2.cpp:5657`; `Ownership_Checks.cpp:70`;
  `ModuleLoader.cpp:2118`; `ModuleMetadata.hpp:158`; `Symbol.hpp:974`). Several *quote a
  decl-cast in order to explain why it is wrong*. `scripts/check_imported_symbol_reads.py` has
  the same blind spot in both directions — it counts comment text, and it misses
  `serializationSurfaceIsEmittable`, which reads `t->decl` with no cast at all. Worth teaching
  it to strip comments.

---

## 7. Family ranking, by severity of a wrong answer

Ranked by what a wrong answer *does*, not by site count — the type-half audit's ranking was the
most useful thing in it.

| # | family | sites | a wrong answer is | reachable today? |
|---|---|---|---|---|
| **1** | **M1 — `owned` / `out owned` / `scoped` formals of an imported callee** | 18 | **[M] DOUBLE FREE — measured `heap-use-after-free` + SIGSEGV.** The caller never marks the argument moved, so it disposes what the callee already disposed. The marked spelling `M(owned r)` is *also* rejected, so there is currently no correct way to write the call | **YES** — #3176 |
| **2** | **M13 — interface conformance witness inherited from an imported base** | 1 | **FALSE ACCEPT / soundness.** `TypeResolution_Checks.cpp:4174`. The `owned`-return match, return-type match and param-type match all key on `implDecl` and all vanish. The itable slot is filled with a function of the wrong signature; dropping `owned` off an inherited implementation means the caller never records the transfer (leak), tightening it means a double dispose | **YES** |
| **3** | **M6 — imported-ABI function maps not consulted** | 9 | **CRASH.** `Assign.cpp:5319/5467` leave a vtable/itable indexer slot as `ConstantPointerNull` → indirect call to `0x0`. `Assign.cpp:9196/9286` and `Stmts.cpp:7411` hand back a raw class pointer that is then driven through the §29.11 enumerator vtable (#2384 records this shape as a confirmed ASan `global-buffer-overflow`), or store nothing into the loop variable so the loop spins on a constant | **YES** for a non-virtual imported base indexer; the virtual sub-shape also needs #2837's ABI |
| **4** | **M8 — §22.15 purity across the boundary** | 2 | **[M] FALSE ACCEPT — measured.** A `pure` ctor chaining to an imported impure base ctor compiles clean. `:277` is the false-*reject* twin for imported body records | **YES** — #3174 |
| **5** | **M3 — auto-property classification** | 8 + 15 unmigrated callers | **[M] §9.2 FALSE ACCEPT + §8.5 WRONG DATA — both measured.** Plus the alloc/release split (drift #3) leaking every `+1` string/closure/box in an imported auto-property slot | **YES** for sema (measured); the disposal half is latent behind #2884 |
| **6** | **M2 — the `owned`-return channel** | 15 | **silent LEAK**, the widest single family. `common::calleeMethodDecl` is *the* shared navigation to `returnOwned` and returns null for every imported method; `isFreshOwnedClassProducer` then says "not a producer", and a dozen codegen disposal sites consume that. `ExprTypecheck.cpp:11067` means `Lib.MakeOwned();` as a bare statement is silently accepted | **YES** |
| **7** | **M4 — member TYPE resolution at a use site** | 13 | **[M] silent WRONG DATA — measured `?.` = 1.** Plus §7.4 nullability never enforced on an imported field (`Calls.cpp:12429/12435`) — a false accept, null deref at runtime | **YES** — #3177 |
| **8** | **M7 — method-level generic params / substitution** | 9 | IR-verifier failure, SEGV on `0x8`, or silent miscompile against the i32-erased template. `Calls2.cpp:10707` disables §580 virtual-generic dispatch wholesale for an imported declaring interface | mostly **LATENT** behind #2884 |
| **9** | **M11 — const folding across the boundary** | 6 | `[K]int` over an imported const never folds → `arraySizeValue` stays −1 → codegen falls back to a scalar, so every element GEP is malformed (**OOB**). Plus `case` out-of-range and shift-domain gates silently not firing, and a false reject of `var b: byte := Lib.C.K;` | **YES** |
| **10** | **M14 — disposal-side member classification** | 2 | **deterministic LEAK.** `Disposal.cpp:1102` is an **alloc/free asymmetry**: #2421 unified the allocation half (`emitOwnFieldInit` gates on `eventIsStatic`) and not the free half, so an imported instance event allocates one `bz_event_source_t` per instance and frees zero. `:954` leaks N objects per `[N owned]T` field | **LATENT** — both reached only via `emitSpecializedDisposer` |
| **11** | **M5 — call binding / overload ranking / defaults** | 8 | **[M] two measured FALSE REJECTS** of legal programs, with a diagnostic describing a call the user did not write; plus named arguments **unchecked entirely** (a false accept) | **YES** — #3178 |
| **12** | **M9 — initializers not replayed** | 3 | silent WRONG DATA: `new ImportedRecord()` reads back all zeros; an imported auto-property's `:= v` gets neither a value nor the §2.8.2 diagnostic the field path fires | **YES** |
| **13** | **M12 — `abstract` / `static` modifier gates** | 5 | **PROBE-REFUTED for `base.P` / `base[i]`** against an imported abstract member (issue #3232, lane D 2026-08-18): the named-method form already reads `ms->modifiers` (populated on import, ExprTypecheck_Calls.cpp:2309-2313) and rejects correctly via the #874 diagnostic; the named-property form at ExprTypecheck_Calls.cpp:18041-18043 reads `pd->modifiers` only (null for imports), so the sema-side #2838 check is bypassed — but `LlvmBackend_Assign.cpp`'s rejectImportedVirtualBaseProperty (#3199) catches the case at codegen time and emits a correct §12.3 error (no silent zero-init). The indexer twin at ExprTypecheck_Exprs.cpp:2934-2939 is the same shape, caught by rejectImportedVirtualBaseIndexer (#2832/#2837). All three named shapes are REJECTED in both single-module and two-module builds; the cross-module diagnostic cites #3199/#2837 instead of #2838/#2832, which is technically correct (§12.3 ABI reasoning) but less informative. The §22.12 rule 5 static-event shape was not probed here (event-syntax probe failed to parse in the time budget); treat the family as REFUTED on `base.X`-via-imported-abstract and DEFERRED on the static-event half | **YES** for the diagnostic-quality concern; NOT a soundness defect |
| **14** | **M10 — §9.8 serialization member clauses** | 2 | wire desync: a renamed component written under its declared name, a `skip` component written anyway, so the field count in `bz_binser_begin_object` and the actual writes disagree | **YES**; **BLOCKED on schema** (§4.2) |
| **15** | **M15 — export-side latents** | 1 | `ModuleMetadataBuilder.cpp:1702` silently `continue`s a decl-less extension method, so the day re-export lands the whole block vanishes from the artifact with no diagnostic | not reachable today |

**Blocked on schema:** only **M10** fully, and **M5** partially (needs `ParameterSymbol::hasDefault`
— a symbol field and one loader line, no format change). **M11** needs a typed value on
`ConstMemberSymbol`. Everything in ranks 1–8 is reachable with what already crosses.

---

## 8. Suggested order of work

The type half's order — smallest blast radius first to establish the test shape, then highest
severity, then the biggest mechanical win — worked well. Adapted, with one addition: **prove
reachability before writing the test** (§1(b)).

1. **#3176 (M1)** — first, out of order, because it is the only *measured memory-safety*
   violation in the audit and its regression test is already written in the issue. It is also the
   smallest real fix: `sema::formalParamModifier` is already both-sources and already has a
   `ConstructorSymbol` overload that three of the six sites simply do not call. Add the
   `MethodSymbol` overload, route all six, land whole.
2. **#3174 + #3175 (M8)** — two small, self-contained soundness fixes with measured repros, in
   one file. #3175 is not even an import bug, which makes it a clean warm-up that establishes the
   §22.15 test shape for #3174.
3. **M13** (`TypeResolution_Checks.cpp:4174`) — one site, soundness, and the fallback it needs is
   *already written 25 lines above it* for the interface side. Delete the three false locality
   comments in the same commit.
4. **#3177 (M4)** — the `storageComponentType` sweep. Thirteen sites, one accessor that already
   exists, and a measured value to assert. ⚠ `Calls.cpp:12429` and `ExprTypecheck.cpp:8445` are
   **coupled**: fixing the gate without the narrowing-key minting converts a false accept into a
   false reject. Same for `Exprs.cpp:8559`/`:8573`.
5. **M2** — the `returnOwned` sweep. Fifteen sites, one existing channel, largest mechanical win.
   ⚠ **`common::calleeMethodDecl`'s null is load-bearing in two contradictory directions**: at
   `OwnershipFacts.cpp:113` it means "not an owned producer"; at `Ownership_Checks.cpp:9666`,
   `:10163`, `:10204`, `:10358` the *same* null means "no user method here, run the builtin rule".
   Anyone fixing it must re-examine those four gates in the same change or flip from over-strict
   to under-strict.
6. **M3** — finish the `isAutoPropertyStorageSymbol` migration (15 sema callers) **and** the
   codegen `isAutoProperty` → `isAutoPropertyStorage` split (drift #3) in one commit. This is the
   half-done fix #3149 asked for first; leaving it half-done is worse than not starting.
7. **#3178 (M5)** — `ParameterSymbol::hasDefault` plus the formal-list view. Two measured repros.
8. **M6, M12, M9, M11** — the remaining reachable families.
9. **M7, M14** — after #2884 / #3162 land, since their tests cannot fail before then. Until then
   their sites must degrade *conservatively* rather than silently answering "no".
10. **M10** last — the only family fully blocked on new schema, and the `EnumConstantSymbol::
    serializedName` precedent (#3140) tells the lane exactly what to write.

**Two cross-cutting cleanups**, each cheap and each preventing a re-earn:
- Populate `MemberSymbol::isPure` from `SymbolCollection` so it becomes a genuine both-sources
  channel, collapsing six copies of one predicate (§4.3, drift #10).
- Either read `MemberSymbol::contractRecords` or delete it (§4.3). A carried-and-never-read
  channel is how the next lane concludes the fact does not cross.

---

## 9. What could not be determined

- **Whether M6's crash is reachable without an imported *generic* type.** `classIndexerMatching`
  walks `baseClassOf(t)` with no wireability filter, which reads as reachable for a local class
  inheriting a **non-virtual** indexer from a plain imported base — but no probe here confirmed
  it. Settle it with a two-module build of exactly that shape before writing the fix.
- **Whether M14's event leak is reachable at all today.** Both `Disposal.cpp` sites are reached
  only through `emitSpecializedDisposer`, and probe M9 showed imported-generic member access is
  refused by #2884. Settle it by re-running M9 after #2884.
- **Whether an `abstract property P: T;` on an abstract class implementing an interface can
  reach `Setup.cpp:3540/3617`** and plant the #2387 null-slot SIGSEGV via drift #2. This is the
  one drift whose consequence is a crash rather than a wrong answer, and it is the one I could
  not close. It needs a targeted probe: an abstract class with an abstract auto-property, an
  interface declaring the same property with a setter, and a write through the interface.
- **Whether `Setup.cpp:1040`'s erased-type miscompile survives #2884.** It is the static-field
  twin of the blocked instance path.
- **The exact §9.8 desync symptom for M10.** Reading says the field count and the writes
  disagree; whether that surfaces as a `SerializationException`, a silently defaulted field, or a
  reader crash depends on `bz_binser`'s framing, which was not traced.

Settling the first three needs three two-module probes and roughly an hour; none needs a code
change.

---

## 10. Issues filed from this audit

| # | title | severity |
|---|---|---|
| **#3176** | an `owned` formal of an IMPORTED method never records the move — measured heap-use-after-free | ownership unsoundness |
| **#3174** | a `pure` constructor chaining to an IMPORTED impure base ctor is accepted (§22.15) | soundness |
| **#3175** | a `pure` method in an `extension` block never has its body checked (§22.15) | soundness, not import-related |
| **#3177** | `imported?.Field` yields a garbage value — `resolveNullCondMember` has no imported arm | silent wrong data |
| **#3178** | cross-module call checking is a stub — §22.6 ranking never runs, optional params unusable | two false rejects |

Pre-existing and confirmed still open: **#3164** (positional field's `owned` marker), **#3162**
(imported primary-ctor construction), **#2884** (imported generic property access), **#3050**
(`equality skip`, unimplemented at every layer — `grep -rn 'equality skip' src/` returns nothing).

---

## Appendix A — the site table

⚠ **READING-DERIVED.** Every row below was produced by reading the site and its enclosing
function, **not** by reproducing it. Rows tagged **[M]** are covered by a §2 measurement. The
type-half audit's equivalent table was wrong in six rows (#3168); assume this one is too, and
verify before fixing.

Verdicts per §1's rubric. ALREADY-CORRECT rows carry their reason:
**local-walk** (the receiver came from `ctx_.module()->children` / `ctx_.symbols()` /
`currentType_` / a parsed `ast::TopDecl&`; note `makeImportedSymbol` keeps imported symbols
**out** of `symbols_`, which alone settles ~25 sites), **export-side**, **body re-parse**
(`ModuleLoader`'s `fresh*` symbols), or **guarded** (a real symbol-side fallback).

### A.1 `src/ownership/Ownership_Checks.cpp`

| line | verdict | kind | question | imported gets today | should read |
|---|---|---|---|---|---|
| 70 | *not a site* | Ctor | prose in the `CtorFormals` comment documenting the cast #2659 removed | — | — |
| 167 | ALREADY-CORRECT (guarded) | Ctor | does ctor formal *i* carry `owned`? | `else f.ctorSym = cs` → `sema::formalParamModifier` | — |
| 480 **[M]** | **UNIFY** | Method | which formals of the callee are `owned`? | `params=nullptr` → owned sink invisible → **double free** | `formalParamModifier` over `ms->params` |
| 482 **[M]** | **UNIFY** | Ctor | same, ctor callee | same | `sema::formalParamModifier(*cs, i)` — **the overload already exists** |
| 637 **[M]** | **UNIFY** | Method | `isImplicitOwned` | `return false` | same |
| 639 **[M]** | **UNIFY** | Ctor | same | `return false` | same |
| 1804 | **UNIFY** | Property | is the declared type a function type (value read) vs a §27.6 promoted reference? | always null → `return true` (may-borrow) | `ctx_.importedMemberType(ps)` |
| 1871 | **UNIFY** | Indexer | user indexer's declared element type | null → conservative `return true` | `ctx_.importedMemberType(ps)` |
| 2203 **[M]** | **UNIFY** | Method | `calleeArgIsOwnedSink` (in-closure §28.3 consume) | `false` → the consume check does not fire → UAF | `formalParamModifier` |
| 2206 **[M]** | **UNIFY** | Ctor | same | `false` | `sema::formalParamModifier(*cs, i)` |
| 3799 | **UNIFY** | Ctor | ctor arity match + is formal `owned`/`out owned`? | `d && d->params.size()==nArgs` **never matches an import** | `formalParamCount`/`formalParamModifier` |
| 6896 | **UNIFY** | Method | is formal `out owned`? | recipient never marked Owned → **silent leak** (#2429 shape) | `formalParamModifier` |
| 7047 | **UNIFY** | Method | same, in an if/while condition | recipient never collected | same |
| 9374 | **UNIFY** | Method | is the callee `static`? (§28.3 static-factory) | `return false` before the modifier scan | `ms->modifiers` — populated on import |
| 9422 | **UNIFY** | Method | is formal a PLAIN (non-`owned`) reference param? | `false` → §28.3 deferred-activation reject silently does not run | `formalParamModifier` |
| 9425 | **UNIFY** | Ctor | same | `false` | `sema::formalParamModifier(*cs, i)` |
| 9598 | **UNIFY** | Method | is formal a plain function slot? (§28.14 capture) | `false` → escaping frame capturing an owned local admitted | `formalParamModifier` |
| 9601 | **UNIFY** | Ctor | same | `false` | same |
| 12045 | **UNIFY** | Ctor | is formal `scoped`? | never arity-matches → `checkScopedAdmission` never runs | `formalParamCount`+`formalParamModifier` |
| 12057 | **UNIFY** | Method | same | falls to builtin/function-type arms → false | `formalParamModifier` |
| 12197 | **UNIFY** | Method | does the call return `owned`? | `false` → §28.14 forms 2/3 lost → **false reject** of `using var sub := imported.Subscribe(…)` | `ctx_.importedReturnOwned(ms)` |
| 13427 | **UNIFY** | Ctor | plain function-typed formal (verified call-bounded)? | `return false` | `formalParamModifier` + `resolvedDeclType` |
| 13437 | **UNIFY** | Method | same | `return false` | same |

### A.2 `src/ownership/Ownership.cpp` · `src/common/OwnershipFacts.cpp` · `src/sema/{DefiniteAssignment,StaticInitCycles,EntryPoint}.cpp`

| file:line | verdict | kind | question | imported gets today | should read |
|---|---|---|---|---|---|
| Ownership 3199 | **UNIFY** | Method | does the inherited base decl return a bare type parameter? | not found → `recognized=false` → the consume-site freeze does not protect the borrow (**UAF**) | `importedReturnType/Owned(bms)` + `bms->params` |
| Ownership 3231 | **UNIFY** | Method | same, callee side | `false` → **false reject** of a body the S10 carve-out should admit | same |
| Ownership 3271 | **UNIFY** | Method | which inherited params are *source* positions? | empty vector | same + `ParameterSymbol::modifier` |
| Ownership 4110 | **UNIFY** | Operator | does this operator return `owned`? | falls to Borrowed → **leak** (#965 cross-module) | `ctx_.importedReturnOwned(ms)` |
| Ownership 4313 | **UNIFY** | Property | is this a user getter (⇒ §28.6 borrow-producing)? | always null; the #2174 fallback covers only iface/virtual reads → `using var x := imported.Prop` **double-frees a borrow** | the resolved `PropertySymbol` itself |
| Ownership 5149 | **UNIFY** | Field | is the field type an owning inline array? | `continue` → missing §28.3 auto-drop **warning** only | `ctx_.importedMemberType(fs)` |
| OwnershipFacts 91 | **UNIFY** | Method | `calleeMethodDecl` — the shared route to `returnOwned` | null for every import → `isFreshOwnedClassProducer` false → a dozen codegen disposal sites emit no reclaim | `ctx.importedReturnOwned(ms)` ⚠ see §8 step 5 |
| DefiniteAssignment 89 | **UNIFY** (narrow) | Method | declared bodyless without an `abstract`/`virtual`/`override` modifier? | `return false`; the `modifiers` scan already covers the modifier-bearing cases | a NEW "declared bodyless" wire bit |
| StaticInitCycles 95 | **NEEDS-AST** | Method | fold a called static method's **body** into the edge set | documented cross-module deferral (§22.13 rule 2 runtime backstop) | — |
| StaticInitCycles 106 | **NEEDS-AST** | Property | fold a read static property's **getter body** | same | — |
| StaticInitCycles 280 | ALREADY-CORRECT (local-walk) | Field | static field's initializer expression | `checkStaticInitCycles` skips `ctx.isImported(t)` | — |
| StaticInitCycles 284 | ALREADY-CORRECT (local-walk) | Ctor | static ctor body | same | — |
| StaticInitCycles 294 | ALREADY-CORRECT (local-walk) | Field | does a static field have an initializer? | same; UNIFY-*shaped* but unreachable | — |
| EntryPoint 63 | ALREADY-CORRECT (local-walk) | Method | is this the §33.2 `Main`? | `ctx.symbols()` excludes imported symbols by arena split | — |

### A.3 `src/sema/ExprTypecheck_Calls.cpp`

| line | verdict | kind | question | imported gets today | should read |
|---|---|---|---|---|---|
| 1077 **[M]** | **UNIFY** | Method | can the initial overload §22.5-bind these named args? | null → `hasNamed` early-accept never fires | `MethodSymbol::params` + a new `hasDefault` |
| 1085 **[M]** | **UNIFY** | Method | same, per candidate | null for every imported candidate | same |
| 1103 **[M]** | **UNIFY** | Method | which overloads can bind this call? (feeds §22.6 ranking) | `cands` is **always empty** → `arityFallback()` → first-in-source wins | a formal-list view over `ms->params` |
| 2170 | **UNIFY** | Method | §14.5 arity, §14.2 constraints, inference, substitution, return type | only the return leg has a fallback, and it publishes the **unsubstituted** type | `genericParams`, `importedGenericConstraints`, `resolvedDeclType`, `importedReturnType` |
| 8959 | **UNIFY** | Method | `Domain.Spawn(Entry,…)` entry return + `owned` | `Unit` → payload-less `OwningTask`, join result corrupted | `importedReturnType/Owned(entryMs)` |
| 9565 | ALREADY-CORRECT (guarded) | Method | is there an AST to check the call against? | `checkImportedCallArity` at :9570 — but see the note below | — |
| 10803 | ALREADY-CORRECT (guarded) | Ctor | which ctor overloads bind? | `checkImportedNewExprArgs` early return at :10791 | — |
| 10925 | ALREADY-CORRECT (guarded) | Ctor | sole ctor's params, §22.3 modifiers | same early return | — |
| 11288 | **UNIFY** | Event | the event's payload `T` for `Subscribe(h)` | nullptr read as "no payload" → handler lambda untyped → its body fails to resolve | `ctx_.importedMemberType(es)` |
| 12115 | **UNIFY** | Method | does this generic call need a target type, and what solves it? | `return false` → no inference **and** no "target type required" diagnostic | `genericParams` + `resolvedDeclType` + `importedReturnType` |
| 12236 | **UNIFY** | Method | an extension-method *reference*'s function type | `funcReturn = Unit` | `ctx_.importedReturnType(ms)` |
| 12288 | ALREADY-CORRECT (loader-never-creates) | Property | extension-property's type | the loader reconstructs only Method members of an imported extension | — |
| 12429 | **UNIFY** | Field | is this bare-ident field's type nullable? (§7.4 TOCTOU gate) | null → **the §7.4 gate never fires** → `_f.M()` accepted with no narrowing | `sema::storageComponentType` ⚠ couple with ExprTypecheck 8445 |
| 12435 | **UNIFY** | Property | same, bare-ident property | same false accept | same |
| 17317 | ALREADY-CORRECT | Extension | does a same-named extension exist? | `ExtensionSymbol::decl` **is** set (`ModuleLoader.cpp:2773`) | — |
| 17438 | **UNIFY** | Property | is this base property `abstract`? | `false` → `base.P` on an imported abstract property accepted → codegen zero-inits | `ps->modifiers` |
| 17653 | ALREADY-CORRECT (guarded) | Field | is an imported generic class's field `T`-typed? | `else if (!fs->decl) fty = ctx_.importedMemberType(fs)` (4th hand-rolled `storageComponentType` copy) | — |
| 17723 | ALREADY-CORRECT (guarded) | Field | field-access result type | `if (ctx_.isImported(member))` block at :17460 returns first | — |
| 17739 | ALREADY-CORRECT (guarded) | Property | property-access result type | same | — |
| 17766 | ALREADY-CORRECT (guarded) | Const | `C.K` access type | same | — |
| 17810 | ALREADY-CORRECT (guarded) | Method | bare `obj.M` reference's function type | same early return — but that arm has **no MethodSymbol case**, so the reference is left untyped | — |
| 18032 **[M]** | **UNIFY** | Field | `obj?.Field` result type | no type → `lowerCoalesce` returns nullptr → **measured garbage value** | `sema::storageComponentType` |
| 18043 **[M]** | **UNIFY** | Property | `obj?.Prop` result type | same | same |

**Note on `:9565`.** `checkImportedCallArity` **returns at `:9655`** on the first named /
modifier / discard argument, so a cross-module call using a named argument is not checked at
all — neither arity nor assignability. That is the false-*accept* twin of #3178's two false
rejects, and it is why `:9565` is ALREADY-CORRECT only in the narrow sense that *a* fallback
exists.

### A.4 `src/sema/ExprTypecheck_Exprs.cpp`

| line | verdict | kind | question | imported gets today | should read |
|---|---|---|---|---|---|
| 625 | ALREADY-CORRECT (guarded) | Indexer | k-th index-param type (§7 nullability) | `prm->resolvedDeclType` | — |
| 985 | ALREADY-CORRECT (local-walk) | Extension | `this`'s type in a primitive-extension body | `currentExtension_` is the block being compiled | — |
| 1105 | ALREADY-CORRECT (guarded) | Field | bare-ident field type | `else if (ctx_.isImported(fs))` → `importedMemberType` (#2532) | — |
| 1157 | ALREADY-CORRECT (guarded) | Property | bare-ident auto-property type | `else if (ctx_.isImported(ps))` (#2504) | — |
| 1215 | **UNIFY** | Const | bare-ident `K`'s type (§22.12/§2.7) | **no arm at all** → `K` carries no exprType (#3079's shape, cross-module). The two adjacent arms both got the fallback; this one did not | `ctx_.importedMemberType(cms)` |
| 2239 | **UNIFY** | Indexer | result type via `where T: IIndexable` | `continue` → IndexExpr untyped → value falls to 0 | `storageComponentType` |
| 2334 | ALREADY-CORRECT (guarded) | Indexer | candidate's param count | `cand->indexerParams.size()` (#2414) | — |
| 2342 | ALREADY-CORRECT (guarded) | Indexer | candidate's k-th param type | `p->resolvedDeclType` (#2414) | — |
| 2567 | ALREADY-CORRECT (guarded) | Indexer | does an index param name a receiver type param? | re-asked over `ps->indexerParams` | — |
| 2620 | ALREADY-CORRECT (guarded) | Indexer | element type | `else { rt = ctx_.importedMemberType(ps); }` | — |
| 2806 | **UNIFY** | Indexer | is this base indexer `abstract`? | `false` → `base[i]` accepted → zero/null value (#2832's other half) | `bps->modifiers` |
| 2844 | **UNIFY** | Property | can this access run user code? (§7.4 narrowing invalidation) | `false` → the narrowing is **kept** across a cross-module accessor that may null the local | `!isAutoPropertyStorageSymbol` — and `true` when unknown, as the indexer twin at :2760 does |
| 4003 | ALREADY-CORRECT (guarded) | Operator | binary operator result type | `else if (ctx_.isImported(opm))` (#1485) | — |
| 4086 | ALREADY-CORRECT (guarded) | Operator | constraint static-operator result | `else if (ctx_.isImported(iop))` (#2359) | — |
| 5111 | ALREADY-CORRECT (guarded) | Operator | unary operator result type | (#1485) | — |
| 5632 | **UNIFY** | Event | is the `Subscribe` target a **static** event? (§22.12 rule 5) | `false` → the capture-free rule is not enforced | `ctx_.importedEventIsStatic(es)` — on the wire since #2421, **never read by sema** |
| 5651 | ALREADY-CORRECT (guarded) | Method | callee formal type for `out var` | `calleeMs2->params[pos]->resolvedDeclType` (#2657) | — |
| 6499 | ALREADY-CORRECT (guarded) | Ctor | ctor formals for `new T(out var x)` | `css`/`ctorSym` fallback (#2668) | — |
| 6803 | **UNIFY** | Ctor | which ctor's formal modifier applies to arg *j*? (§22.3/§22.4 `out owned`) | every candidate `continue`s → the call-site rule is not enforced | `ConstructorSymbol::params` → `modifier` |
| 8573 | ALREADY-CORRECT (unreachable) | Property | `with`-target replacement §5.1 check | the AST-only `isAutoPropertySymbol` gate at :8559 rejects every imported auto-property first. ⚠ **LATENT**: fixing :8559 makes this a live false accept | `storageComponentType` — must land *with* :8559 |
| 8594 | **UNIFY** | Field | `with`-target field's declared type | `posParam->type` null and `decl` null → **`checkAssignable` skipped entirely** | `storageComponentType` |

### A.5 `src/sema/ExprTypecheck.cpp`

| line | verdict | kind | question | imported gets today | should read |
|---|---|---|---|---|---|
| 4550 | ALREADY-CORRECT (guarded) | Property | setter presence / readonly / visibility on write | full `else` at :4619 (#2412/#2415) | — |
| 4929 | ALREADY-CORRECT (guarded) | Property | setter/getter for a `ref`/`out` operand | `if (!pd) {…}` (#2400/#2415/#2461) | — |
| 6752 | ALREADY-CORRECT | Extension | extension block's target type | `ExtensionSymbol::decl` is set on import | — |
| 6843 | **UNIFY** | Method | does an extension method's *signature* mention a free type variable? (#773) | never runs → the member is admitted and generates type-mismatched IR | `resolvedDeclType` + `importedReturnType` — **the same scan this function already does over TypeRefs 40 lines above** |
| 7338 | ALREADY-CORRECT (local-walk) | Extension | is `name` a type param of the enclosing extension? | `currentExtension_` | — |
| 7362 | ALREADY-CORRECT (local-walk) | Extension | enclosing extension's constraints | same | — |
| 7954 | **UNIFY** | Const | bare-name const's integer value (`case` range gate) | `nullopt` → out-of-range `case` **silently accepted**, truncated by codegen | a typed `ConstMemberSymbol::intValue` |
| 7981 | **UNIFY** | Const | qualified `C.K`'s value (§5.1 constant narrowing) | `nullopt` → `var b: byte := Lib.C.K;` **false-rejects** where the in-module spelling compiles | same |
| 8001 | **UNIFY** | Const | const value for the div-by-zero diagnostic | degrades to the generic message | same |
| 8085 | **UNIFY** | Const | const as an inline-array size `[K]T` | `arraySizeValue` stays −1 → codegen falls back to a scalar → **malformed element GEPs** | same |
| 8445 | **UNIFY** | Field | readonly field's type (nullable ⇒ trackable?) | `readonlyFieldPathOf` declines → no narrowing fact minted | `storageComponentType` ⚠ couple with Calls 12429 |
| 9502 | **UNIFY** | Method | does this call produce an `owned` value? (§22.5/§21.9) | `false` → the leak check is skipped | `ctx_.importedReturnOwned(ms)` |
| 11067 | **UNIFY** | Method | same, §21.9 bare discard | `Lib.MakeOwned();` as a statement **silently accepted** → leak | same |
| 11537 | **UNIFY** | Event | `event += h`: payload type **and** staticness | payload null (handler untyped) **and** §22.12 rule 5 skipped | `importedMemberType` + `importedEventIsStatic` |
| 12395 | **UNIFY** | Operator | compound-assign result type | `resTy` null → an imported `operator +(Point,Point): int` lets `p += q` store an `int` into a `Point` slot | `ctx_.importedReturnType(opm)` — what :4003/:5111 already use |
| 12895 | **UNIFY** | Method | `for var e in coll`: element type from `GetEnumerator` | the `ms->decl` guard skips an imported one → loop var untyped → codegen emits nothing | `ctx_.importedReturnType(ms)` → `genericArgs[0]` |

### A.6 `src/sema/TypeResolution_Checks.cpp`

Forty-nine of the 52 sites are **ALREADY-CORRECT**. Forty-one are `local-walk` — the driver
loops (`:230`, `:305`, `:356`, `:482`, `:541`, `:871`, `:981`, `:1023`, `:1674`, `:3359`,
`:5182`, `:7046`, `:7115`, `:7597`, `:8719`, `:8738`, `:9042`) all iterate
`ctx_.module()->children` or `ctx_.symbols()`. Eight are `guarded` with a real symbol-side
fallback, each from a named fix: `:2890`/`:4172`/`:5147`/`:5152`
(`importedReturnOwned`/`importedReturnType`, #2386/#2373/#2294), `:3146`/`:4907`
(`importedMemberType`, #2380), `:4442`/`:4450`/`:4711`/`:4849`
(`importedPropertyHasSetter`/`HasGetter`, #2529), `:4754`/`:4761`/`:4776`
(`indexerParams`/`resolvedDeclType`, #2449), `:3651`/`:4231` (`modifiers` first — the loader
pushes `"pure"` into an imported member's `modifiers`).

Several of those `local-walk` rows are *also* NEEDS-AST in substance (`:331` walks `cd->body`;
`:887`/`:925` fold an initializer expression; `:1684` reads a default-value expression, and its
comment at `:1680` is the file's one model of documenting *why* the imported case is skipped;
`:7991` walks statements; `:8785` scans for `yield`; `:8978` reads accessor `isPure`/`isSet`).

The three that are not ALREADY-CORRECT:

| line | verdict | kind | question | imported gets today | should read |
|---|---|---|---|---|---|
| 144 | **UNIFY** (latent) | Field | field type, §9/§35.1 by-value self-containment walk | an imported hop in the value graph is invisible. **Latent**: `start` is always local and module dependencies are acyclic, so nothing can close a cycle through an import today | `sema::storageComponentType` — this is a hand-inlined 2-source copy of the 3-source accessor |
| 4174 | **UNIFY** | Method | does the *implementing* witness's return match the interface's? | **the comment at :4187 claims `impl` is always LOCAL — it is FALSE**; the witness BFS at :4111 walks `c->baseTypes`. `implDecl` null ⇒ the `owned` match, the return-type match and the param-type match are all **skipped** | `importedReturnOwned/ReturnType(impl)` — written 25 lines above for `im` |
| 7678 | **UNIFY** | Field/Property | does this class level carry its own instance storage? (§9.8 base-subobject interlock) | **unconditionally false for every imported base**: no `FieldDecl`, `posParamBodyRef` never set, `isAutoPropertySymbol` decl-only. The reject never fires → inherited fields round-trip as calloc-zeros, which the site's own comment calls "instance-corrupting" | `anyRecordStorageField` + `isAutoPropertyStorageSymbol`; the captured-vs-uncaptured positional split needs a NEW `posParamCaptured` wire bit |

### A.7 `src/sema/TypeResolution_Names.cpp` · `SymbolCollection.cpp` · `Symbol.hpp`

| file:line | verdict | kind | question | imported gets today | should read |
|---|---|---|---|---|---|
| Names 70 | ALREADY-CORRECT (guarded) | Field | is an `owned` field's type a bare type param? (§14.4/§28.5) | this IS a both-sources accessor (#3147, family F12) | — |
| Names 1409 | **UNIFY** | Const | what does this inherited member const fold to? | the loop walks `ts->baseTypes` **across modules**; an imported `ConstMemberSymbol` matches by name then yields nothing | `ctx_.importedConstValue(cms)` + codegen's `foldImportedConstText` shape |
| Names 1421 | ALREADY-CORRECT | Const | is this lexically-scoped name a foldable const? | only imported **TypeSymbol**s enter `moduleScope.names`; module-level consts are not part of the exported surface at all | — |
| Names 1442 | **UNIFY** | Const | same as 1409, §17.14 shift-domain probe | verbatim-copied closure, same defect | same |
| Names 1454 | ALREADY-CORRECT | Const | same as 1421 | same | — |
| SymbolCollection 481 | ALREADY-CORRECT (guarded) | Ctor | the ctor's k-th formal (`ctorDeclParam`) | the LOCAL half of the documented both-sources pair; `formalParamModifier` falls through to `ctorSymParam`. **This is the #2659/#2374 template** | — |
| SymbolCollection 508 | ALREADY-CORRECT (guarded) | Ctor | ctor formal arity | `return cs.params.size()` | — |
| SymbolCollection 680 **[M]** | **UNIFY** (by-design split; fix at the callers) | Property | is this an auto-property? | `false` for every imported property. The both-sources twin sits **13 lines below** and has 1 caller to this one's 15 | callers on imported-reachable paths must call `isAutoPropertyStorageSymbol`; this one should become file-local so no new caller can pick the wrong half |
| SymbolCollection 732 | ALREADY-CORRECT (guarded) | Field | body field's type (inside `storageComponentType`) | `return ctx.importedMemberType(m)` | — |
| SymbolCollection 737 | ALREADY-CORRECT (guarded) | Property | property type (inside `storageComponentType`) | `return ctx.importedMemberType(m)` | — |
| Symbol.hpp 974 | *not a site* | Ctor | prose in `formalParamModifier`'s doc comment **describing this very defect** | — | teach the ratchet to strip comments |

### A.8 `src/sema/PureChecker.cpp`

| line | verdict | kind | question | imported gets today | should read |
|---|---|---|---|---|---|
| 188 | ALREADY-CORRECT (guarded) | Operator | is the callee operator `pure`? | `:187`'s `if (ms->isPure) return true;` answers first | — |
| 196 | ALREADY-CORRECT (guarded) | Ctor | is the callee ctor `pure`? | `:195`'s `cs->isPure` fires first | — |
| 277 | **UNIFY** | Ctor | does a user ctor of arity N exist, and is it `pure`? | `continue` → `findUserCtorByArity` returns nullptr → body record ⇒ **FALSE REJECT**; positional record ⇒ **FALSE ACCEPT** (an authored impure shadowing ctor is invisible) | `cs->isPure` + `cs->params.size()`; return the SYMBOL, not the decl |
| 377 **[M]** | **UNIFY** | Ctor | is the `base(…)`/`this(…)` chain target `pure`? | every imported ctor decl is null → `if (!targetCD) return;` **admits silently** — measured | `targetSym->isPure` |
| 449 | ALREADY-CORRECT (guarded) | Ctor | does the thrown exception class have a `pure` ctor? | `:448`'s `cs->isPure` fires first | — |
| 985 | ALREADY-CORRECT (local-walk) | Operator | is this member `pure`? | `checkPureBodies` walks `ctx.module()->children` | — (drift: this copy omits `ms->isPure`) |
| 1001 | **NEEDS-AST** | Method | contract clauses + statement/expression body | unreachable (local-walk) | `md->contractClauses`, `exprBody`, `stmts` |
| 1002 | **NEEDS-AST** | Operator | operator body | unreachable | `od->exprBody`, `od->stmts` |
| 1025 | **NEEDS-AST** | Ctor | clauses, `pure` flag, statement body | unreachable | `cd->contractClauses`, `cd->body` |
| 1056 | **NEEDS-AST** | Property | is any getter `pure`, and what is its body? | unreachable — and independently always-null for an import | `pd->accessors[i].{exprBody,stmts}` |
| 1172 | ALREADY-CORRECT (local-walk) | Ctor | does this class have a `pure` ctor? | unreachable | — (drift: also omits `cs->isPure`) |
| 1188 | **NEEDS-AST** | Field | the field's initializer expression | unreachable | `fd->initializer` |

⚠ `checkPureBodies` at `:1133` claims extension members are "visited separately below". They are
not — see §6 and **#3175**.

### A.9 `src/module/ModuleMetadataBuilder.cpp` · `ModuleLoader.cpp` · `ModuleMetadata.hpp`

All ten builder sites are **export-side**: `buildModuleMetadata` → `mod->children`. The three
candidate import-leak paths were traced and none reads `->decl` on an imported symbol
(`collectInvariants` has a symbol-side fallback for an imported base; `canonicalClauseText`'s
base-chain walk reads only `member->name`; base type refs are converted as `TypeRef`s, never as
members). Nine are **NEEDS-AST** — `:873` (field type + initializer), `:955` (return type,
constraints, param defaults, contracts, span), `:991` (operator), `:1004` (ctor), `:1037`
(property type, accessors, accessor visibility), `:1192` (event payload type), `:1208` (const
type + value), `:1562` (destructor body text), `:1664` (extension target type, `where` clauses,
span).

| line | verdict | note |
|---|---|---|
| Builder 1702 | ALREADY-CORRECT (export-side) — **LATENT** | the one re-export hazard: `ModuleLoader.cpp:2777-2791` builds imported extension `MethodSymbol`s and sets `extensionDecl` but **never `decl`**, so the day a re-exported extension reaches here every method is silently `continue`d and `convertExtension` returns false — the whole block vanishes from the artifact with no diagnostic. §2.8.2 requires such a shape be *rejected with a diagnostic naming it*, not dropped |
| Loader 1356 / 1592 / 1854 / 2279 | ALREADY-CORRECT (body re-parse) | `fresh{Cs,Ds,Ms,Fs}` come from `freshCtx.module()->children` — local symbols of the fresh module, whose decls are then written onto the imported symbols. `:2279` is the one of the four with a *hard reject* on failure (§2.8.2 clause 2) rather than a silent degrade |
| Loader 2118 · Metadata.hpp 158 | *not a site* | prose quoting the backend's `dynamic_cast<const ast::FieldDecl*>(fs->decl)` in `genericInstSfinitFn` |

⚠ Two builder sites carry a fact that is **irrecoverable** if they are ever "unified":
`:991` reads `od->isPure` and `:1004` reads `cd->isPure`, because `SymbolCollection` gives a
local operator/constructor symbol **no** `modifiers` — so the generic
`out.isPure = hasModifier(m.modifiers, "pure")` at `:870` is always false for them. Fixing
§4.3's "`isPure` is imported-only" would also fix this; doing one without the other silently
drops `pure` from every exported operator and constructor.

### A.10 `src/codegen/LlvmBackend_Setup.cpp`

Forty-seven of the 60 sites are ALREADY-CORRECT, and one fact settles twenty of them:
**`ctx_.symbols()` excludes imported symbols outright** (`makeImportedSymbol` appends to a
separate `importedSymbols_` list), so every loop driven by it is a local-walk — `:527`, `:563`,
`:595`, `:1807`, `:1860`, `:1893`, `:2175`, `:2303`, `:2396`, `:2508`, `:2586`, `:3526`,
`:4663`, `:24223`, `:24225`, `:24264`. Another group is `guarded` by an explicit
`ctx_.isImported(...)` early return (`:7768`, `:8042`, `:9183`, `:24111`, `:24244`, `:24254`) or
by a real symbol-side fallback (`:434` `importedConstValue`; `:3156`/`:3217`
`resolvedDeclType`-primary; `:9511`/`:9672`/`:9674`/`:9757`/`:9759`/`:9823`/`:9825`/`:9870`/`:9872`
`importedInstanceFieldInitExpr` / `importedMemberType`; `:14884`, `:17476`, `:19508`, `:19983`,
`:24522`). `:8274`/`:8407` are local via `staticCtors_`; `:15582` has an explicit
`if (ctx.isImported(...)) return false;`.

| line | verdict | kind | question | imported gets today | should read |
|---|---|---|---|---|---|
| 1040 | **UNIFY** | Field (static) | this static field's CONCRETE type at this instantiation | decl null whenever no `fieldInitText` was carried → the re-lower is skipped and `instTy` stays the **ERASED** template type: `owned T?` keeps the `{i8,i32}` placeholder for `T = Cell`, so every load reads a 32-bit address fragment as the value and the pointer's low byte as `hasValue` | `ctx_.importedMemberType(fs)` + `lowerTypeRef` under the installed substitution |
| 1370 | **NEEDS-AST** | Field (static) | the initializer to lower into the per-instantiation global | dereferences `fd->initializer` | — |
| 5750 | **NEEDS-AST** | Method | did the loader reconstruct a body to re-lower here? | correct as written — the next lines lower the reconstructed `ast::MethodDecl` | — |
| 5827 | **UNIFY** | Indexer | "is this accessor imported?" | **always null** — `PropertySymbol::decl` is never set and this function only ever sees imported members, so the documented refusal is **dead code that fails open** | `ctx_.isImported(pimpl)` |
| 6054 | **NEEDS-AST** | Method | same as :5750 | correct | — |
| 9085 | **NEEDS-AST** | Field (static) | does sfinit runtime-store this field, and at what ordinal? | reachable with an imported field via `staticFieldInitIndex`; needs `fd->initializer` and inspects its literal shape | — |
| 9513 | ALREADY-CORRECT (guarded) — **but see §4.2** | Property (auto) | the auto-property's inline initializer | the fallback chain runs, but all three channels are **FIELD-keyed**, so an imported `property X: int := 7` gets neither a value **nor the §2.8.2 diagnostic the field path fires** | extend the three existing tables' population in the loader's Property branch |
| 10480 | **NEEDS-AST** | Ctor | the chained ctor's body to inline | gated on a generic parent, where the loader does attach a decl | — |
| 10569 | **NEEDS-AST** | Ctor | the ctor body to inline | lowers `cd->body` | — |
| 11018 | **NEEDS-AST** | Ctor | is there a real parameterless body for `inlineCtorBody`? | its own comment states the precondition | — |
| 14844 | **UNIFY** | Operator | does this `operator` return `owned`? | `false` → the owned operator result is never registered for unwind or explicit-dispose reclaim → **silent leak** on `var r := a + b` | `ctx_.importedReturnOwned(ms)` — the fallback its sibling at `:14884` already has |
| 15482 | **ALREADY-CORRECT — do NOT unify alone** | Method | does the lowering NULL the caller's transferred-owned slot? | deliberately byte-identical to the lowering's own gate at `LlvmBackend_Calls2.cpp:7905`. That identity is what keeps `eligible ⟹ nulled` exact; unifying this without the Calls2 gate converts a safe leak into a **double free** | — |
| 17415 | **NEEDS-AST** | Property | is this property's getter a +1 string producer? | recurses `isPlusOneStringProducer` on `pd->exprBody`. But for an import the cast is always null → `false` → the caller borrow-retains a string the exporter already minted +1 → **leak** | a composed `importedMemberType` + `importedPropertyIsAutoStorage` accessor (§4.2) |
| 20968 | **NEEDS-AST** | Method | is every returned value a fresh record construction? | inspects `md->exprBody` / `md->stmts` structurally | — |
| 21350 | **UNIFY** | Method | does this method return `owned Array<T>`? | `false` → the unbound borrowing-array temp's `bz_array_free` is never emitted → **leaks the array backing per call** | `ctx_.importedReturnOwned(ms)` |
| 24486 | **UNIFY** | Operator | does this `operator` return `owned`? | identical defect to `:14844`, in the `using var c := a + b` classifier; its sibling arm 36 lines below has the fallback | `ctx_.importedReturnOwned(ms)` |
| 2679 · 6030 | *not a site* | — | prose quoting a decl-cast as a known defect | — | — |

### A.11 `src/codegen/LlvmBackend_Disposal.cpp` · `LlvmBackend_Layout.cpp`

| file:line | verdict | kind | question | imported gets today | should read |
|---|---|---|---|---|---|
| Disposal 35 | **NEEDS-AST** | Destructor | the `ast::DestructorDecl` whose body `emitDisposerCore` lowers | honest for the body consumer — but the **presence-only** consumer at `Setup.cpp:6367` (`findDestructor(ts) != nullptr`, deciding `needsSpecDispose`) then answers "no destructor" for a class that has one. `findDestructor` is two questions wearing one signature; the loop at `:33` has already found the `DestructorSymbol` that answers the boolean | for the presence question: the `DestructorSymbol` |
| Disposal 954 | **UNIFY** | Field | is this an `[N owned]T` field, and how many slots? | `continue` → **every element is never disposed**. The one field-type read in the file with no imported fallback; its sibling `isOwnedReferenceField` already has one | `ctx_.importedMemberType(fs)` |
| Disposal 1102 | **UNIFY** | Event | is this an INSTANCE event whose backing `EventSource` must be freed? | `continue` → **alloc/free ASYMMETRY**: #2421 already unified the ALLOCATION side, so the importer emits one `bz_event_source_new` per instance and **zero** frees | `eventIsStatic(es)` — the both-sources helper |
| Layout 1420 | ALREADY-CORRECT (guarded) | Field | this storage slot's LLVM type | the `decl == nullptr → importedMemberType → lowerTypeRef` fallback is ordered **before** this cast | — |
| Layout 1422 | ALREADY-CORRECT (guarded) | Property (auto) | same | same pre-ordered imported branch | — |
| Layout 4534 | ALREADY-CORRECT (guarded) | Property | property thunk's return type | `else if (ctx_.importedMemberType(ps))` on the next line | — |

### A.12 `src/codegen/LlvmBackend_Calls2.cpp` · `LlvmBackend_impl.hpp` · `LlvmBackend_String.cpp`

| file:line | verdict | kind | question | imported gets today | should read |
|---|---|---|---|---|---|
| Calls2 1346 | **UNIFY** | Method | is argument position an `out owned` transfer? | `false` → the callee's +1 is never registered → **leak**, unwind slot unarmed | `ParameterSymbol::modifier` |
| Calls2 1693 | ALREADY-CORRECT (guarded) | Method | out-var slot type | `resolvedDeclType` arm (#2657) | — |
| Calls2 1950 | ALREADY-CORRECT (guarded) | Ctor | which ctor's formals size the out-var slots? | `css`/`ctorSym` symbol-side fallback (#2639) | — |
| Calls2 2669 | ALREADY-CORRECT (guarded) | Event | the event's payload type | `ctx_.importedMemberType(es)` (#2421) | — |
| Calls2 4311 | ALREADY-CORRECT (guarded) | Field | is this callee a function-typed field? | `ctx_.importedMemberType(fs)` (#2300) | — |
| Calls2 4408 | ALREADY-CORRECT (guarded) | Property | is this callee a function-typed property? | `ctx_.importedMemberType(ps)` (#2589) | — |
| Calls2 7790 | **UNIFY** | Method | does this call's declared return carry `owned`? | `registerFreshNewReceiverTempUnwind` armed wrong → **leak** | `ctx_.importedReturnOwned(ms)` |
| Calls2 7821 | **UNIFY** | Method | method-level `<T>` + formals' declared types | pre-pass skipped → a lambda argument lowers at the i32 placeholder → `ret void` in an i32 function → **IR-verifier failure** | `ms->genericParams` + `resolvedDeclType` |
| Calls2 7915 | ALREADY-CORRECT (guarded) | Method | ref/out argument shaping | `else` imported arm reads `ParameterSymbol::modifier` (#1221/#2462) | — |
| Calls2 8386 | **UNIFY** | Method | is formal *pi* a bare type parameter (⇒ re-box, §8.6)? | not boxed → the specialized signature is built from the unboxed type while the body is compiled with `T`=interface → #2477 zero-init → **SEGV on 0x8** | `ParameterSymbol::{modifier, resolvedDeclType}` |
| Calls2 8442 | **NEEDS-AST** | Method | is the callee a sync/async ITERATOR? | dereferences `mdIt->stmts` (`containsYield`). Fails **closed** (a leak, never a UAF) — do not guess from the return type | needs the new "contains yield" bit (§4.2) |
| Calls2 8514 | **NEEDS-AST** | Method | §28.14 deferred-activation frame / sync iterator? | dereferences `mdCb->stmts` + `returnType`; same safe-direction posture | same |
| Calls2 8745 | **UNIFY** | Method | declared return `owned`? | `disposeFreshNewReceiverTemp` mis-gated → **leak** | `ctx_.importedReturnOwned(ms)` |
| Calls2 8796 | **UNIFY** | Method | method-level `<T>`, formals, declared return | early `return` → **no method-generic substitution at all** | `genericParams`, `resolvedDeclType`, `importedReturnType` |
| Calls2 9256 · 9377 · 9922 | ALREADY-CORRECT | Method | did the loader re-parse a body I can monomorphize? | decl-presence **is** the predicate's subject (#2821) | — |
| Calls2 9471 | **UNIFY** | Method | is a method-level type param bound in the active substitution? | `false` → the fixed vtable slot holding the i32-erased template is called → wrong-width argument, **silent miscompile** (partly masked by #2896's fallback, which covers only a generic *parent*) | `ms->genericParams` |
| Calls2 9530 | **UNIFY** | Method | does the declared return differ from the erased `fnTy` return? | `needsInline` stays false on the return axis | `ctx_.importedReturnType(ms)` |
| Calls2 10006 | **UNIFY** | Method | does the concrete return differ from the compiled signature's? | block skipped → the placeholder template is called | `ctx_.importedReturnType(ms)` |
| Calls2 10707 | **UNIFY** | Method | how many/which generic params does the statically declared method have? | `return nullptr` → **§580 virtual-generic dispatch disabled wholesale** for an imported declaring interface → the i32-erased itable slot | `ms->genericParams` |
| Calls2 10852 | ALREADY-CORRECT (guarded) | Method | `MethodDecl` vs `OperatorDecl`? | `callableInfo` one line above already returned false for a decl-less symbol | — |
| Calls2 11416 | **UNIFY** | Method | does the `?.`-called method declare `<T>`? | the `{i8,T}` wrap width / ref-vs-value decision is made from the unspecialized return | `ms->genericParams` |
| Calls2 11521 | ALREADY-CORRECT (guarded) | Method | ref/out shaping for `?.` | `else` imported arm (#1221/#2462) | — |
| Calls2 11689 | ALREADY-CORRECT (guarded) | Method | the callee's LLVM `FunctionType` | full symbol-side reconstruction from `importedReturnType` + `ParameterSymbol::{modifier,resolvedDeclType}` (#328). **The model the other Calls2 sites should copy** | — |
| Calls2 1932 | *not a site* | — | comment text in `ensureCtorOutVarSlots`' #2639 note | — | — |
| impl.hpp 2546 · 2551 | **NEEDS-AST** | Method/Operator | `callableInfo` — params, return, **and body** | dereferences `stmts`/`exprBody`. ⚠ it fails **wholesale**: callers needing only `ci.params`/`ci.returnType` (`Setup.cpp:5753`, `:6774`, `:6864`) are refused for an imported symbol even though the symbol could answer. Splitting it into a signature half and a body half unblocks the most sites for the least risk | — |
| impl.hpp 2653 | ALREADY-CORRECT (guarded) | Indexer | can this be an itable slot? | explicit `if (ps->decl) return false;` (#2449) | — |
| impl.hpp 2694 | ALREADY-CORRECT (guarded) | Property | does this property declare a setter? | `ctx_.importedPropertyHasSetter` (#2406) — ⚠ but see drift #2 | — |
| impl.hpp 4494 | **UNIFY** | Property | is this an auto-property (storage / disposable slot)? | `false` for every import, at 7 call sites, **none** widened — while `collectInstanceStorage` used the *fixed* twin and already gave the member a slot. See drift #3 | `isAutoPropertyStorage` |
| impl.hpp 4510 | **UNIFY** | Property | is this a STATIC auto-property? | `false` → `declareStatics` allocates no global and `staticFieldGlobal` does not route the get/set → **no storage at all** | `ps->modifiers` + a new bit (§4.2 — the exporter force-clears `isAutoProperty` for statics) |
| impl.hpp 4601 · 4608 | ALREADY-CORRECT (guarded) | Event | is the event `static` / `async`? | `ctx_.importedEventIsStatic/IsAsync` (#2421) | — |
| impl.hpp 5797 | ALREADY-CORRECT (guarded) | Indexer | the indexer's vtable slot key | `else` arm walks `indexerParams`→`resolvedDeclType` (#2545) | — |
| impl.hpp 6189 · 6222 · 7918 · 8000 · 8123 · 8189 · 8225 · 8254 · 8310 | ALREADY-CORRECT (guarded) | Field | the eight `owned`-field predicates | every one has the `if (!fs->decl) rt = ctx_.importedMemberType(fs)` branch (#662 foundation B) | — |
| impl.hpp 8918 | **UNIFY** | Method | does the declared return name `IAsyncEnumerable<T>`/`IAsyncEnumerator<T>`? | `if (!ms->decl) return false` is a **bail, not a fallback** → misrouted to `emitAsyncMethod`, and `await for` loads a vtable out of a `bz_task*` → **SIGSEGV** (the function's own comment states the mechanism) | `ctx_.importedReturnType(ms)` |
| impl.hpp 9757 | **UNIFY** | Ctor | which same-arity ctor overload do these argument types select? | every imported candidate `continue`s → falls to `candidates[0]` → **WRONG OVERLOAD**, arguments emitted against the other ctor's types | `cs->params` → `resolvedDeclType` |
| impl.hpp 9780 | **UNIFY** | Ctor | can these named/defaulted args fill this ctor? | `ctorForArgs` yields **nullptr** for any imported class constructed with named or defaulted arguments | `ParameterSymbol::name` + the dropped `hasDefault` (§4.2) |
| impl.hpp 4520 · 4581 | *not a site* | — | comment text in the #2478 / #2421 headers | — | — |
| String 2601 | **UNIFY** | Field | a storage field's declared type | the #2874 fallback's gate `if (!fs->posParam && !fs->decl)` **excludes the very case its own comment names** — an imported positional field has a synthetic `posParam` with a null `type` | `sema::storageComponentType` |
| String 2632 | **UNIFY** | Property | a record's body auto-property storage type | the property arm has **no** `!decl` fallback → `nullptr` at 12 call sites | `sema::storageComponentType` |

### A.13 `src/codegen/LlvmBackend_Assign.cpp` · `_Exprs.cpp` · `_Exprs2.cpp` · `_Stmts.cpp` · `_Serialization.cpp`

| file:line | verdict | kind | question | imported gets today | should read |
|---|---|---|---|---|---|
| Assign 38 · 52 · 80 | ALREADY-CORRECT (guarded) | Indexer | index-param type / arity / element type | `indexerParams`→`resolvedDeclType`, `importedMemberType` (#2414/#2528) | — |
| Assign 1550 · 6161 | ALREADY-CORRECT (guarded) | Property | "is this IMPORTED?" | the null cast **is** the imported arm being written (#2832/#2862). ⚠ exact for properties only — see the note below | — |
| Assign 1572 · 2187 · 2811 · 4537 · 5595 | ALREADY-CORRECT (guarded) | Property/Indexer | local AST body vs imported ABI stub | `importedIndexer{Getter,Setter}Fns_` / `importedProp{Getter,Setter}Fns_` (#2414/#2387/#2400/#2449/#2509) | — |
| Assign 4053 · 4073 · 5607 | **NEEDS-AST** | Property | the arrow/block accessor's **body** | callers fall through to the imported-stub path | — |
| Assign 5319 | **UNIFY** | Indexer | which fn fills this indexer's getter slot? | `return nullptr` → the vtable/itable slot stays `ConstantPointerNull` → **indirect call to 0x0** | `importedIndexerGetterFns_[ps]` — what `indexerItableAccessor` at `:5595` already does |
| Assign 5467 | **UNIFY** | Indexer | same, setter slot | same **SIGSEGV** on assignment through the interface | `importedIndexerSetterFns_[ps]` |
| Assign 5638 · 9147 | ALREADY-CORRECT (guarded) | Property | the setter's node / thunk box type | unreachable: the caller already proved a non-null decl. ⚠ `:9147` is **latent** — fixing `:9286` makes it reachable and it then needs `importedMemberType` | — |
| Assign 9196 | **UNIFY** | Method | `GetEnumerator`'s return type | `return src` — the **raw** instance is handed back and driven through the §29.11 enumerator vtable (#2384 records this as a confirmed ASan `global-buffer-overflow`) | `ctx_.importedReturnType(getEnumSym)` |
| Assign 9286 | **UNIFY** | Property | which fn reads the enumerator's `Current`? | `currentFn == nullptr` → same raw-pointer hand-back | `importedPropGetterFns_[ps]` |
| Assign 10512 · 10520 | **UNIFY** | Operator/Method | does this return `owned`? | `isOwnedTempArg = false` → the temporary leaks on the **normal** path, not just on unwind | `ctx_.importedReturnOwned(ms)` |
| Exprs 2197 · 5268 · 5661 · 5973 | ALREADY-CORRECT (guarded) | Property | the getter's decl node | unreachable: the enclosing `blockGetterOf`/`ncGetter` already proved non-null | — |
| Exprs 2370 · 5016 · 5869 · 6185 | ALREADY-CORRECT (guarded) | Property | slot / declared / virtual accessor type | `ctx_.importedMemberType(ps)` (#3016/#1187/#503/#2406) | — |
| Exprs 6650 | **UNIFY** | Operator | does the chained `operator` return `owned`? | `false` → the intermediate owned temporary is never disposed. **13 lines above its own fixed sibling** | `ctx_.importedReturnOwned(ms)` |
| Exprs 6663 | ALREADY-CORRECT (guarded) | Method | does the called method return `owned`? | `(!md && ctx_.importedReturnOwned(ms))` | — |
| Exprs 6674 | **UNIFY** | Operator | is the operator's *i*-th parameter `owned`? | `paramConsumes(i)` is **always false** → the guard that exists to prevent a **double free** is silently disarmed | `ParameterSymbol::modifier` |
| Exprs2 2695 | **NEEDS-AST** | Ctor | the leading `base(...)` chain call's **argument expressions** | on the recursive call both sources die, so for `LocalD : ImportedB : ImportedA(x)` the grandparent's positional fields stay calloc-zero — #3141 one level deeper | the exporter carrying base-call argument texts (§4.2) |
| Exprs2 5072 · 5196 · 5717 | ALREADY-CORRECT | Ctor | ctor params/body for the inline path | local-walk (`else if (!ctx_.isImported(t))`) / body re-parse (`cs->decl != nullptr` selection) | — |
| Exprs2 6917 · 6919 | **UNIFY** | Field/Property | this storage member's **default initializer** | the member keeps the `ConstantAggregateZero` → `new ImportedRecord()` reads back all zeros. A line-for-line clone of `emitOwnFieldInit` that never grew the imported fallbacks | `ctx_.importedInstanceFieldInitExpr(m)` then `…InitText` |
| Exprs2 7257 | **UNIFY** | Ctor | how do I run this record's explicit ctor? | `return nullptr` → **the entire `new ImportedRecord(...)` lowers to nothing**. The class path was made loud for exactly this residual (#2658); the record path has no such guard | `declareImportedCtor(cs)` + `cs->params` |
| Exprs2 5657 | *not a site* | — | comment text in the #2658 rationale block | — | — |
| Stmts 7175 | **UNIFY** | Method | `GetEnumerator`'s return type | `return false` → `for-in` falls through every handler to a hard "not yet implemented" error. ⚠ **probe M15 did not isolate this** — the local twin fails identically | `ctx_.importedReturnType(getEnumSym)` |
| Stmts 7356 · 7362 | **UNIFY** | Method/Property | `Current`'s type ⇒ loop-var LLVM type | `elemTy` stays `ptrTy()`; rescued only when `GetEnumerator` returns the generic `IEnumerator<T>` spelling | `importedReturnType` / `importedMemberType` |
| Stmts 7411 | **UNIFY** | Property | which fn reads `Current`? | `curVal` stays null → **nothing is ever stored into the loop variable**; the loop still spins on `MoveNext` over a constant | `importedPropGetterFns_[currentPropSym]` |
| Serialization 156 | **UNIFY** | Field | is this component `owned`? (⇒ back-edge? tree-mode-excluded?) | the `else` at `:235` sets only `wireName`+`ty`, so `isOwned=false` **and the back-edge classification is skipped entirely** | a new `owned` component bit + a both-sources accessor (§4.2) |
| Serialization 182 | **UNIFY** | Property | the `serialized` clause: skip / optional / rename | `isSkip=false`, `isOptional=false`, `wireName = declName` → a renamed component is written under the **wrong name**; a `skip` component is written at all, desyncing the field count in `bz_binser_begin_object` from the writes | new `serializedWireName`/`Skip`/`Optional` + one accessor, on the #3140 template |
| Serialization 395 · 429 · 871 · 1308 · 1324 · 1335 · 1338 · 4029 · 4050 | ALREADY-CORRECT (local-walk) | Property/Ctor/Field | body-record and class component/ctor bijection | `emitRecordSerialize`/`-Deserialize`/`emitClassDeserialize` all early-return on `!ts->decl`, and §9.6 forbids a record base class | — |

⚠ **A note on the `decl != null` ⇒ "is LOCAL" idiom** (`Assign.cpp:1550`, `:6161`). That is
exact **for properties**, because `PropertySymbol::decl` is never set on import. It is **not**
exact for methods, constructors, destructors or static fields, where the loader's four
schema-v5 assignments make `decl != null` for a GENERIC import. Copy-pasting this idiom onto a
`MethodSymbol` would misfire silently on generic imports.
