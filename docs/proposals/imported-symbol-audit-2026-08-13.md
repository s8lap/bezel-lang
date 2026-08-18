# The imported-symbol classification seam — a complete triage

**Status:** audit, read-only. No `src/` change was made and nothing here was reproduced on
HEAD. **Every claim below is derived by reading the code**; a fix lane must verify-first with
a real two-module build before changing anything (lane contract §6 — issues in this tracker
routinely *understate* the defect, and they occasionally overstate it).

**Base:** `main` @ `701cb2d`, spec v0.252.
**Umbrella issue:** #3152. **Family issues:** #3139–#3147, plus #3149, #3150, #3151.

> **Lane contract — re-verified 2026-08-18 by issue #3168.** Per-row line
> numbers in §5 drift on every `src/codegen/LlvmBackend_*.cpp` change,
> and **the line label itself is the untrustworthy part of every row.**
> Six of F6's 22 rows at this revision (Calls.cpp:8622, Calls2.cpp:8062,
> Calls2.cpp:8315, Layout.cpp:1721, Exprs2.cpp:4731 — the `lowerNew`
> site — and Layout.cpp:5630 — the `fieldAddr` site) had been wrong about
> "what an imported symbol gets today" because the audit described the
> pre-issue 3143 shape but the unified accessor introduced by issue 3143
> is already in place at the content-addressable site; all six are now
> ALREADY-CORRECT. A further three rows (Exprs.cpp:5110, Layout.cpp:4632,
> Setup.cpp:7753) had audit line labels that did not even point at the
> construct the audit was describing: `:5110` is now inside Exception
> `CauseChain` handling, `:4632` is a blank line inside
> `Emitter::makePropertyThunk`, and `:7753` is inside
> `Emitter::rejectCollapsedTypeTestTarget`. **A reader must re-locate a
> site by content (grep for the construct, not the line number) before
> relying on any row.** Two of those three re-locate to sites that are
> REACHABLE for an imported generic property — the #2884 sema-time gate
> does not stop codegen from walking the AST — and use the unified
> accessor (ALREADY-CORRECT). One re-locates to the
> `templateGenericParamNames_` block inside `Emitter::emitFunctionBody`,
> which the `isImported(ms)` early-return makes unreachable for imports
> (UNREACHABLE for the right reason). The cheap check that catches the
> wrong-line-label class: read the line and confirm it is the construct
> the audit row describes; if not, grep for the construct (e.g.
> `grep -n 'genericParamNames' <file>` for the F6 family) and re-derive.
> This note is to be re-read against HEAD on every lane briefing; the
> row-level corrections it documents are inline at the rows themselves.

---

## 1. Headline

| verdict | count | meaning |
|---|---|---|
| **UNIFY** | **94** | reads a fact the `TypeSymbol` carries, or could carry. **These are the bugs.** |
| **NEEDS-AST** | **3** | dereferences something only an AST can supply. Leave alone. |
| **ALREADY-CORRECT** | **60** | guarded with a symbol-side fallback, or on a path no imported symbol reaches. |

157 distinct decision sites (159 grep hits: one is a comment, one statement casts twice).

**Only 3 of 157 sites legitimately need the declaration.** The seam is not "the AST is
required here"; it is "the AST was the first thing to hand".

### Family ranking, by blast radius

| # | family | sites | a wrong answer is | issue |
|---|---|---|---|---|
| 1 | §9.7 borrow-bearing record | 4 | **ownership unsoundness** — the `new Record(<temporary>)` dangling-borrow reject never fires | #3145 |
| 2 | generic param names / arity / variance | 22 | wrong layout / SEGV (per the source's own comment) + 2 false diagnostics. **No new field needed** | #3143 |
| 3 | §9.8 serialization surface | 22 | **silent wrong DATA** — an imported record serializes to nothing, with no error | #3140 |
| 4 | positional / primary-ctor field list | 16 | **stack corruption** (the `new` arg check is skipped) + calloc-zero construction | #3141 |
| 5 | built-in-interface conformance | 12 | false diagnostic; one codegen IR-verifier failure | #3139 |
| 6 | choice case list | 8 | leak (an owning payload is never disposed); **blocked on schema** | #3144 |
| 7 | bare `kind` read through the AST | 3 | **two cross-module leaks**; three one-line fixes | #3142 |
| 8 | §14.2 generic constraints | 3 | silent accept of an ill-formed instantiation | #3146 |
| 9 | one-site residues | 3 | missed narrowing, missed §14.4 reject | #3147 |
| 10 | record auto-equality classification | 1 | already filed | #3132 |

Ranking is by *severity of a wrong answer*, not by site count. Family 2 has the most readers
but almost all of them fail loudly (an IR-verifier error or a mis-typed field); family 1 has
four readers and one of them is a **safety check that silently does not run**.

---

## 2. The root cause, precisely

`ModuleLoader` builds every imported `sema::TypeSymbol` in two places
(`reconstructTypeSymbol` at `:325`, `loadMetadataPass1` at `:2433`). Neither assigns
`Symbol::decl`. There is no `ast::TypeDecl` to assign — the importing compilation has no AST
for a dependency's types at all.

So:

```cpp
    dynamic_cast<const ast::TypeDecl*>(ts->decl)   // null for EVERY imported type
```

This is not a null-dereference risk — every site guards. It is a **silent answer change**:
the guard's `false` branch is written as "not a positional record" / "no `serialized` clause" /
"no generic params" / "not borrow-bearing", and each of those is a *substantive claim* that
happens to be wrong.

The 2026-08-06 taxonomy names all three shapes this produces, and all three are present here:

- **positive-list missing** → loud reject (family 5: `ISerializable` conformance).
- **negative-list "not X ⇒ assume Z"** → silent accept (family 1: `!decl → not borrow-bearing`).
- **a correct predicate fed by an empty source** → invisible (family 6: an imported choice has
  `kind == Choice` and zero cases; `isAutoPropertySymbol` returns `false` for every imported
  property, so `isAutoEqualityBearingBodyRecord` checks *no* components and returns `true`).

---

## 3. What the `TypeSymbol` already carries

This is the load-bearing table. Everything in the left column is available on **both** sides
today and needs no schema work:

| fact | local source | imported source |
|---|---|---|
| `kind` | `SymbolCollection::collectTypeDecl` | `fromMetaTypeDeclKind` (Class/Interface/Record/Enum/Choice) |
| `visibility`, `modifiers`, `name` | ditto | `ModuleLoader.cpp:328–329` |
| `hasPositionalFields` | ditto | `MetaTypeDecl::hasPositionalFields` (#501) |
| `genericParams` (name + variance) | `SymbolCollection.cpp:293` | `convertGenericParams` (`:306`) |
| `baseTypes` / `baseTypeRefs` | `TypeResolution_Names.cpp:604` | `ModuleLoader.cpp:346`, `:2482` ⚠ see §6 |
| `members` | ditto | `convertMember` |
| `moduleName` | `collectTypeDecl` | `md.moduleName` (#1488) |
| `builtinExceptionBase` | `TypeResolution_Names.cpp:596` | `MetaTypeDecl::builtinExceptionBase` (#2950) |
| `invariantContracts` | derived from the AST | loader side table (#2054) |
| a field's positional-ness | `FieldSymbol::posParam → td.positionalFields[i]` | a **synthetic** `ast::Param` (#2516) — `posParam != nullptr` works, `posParam->type` is null |
| a field's / member's type | the AST type | `ctx.importedMemberType(m)` |
| an enum constant's value | folded from the AST | `EnumConstantSymbol::value` |

And the facts that have **no symbol-side channel at all** — the ones a family needs new
carriage for:

| fact | why it is missing |
|---|---|
| declared built-in-interface bases (`ISerializable`, `IComparable`, `IFormattable`, `IDisposable`, `IEnumerable`) | a built-in interface has no user `TypeSymbol`, so `TypeResolution_Names.cpp:604`'s `if (ref->declaration)` guard drops it from **both** symbol lists on the LOCAL side too — and `MetaTypeDecl::baseTypes` is built from `baseTypeRefs`, so it never reaches the wire |
| `serializedClass` (the §9.8 clause + poly registry) | no `TypeSymbol` field, no `MetaTypeDecl` field |
| §14.2 generic constraints | **on the wire** (`MetaGenericParam::constraintTypes` / `specialConstraints`) and **dropped at load** — `GenericParamSymbol` has no fields for them |
| choice cases + payloads | `MetaTypeDeclKind::Choice` crosses; the case list explicitly does not ("cross-module choice consumers are not in scope yet") |
| `primaryBaseCtorArgs` | arbitrary argument **expressions** — genuinely NEEDS-AST |
| §22.16 invariant *expressions* | only canonical TEXT crosses; not lowerable |

---

## 4. The pattern that works — reuse these, don't write a sixth

Four accessors already do this correctly. Every UNIFY site is a site that should be calling
one of them, or a sibling that does not exist yet.

- **`recordIsPositional` / `recordIsAutoBearing`** — `LlvmBackend_impl.hpp:5221`. Reads
  `hasPositionalFields`, then falls back to the AST. Its comment records the measured cost of
  reading only the AST: an imported `record Pt(X: int, Y: int)` rendered `"Dep.Pt"` (a
  qualified record head, which §8.5 forbids) while the boxed and interpolated spellings of the
  same value in the same program rendered `"Pt(X: 1, Y: 2)"` and `"Pt"`.
- **`builtinExceptionBaseOf`** — `SymbolCollection.cpp:505` (#2950). **This is the template
  for every family that needs a new channel**: a `TypeSymbol` field + a `MetaTypeDecl` record +
  one accessor whose walk *"must not require an ancestor to have an `ast::TypeDecl` (an
  imported one never does)"*.
- **`genericParamNamesOf`** — `LlvmBackend_Calls2.cpp:8927`, and **`RecordSubstGuard`**'s
  imported branch — `LlvmBackend_Layout.cpp:2097` (#428).
- **`sema::enumDeclaredValues`** — `EnumValues.hpp:59`.

### The rule for the sweep

⚠ **Do not partially unify.** #1462 attempted exactly this kind of centralisation partially
and **turned a leak into a SIGSEGV**; it had to be reverted. A family lands whole or not at
all, and the fix lane converts *every* site in its family's table in one commit.

⚠ **An accessor must consult both sources, not switch on which one exists.** The two
`recordTypeOfRef`-vs-`lowerValueSemMethodOn` divergences and the §8.5 rendering split both
came from two copies of one predicate reading different sources; the fix is one predicate, not
two synchronised ones.

---

## 5. The triage

Verdicts are per the brief's rubric: **UNIFY** = reads a flag/list the `TypeSymbol` carries or
could carry; **NEEDS-AST** = goes on to dereference something only an AST holds;
**ALREADY-CORRECT** = guarded, or unreachable by an imported symbol (the reason is given).

`currentType_` / `currentOuterType_` / `ctx_.module()->children` sites are ALREADY-CORRECT for
one shared reason: they are *declaration-site* walks over the module being compiled.
`ModuleLoader`'s re-parsed ctor/method bodies are sema-checked against a **fresh** module whose
`TypeSymbol`s do have decls (`ModuleLoader.cpp:1284–1288`), and are only rebound to the
imported symbols afterwards — so `currentType_` is never an imported symbol.

### Family legend

| code | family | issue |
|---|---|---|
| F1 | built-in-interface conformance (`ISerializable`, `IDeserializable`, `IComparable`, `IFormattable`, `IEnumerable`) | #3139 |
| F2 | §9.8 `serialized` class clause / serialization surface | #3140 |
| F3 | positional-record / primary-constructor field list | #3141 |
| F4 | enum constants + serialized wire names | #3140 |
| F5 | bare `TypeDeclKind` read through the AST | #3142 |
| F6 | generic parameter names / arity / variance | #3143 |
| F7 | record auto-equality classification | #3132 |
| F8 | primary base-constructor argument expressions (**NEEDS-AST**) | — |
| F9 | choice case list + payload fields | #3144 |
| F10 | §9.7 borrow-bearing record | #3145 |
| F11 | built-in exception base | #3147 |
| F12 | member-shape reads at a use site | #3147 |
| F13 | §14.2 generic constraints | #3146 |
| F14 | type modifiers (`sealed`/`abstract`/`closed`) | #3147 |

### Site table


#### `src/codegen/LlvmBackend_Assign.cpp`

| line | verdict | family | question it answers | what an imported symbol gets today | what it should read |
|---|---|---|---|---|---|
| 10064 | **UNIFY** | F5 | `out owned` DISCARD: is the param type a class/interface? | false -> the scratch slot is neither nulled nor registered for disposal -> LEAK | `pt->declaration->kind` |

#### `src/codegen/LlvmBackend_Calls.cpp`

| line | verdict | family | question it answers | what an imported symbol gets today | what it should read |
|---|---|---|---|---|---|
| 709 | **UNIFY** | F6 | setupReceiverGenericSubst | returns the saved subst unchanged (`!parent->decl` early-out) | genericParamNamesOf |
| 758 | **UNIFY** | F7 | recordTypeOfRef: does the record carry auto equality? | false -> routed off the record-key fast path (#3132) | recordIsAutoBearing(ts) |
| 8622 | **ALREADY-CORRECT** | F6 | generic substitution | MODEL ACCESSOR `sema::genericParamNames(cmpMs->parent)` — already the one both-sources walk introduced by #3143 (the line-number drifted past 8700 by 2026-08-18); the "no subst" claim was the pre-#3143 shape, but the unified accessor was applied at the same logical site | - |

#### `src/codegen/LlvmBackend_Calls2.cpp`

| line | verdict | family | question it answers | what an imported symbol gets today | what it should read |
|---|---|---|---|---|---|
| 8062 | **ALREADY-CORRECT** | F6 | generic substitution | MODEL ACCESSOR `sema::genericParamNames(ms->parent)` — already the one both-sources walk introduced by #3143; the "no subst" claim was the pre-#3143 shape, but the unified accessor was applied at the same logical site (line-number drifted past 8400 by 2026-08-18) | - |
| 8315 | **ALREADY-CORRECT** | F6 | generic substitution | MODEL ACCESSOR `sema::genericParamNames(ms->parent)` — already the one both-sources walk introduced by #3143; the "no subst" claim was the pre-#3143 shape, but the unified accessor was applied at the same logical site (line-number drifted past 8650 by 2026-08-18) | - |
| 8669 | **UNIFY** | F6 | generic substitution | no subst | genericParamNamesOf |
| 8927 | **ALREADY-CORRECT** | — | - | MODEL ACCESSOR `genericParamNamesOf` -- AST first, `ts->genericParams` fallback | - |
| 9996 | **UNIFY** | F6 | emitMethodCall: is the parent class generic (=> monomorphise)? | false -> the body is NOT monomorphised; static-field globals resolve to the unspecialised symbol | genericParamNamesOf(ms->parent) |

#### `src/codegen/LlvmBackend_Disposal.cpp`

| line | verdict | family | question it answers | what an imported symbol gets today | what it should read |
|---|---|---|---|---|---|
| 2078 | **UNIFY** | F9 | owning-choice active-case disposal: the choice decl for §28.1 payload substitution | null -> substitutedChoicePayloadType has no declaration -> owned payload not disposed | symbol-side case/payload list |
| 2198 | **UNIFY** | F9 | value-like choice string-payload release | null -> +1 payload not released | symbol-side case/payload list |
| 2613 | **UNIFY** | F5 | freshNewReceiverTempEligible: is the result an enum? | false -> not classified valueResult | `resultTy->declaration->kind` |
| 3674 | **UNIFY** | F6 | emitBaseDisposeChain: base's generic params for the specialised base disposer | null -> no specialisation; per the site's comment this reads the WRONG offset (SEGV) | genericParamNamesOf(base) |

#### `src/codegen/LlvmBackend_Exprs.cpp`

| line | verdict | family | question it answers | what an imported symbol gets today | what it should read |
|---|---|---|---|---|---|
| 2216 | **UNIFY** | F6 | generic substitution | no subst | genericParamNamesOf |
| 5110 | **ALREADY-CORRECT** | F6 | property-getter generic substitution (re-located by content 2026-08-18) | The audit's `:5110` line label is stale; what's actually at `Exprs.cpp:5110` on HEAD is `i8Ty, excObj, builder_.getInt64(kExcCauseOffset));` inside the Exception `CauseChain` accessor — not a property-getter site at all. The content the audit MEANT is the `savedSubst = genericSubst_` block in `Emitter::lowerExprInner` (now at `LlvmBackend_Exprs.cpp:5304-5423`), which carries `sema::genericParamNames(ps->parent)` at `:5320` — the unified accessor from issue 3143, so ALREADY-CORRECT. The site IS reachable for an imported generic property: `#2884`'s `declineUnsupportedImportedPropertyAccess` (the sema-time gate at `ExprTypecheck_Calls.cpp:12678`) does not gate codegen, which walks the AST and reaches `lowerExprInner` regardless; the audit's earlier "no subst" claim was the pre-issue 3143 shape, but the unified accessor is in place at the content-addressable site today | - |
| 6585 | **UNIFY** | F6 | generic substitution | no subst | genericParamNamesOf |

#### `src/codegen/LlvmBackend_Exprs2.cpp`

| line | verdict | family | question it answers | what an imported symbol gets today | what it should read |
|---|---|---|---|---|---|
| 351 | **UNIFY** | F6 | lowerUnary generic substitution | no subst | genericParamNamesOf |
| 2646 | **UNIFY** | F3 | prePopulateBasePositionalFields: base's positional fields | not pre-populated | positional FieldSymbols |
| 2668 | **NEEDS-AST** | F8 | the base-call argument expressions (`primaryBaseCtorArgs`) | arbitrary expressions; no symbol channel | - |
| 4731 | **ALREADY-CORRECT** | F6 | `lowerNew` generic substitution | MODEL ACCESSOR `sema::genericParamNames(t)` — already the one both-sources walk introduced by #3143 (the line-number drifted past 5060 by 2026-08-18); the "no subst" claim was the pre-#3143 shape, but the unified accessor was applied at the same logical site, including the `#732` `allUnbound` guard that used to live only on the local arm | - |
| 4938 | **UNIFY** | F3 | `new C(x)` primary-ctor field stores | skipped -> calloc-zero object (the class twin of #3130) | positional FieldSymbols |

#### `src/codegen/LlvmBackend_Layout.cpp`

| line | verdict | family | question it answers | what an imported symbol gets today | what it should read |
|---|---|---|---|---|---|
| 1128 | **UNIFY** | F6 | generic substitution / specialised struct selection | the i32 placeholder layout is used for an imported instantiation | genericParamNamesOf |
| 1233 | **UNIFY** | F6 | generic substitution / specialised struct selection | the i32 placeholder layout is used for an imported instantiation | genericParamNamesOf |
| 1721 | **ALREADY-CORRECT** | F6 | generic substitution / specialised struct selection | MODEL ACCESSOR `sema::genericParamNames(base)` — already the one both-sources walk introduced by #3143 (the line-number drifted past 1778 by 2026-08-18); the "i32 placeholder" claim was the pre-#3143 shape, but the unified accessor was applied at the same logical site | - |
| 2097 | **ALREADY-CORRECT** | — | - | MODEL ACCESSOR -- RecordSubstGuard has the explicit `else if (!rtd && rt->declaration->genericParams...)` imported branch (#428) | - |
| 4632 | **ALREADY-CORRECT** | F6 | property-getter generic substitution / specialised struct selection (re-located by content 2026-08-18) | The audit's `:4632` line label is stale; what's actually at `Layout.cpp:4632` on HEAD is a blank line between `auto savedSubst = genericSubst_;` and `auto savedSemaTy = genericSubstSemaTy_;` inside `Emitter::makePropertyThunk` (defined at `Layout.cpp:4578`) — not itself a substitution site, but inside the function the audit row was describing. The substitution the row MEANT is at `Layout.cpp:4699` (`sema::genericParamNames(ps->parent)`), the unified accessor from issue 3143, so ALREADY-CORRECT. The site IS reachable for an imported generic property: `makePropertyThunk` is invoked from `LlvmBackend_Exprs.cpp:5225`, which is not gated on `hasErrors()` (only the final `emitModule` checks errors at `LlvmBackend_impl.hpp:532`), so codegen walks the AST and reaches it whether or not the sema-time decline fired | - |
| 5630 | **ALREADY-CORRECT** | F6 | `fieldAddr` generic substitution / specialised struct selection | MODEL ACCESSOR `sema::genericParamNames(ms->parent)` — already the one both-sources walk introduced by #3143 (the line-number drifted past 5705 by 2026-08-18); the "i32 placeholder" claim was the pre-#3143 shape, but the unified accessor was applied at the same logical site, including the `#828`/`#2014` `hasUnresolved` recovery that used to live only on the AST arm | - |

#### `src/codegen/LlvmBackend_Serialization.cpp`

| line | verdict | family | question it answers | what an imported symbol gets today | what it should read |
|---|---|---|---|---|---|
| 49 | **UNIFY** | F1 | `declaresConformance(ts, name)` -- does the type declare `: ISerializable`? | always false (also early-returns on `!ts->decl`) | walk a symbol-side built-in-interface list |
| 137 | **UNIFY** | F2 | collect a class's serialized components | early-returns -> EMPTY component list; `td` is a bare guard, no `td->` read in the body | drop the guard; walk symbols |
| 300 | **UNIFY** | F2 | is the `Shared<C>` element a serialization-eligible class? | false (no `serializedClass`) | read the symbol-side shape |
| 328 | **UNIFY** | F2 | same, `Weak<C>` element | false | read the symbol-side shape |
| 363 | **UNIFY** | F3 | emitRecordSerialize: positional components | returns early -> writes NOTHING | positional FieldSymbols + importedMemberType |
| 690 | **UNIFY** | F2 | emitSerializeField: owned class component has a clause? | false -> falls to the null-marker fallback | symbol-side shape |
| 734 | **UNIFY** | F4 | emitEnumSerialize: constants + wire names | returns early -> writes NOTHING | enumDeclaredValues + a wire-name channel |
| 827 | **UNIFY** | F3 | emitRecordDeserialize: positional components | returns early -> reads NOTHING | positional FieldSymbols |
| 1671 | **UNIFY** | F2 | emitDeserializeField: class component clause | false | symbol-side shape |
| 1693 | **UNIFY** | F4 | emitEnumDeserialize | returns early | enumDeclaredValues + wire names |
| 2328 | **UNIFY** | F2 | emitPolyClassSerialize: base clause + poly registry | false -> no polymorphic dispatch | symbol-side shape |
| 2354 | **UNIFY** | F2 | poly case's own clause | false | symbol-side shape |
| 2406 | **UNIFY** | F2 | emitPolyClassDeserialize: base clause | false | symbol-side shape |
| 2477 | **UNIFY** | F2 | poly case's own clause | false | symbol-side shape |
| 2579 | **UNIFY** | F2 | emitClassSerialize: graph mode? | false -> tree mode chosen for a graph class | symbol-side shape |
| 3039 | **UNIFY** | F2 | emitClassDeserialize: graph mode? | false | symbol-side shape |
| 3758 | **UNIFY** | F2 | component class's clause | false | symbol-side shape |
| 4218 | **UNIFY** | F2 | isSerializationCall: class has a clause? | false -> not a serialization call | symbol-side shape |
| 4238 | **UNIFY** | F2 | same, static Deserialize | false | symbol-side shape |
| 4663 | **UNIFY** | F2 | tryLowerSerializationCall: class has a clause? | returns nullptr -> not lowered | symbol-side shape |
| 4695 | **UNIFY** | F2 | same | nullptr | symbol-side shape |
| 4732 | **UNIFY** | F2 | same | nullptr | symbol-side shape |

#### `src/codegen/LlvmBackend_Setup.cpp`

| line | verdict | family | question it answers | what an imported symbol gets today | what it should read |
|---|---|---|---|---|---|
| 209 | **ALREADY-CORRECT** | — | - | populateEnumValuesFromDecl -- imported enums are covered at :275-282 from EnumConstantSymbol::value | - |
| 1794 | **ALREADY-CORRECT** | — | - | `isSynthesizedPrelude` -- the prelude decl is injected locally into every unit and is never exported | - |
| 2088 | **ALREADY-CORRECT** | — | - | same; ModuleMetadataBuilder:1727 skips the prelude, so an imported parent is never one | - |
| 5187 | **NEEDS-AST** | F12 | emitInvariantCheck: §22.16 invariants across the base chain | an imported base's invariants are canonical TEXT (`TypeSymbol::invariantContracts`), not a lowerable expression -- a real separate §22.16 gap, not a unify | - |
| 7753 | **UNREACHABLE** | F6 | templateGenericParamNames_ for the dead generic template body (re-located by content 2026-08-18) | The audit's `:7753` line label is stale; what's actually at `Setup.cpp:7753` on HEAD is `if (!sema::typeArgRuntimeReified(subst))` inside `Emitter::rejectCollapsedTypeTestTarget` (defined at `:7731`) — not inside `emitFunctionBody` at all. The block the audit MEANT is the `templateGenericParamNames_.clear()` / `.insert(gp)` / `.insert(gp.name)` cluster at `Setup.cpp:7968-7988`, which IS inside `Emitter::emitFunctionBody` (defined at `:7946`) AFTER the `ctx_.isImported(ms)` early-return at `:7955`. So the UNREACHABLE verdict holds — for the RIGHT reason this time: `emitFunctionBody` returns on import before the cluster runs, and the cluster uses `sema::genericParamNames(ms->parent)` (the unified accessor from issue 3143, applied at `:7976` "converted with the family so the read cannot answer differently from its siblings if that early-out ever moves") but with no consumer for an imported method today | - |
| 10261 | **UNIFY** | F3 | emitChainCall `this(...)`: primary-ctor positional stores | skipped -> the object stays calloc-zero | positional FieldSymbols |
| 10398 | **UNIFY** | F6 | generic substitution | no subst | genericParamNamesOf |
| 10837 | **NEEDS-AST** | F8 | emitImplicitBase: `primaryBaseCtorArgs` (the `: Base(v)` clause) | arbitrary argument EXPRESSIONS; no symbol channel is possible without carrying body text | - |
| 15165 | **UNIFY** | F9 | valueLikeChoiceLocalMintsStringPayload: the choice decl | ALWAYS null -- casts a `sema::TypeSymbol*`, not `->decl` (see #3150) | `rt->declaration->decl`, then the symbol-side payload list |
| 20881 | **UNIFY** | F3 | isFreshRecordConstructionOfExactType | false -> the freshness obligation is lost | positional FieldSymbols |
| 20979 | **UNIFY** | F1 | resolveSynthesizedDeserializeCallTarget: declares `IDeserializable`? | false -> never resolved | built-in-interface list |

#### `src/codegen/LlvmBackend_Stmts.cpp`

| line | verdict | family | question it answers | what an imported symbol gets today | what it should read |
|---|---|---|---|---|---|
| 1428 | **UNIFY** | F6 | op++ receiver generic substitution | no subst installed | genericParamNamesOf |

#### `src/codegen/LlvmBackend_impl.hpp`

| line | verdict | family | question it answers | what an imported symbol gets today | what it should read |
|---|---|---|---|---|---|
| 5207 | **ALREADY-CORRECT** | — | - | a COMMENT, not a site (the grep matches the prose) | - |
| 5224 | **ALREADY-CORRECT** | — | - | MODEL ACCESSOR `recordIsPositional` -- `hasPositionalFields` first, AST fallback | - |

#### `src/common/OwnershipFacts.cpp`

| line | verdict | family | question it answers | what an imported symbol gets today | what it should read |
|---|---|---|---|---|---|
| 124 | **UNIFY** | F5 | isFreshOwnedClassProducer: is the produced type a class? | false -> `new ImportedClass()` in temporary position registers no §28.3 disposal obligation -> LEAK | `rt->declaration->kind` |
| 769 | **UNIFY** | F10 | isBorrowBearingRecordCommon (the SHARED core) | false for every imported record | symbol-side field walk |
| 986 | **UNIFY** | F9 | payloadFieldTypeIsMoveOnly: nested choice payloads | false -> a move-only payload is not tracked | symbol-side payload list |
| 1089 | **UNIFY** | F9 | classifyChoiceCore | std::nullopt for every imported choice | symbol-side case list |

#### `src/driver/Driver.cpp`

| line | verdict | family | question it answers | what an imported symbol gets today | what it should read |
|---|---|---|---|---|---|
| 132 | **ALREADY-CORRECT** | — | - | casts an `ast::TopDecl&` from the parsed unit -- not a symbol's decl | - |

#### `src/module/ModuleMetadataBuilder.cpp`

| line | verdict | family | question it answers | what an imported symbol gets today | what it should read |
|---|---|---|---|---|---|
| 731 | **ALREADY-CORRECT** | — | - | collectInvariants -- has the explicit `else` imported branch reading `invariantContracts` | - |
| 1309 | **ALREADY-CORRECT** | — | - | export side; walks `mod->children`, always local (but latent: the WHOLE generic-param list is gated on the AST decl) | - |
| 1516 | **ALREADY-CORRECT** | — | - | export side, enum constants -- always local | - |
| 1727 | **ALREADY-CORRECT** | — | - | export side, `isSynthesizedPrelude` -- always local | - |

#### `src/ownership/Ownership.cpp`

| line | verdict | family | question it answers | what an imported symbol gets today | what it should read |
|---|---|---|---|---|---|
| 8931 | **ALREADY-CORRECT** | — | - | casts an `ast::TopDecl&` -- local AST walk | - |

#### `src/ownership/Ownership_Checks.cpp`

| line | verdict | family | question it answers | what an imported symbol gets today | what it should read |
|---|---|---|---|---|---|
| 143 | **UNIFY** | F3 | resolveCtorParams: the primary ctor's formals | {} -> an `owned` argument is not classified an owned sink -> leak / use-after-move | positional FieldSymbols |
| 485 | **UNIFY** | F9 | recordMovesInArgs: the choice decl for §28.1 erasure | null -> the erasure decision runs without the declaration | symbol-side payload list |
| 633 | **UNIFY** | F9 | isImplicitOwned: same | null | symbol-side payload list |
| 2935 | **UNIFY** | F10 | isBorrowBearingRecordImpl | false -- "not borrow-bearing" for EVERY imported record | walk `rt->declaration->members` + importedMemberType |
| 3025 | **UNIFY** | F3 | isNullSourceRecordExpr: positional fields | false -> the §9.7 null-source accept never fires | positional FieldSymbols |
| 3121 | **UNIFY** | F9 | isNullSourceChoiceExpr: the choice decl | null | symbol-side payload list |
| 3320 | **UNIFY** | F10 | checkRecordBorrowEscapesOnReturn | the per-field gate is skipped | symbol-side field walk |
| 4553 | **UNIFY** | F11 | ownershipIsExceptionClassRef | false -- the AST base scan AND the `b->decl` ancestor guard both stop at the module boundary | sema::builtinExceptionBaseOf(cur) |
| 8243 | **UNIFY** | F10 | checkRecordFromTemporary | returns before checking -> the §9.7 dangling-borrow REJECT never fires (soundness) | symbol-side field walk |

#### `src/sema/DefiniteAssignment.cpp`

| line | verdict | family | question it answers | what an imported symbol gets today | what it should read |
|---|---|---|---|---|---|
| 1977 | **ALREADY-CORRECT** | — | - | casts an `ast::TopDecl&` -- local AST walk | - |

#### `src/sema/EnumValues.hpp`

| line | verdict | family | question it answers | what an imported symbol gets today | what it should read |
|---|---|---|---|---|---|
| 59 | **ALREADY-CORRECT** | — | - | MODEL ACCESSOR `enumDeclaredValues` -- AST for local, `EnumConstantSymbol::value` for imported | - |

#### `src/sema/ExprTypecheck.cpp`

| line | verdict | family | question it answers | what an imported symbol gets today | what it should read |
|---|---|---|---|---|---|
| 1061 | **UNIFY** | F13 | leafParamExcludesNullable (§19.6 census) | "no notnull/class/value/type bound" | GenericParamSymbol-side constraints |
| 1415 | **UNIFY** | F3 | groundSatisfiesNew, record arm | undecided -> silent accept | positional FieldSymbols |
| 1433 | **UNIFY** | F3 | groundSatisfiesNew, class arm | `new()` wrongly satisfied | positional FieldSymbols |
| 1584 | **UNIFY** | F13 | leafSolutionViolatesScalarConstraint | "no constraint" | GenericParamSymbol-side constraints |
| 1984 | **ALREADY-CORRECT** | — | - | MODEL: `!td -> fall back to EnumConstantSymbol members` | - |
| 2514 | **ALREADY-CORRECT** | — | - | reads `currentType_->decl` -- always local | - |
| 3957 | **UNIFY** | F1 | namedAncestorChain: built-in bases survive only on the AST list | an imported node contributes no built-in ancestor | built-in-interface/exception list |
| 5582 | **UNIFY** | F1 | userTypeImplementsBuiltinIface | false / sawUnmodeledBuiltinBase | built-in-interface list |
| 5718 | **UNIFY** | F6 | variantGenericAssignable: the i-th param's variance | "" -> invariant -> a legal §12.9 variant assignment is REJECTED | GenericParamSymbol::variance |
| 5846 | **UNIFY** | F14 | isFinalClass: `sealed` and not `abstract`? | false -> an imported sealed class is not final; the §19.3/§5.1 exhaustiveness reasoning is lost | typeSymbolHasModifier / typeIsAbstractOrClosed |
| 6151 | **UNIFY** | F6 | typeTestInterfaceBindAmbiguous: is the interface variant? | documented conservative accept -- but the variance IS on the symbol, so the imprecision is avoidable | GenericParamSymbol::variance |
| 6925 | **ALREADY-CORRECT** | — | - | reads `currentType_->decl` -- always local | - |
| 6959 | **ALREADY-CORRECT** | — | - | reads `currentType_->decl` -- always local | - |
| 7238 | **ALREADY-CORRECT** | — | - | reads `currentType_->decl` -- always local | - |
| 7262 | **ALREADY-CORRECT** | — | - | reads `currentType_->decl` -- always local | - |
| 8636 | **ALREADY-CORRECT** | — | - | casts an `ast::TopDecl&` -- local AST walk | - |
| 10282 | **UNIFY** | F1 | userTypeSupportsForIn: an IEnumerable-family base? | false -> `for ... in` over an imported iterable type is rejected | built-in-interface list |

#### `src/sema/ExprTypecheck_Calls.cpp`

| line | verdict | family | question it answers | what an imported symbol gets today | what it should read |
|---|---|---|---|---|---|
| 2773 | **UNIFY** | F3 | `new()` call-site re-check, class arm | imported class with a parameterful primary ctor SATISFIES `new()` | positional FieldSymbols |
| 3654 | **ALREADY-CORRECT** | — | - | reads `currentType_->decl` -- always local | - |
| 3672 | **ALREADY-CORRECT** | — | - | reads `currentType_->decl` -- always local | - |
| 5065 | **UNIFY** | F1 | does the element implement `IComparable`? (Min/Max gate) | false -> per the site's comment, sema types the result as T and codegen IR-FAILS | built-in-interface list |
| 5361 | **UNIFY** | F1 | same (#1195 gate) | false | built-in-interface list |
| 10610 | **UNIFY** | F3 | checkNewExprArgs: count + per-field type for `new Record(args)` | the ENTIRE check is skipped -> the stack corruption the site's own comment describes | positional FieldSymbols |
| 15967 | **ALREADY-CORRECT** | — | - | reads `currentType_->decl` -- always local | - |
| 16810 | **UNIFY** | F1 | is `.Serialize`/`.Deserialize` synthesized here? | false -> "no member `Serialize`" on an imported serializable type | built-in-interface list |
| 16908 | **UNIFY** | F1 | the "no member" suppression list | false | built-in-interface list + symbol-side shape |
| 17626 | **ALREADY-CORRECT** | — | - | MODEL: `targetType->hasPositionalFields` first, AST fallback | - |

#### `src/sema/ExprTypecheck_Exprs.cpp`

| line | verdict | family | question it answers | what an imported symbol gets today | what it should read |
|---|---|---|---|---|---|
| 188 | **UNIFY** | F1 | declaresIFormattable (§18.5) | false -> a format spec on an imported formattable type is rejected | built-in-interface list |
| 355 | **ALREADY-CORRECT** | — | - | guarded and documented: `!td -> false` ("imported record -- defer, don't reject") | - |
| 763 | **ALREADY-CORRECT** | — | - | reads `currentType_->decl`; `currentType_` is the type being typechecked, always local | - |
| 2269 | **ALREADY-CORRECT** | — | - | reads `currentType_->decl` -- always local | - |
| 3000 | **ALREADY-CORRECT** | — | - | reads `currentType_->decl` -- always local | - |
| 3579 | **UNIFY** | F3 | gate on calling checkNewExprArgs for a positional record | never called -> the #1105 arg check (which prevented a stack corruption) is skipped | `ts->hasPositionalFields` |
| 3734 | **ALREADY-CORRECT** | — | - | reads `currentType_->decl` -- always local | - |
| 3772 | **ALREADY-CORRECT** | — | - | reads `currentType_->decl` -- always local | - |
| 4467 | **ALREADY-CORRECT** | — | - | MODEL: `ts->hasPositionalFields` first, AST fallback | - |
| 6672 | **UNIFY** | F3 | literal-type hint from positional field i | no hint -> the literal may be typed wrong | positional FieldSymbols + importedMemberType |

#### `src/sema/PureChecker.cpp`

| line | verdict | family | question it answers | what an imported symbol gets today | what it should read |
|---|---|---|---|---|---|
| 256 | **ALREADY-CORRECT** | — | - | MODEL: `rec->hasPositionalFields` first, AST fallback | - |
| 311 | **ALREADY-CORRECT** | — | - | MODEL: same (one statement, two casts) | - |
| 1100 | **ALREADY-CORRECT** | — | - | deliberate documented skip: an imported class's invariants are artifact facts, not re-checked here | - |

#### `src/sema/Sendability.cpp`

| line | verdict | family | question it answers | what an imported symbol gets today | what it should read |
|---|---|---|---|---|---|
| 302 | **ALREADY-CORRECT** | — | - | guarded: `!td -> return true` (trust the value-like verdict) -- deliberate and documented | - |
| 338 | **ALREADY-CORRECT** | — | - | guarded: `!td -> forEachReconstructedStorageMember` with the SAME per-field predicate (#1824) | - |
| 484 | **ALREADY-CORRECT** | — | - | feeds the `td ? ... : forEachReconstructedStorageMember` pair at :551 | - |
| 551 | **ALREADY-CORRECT** | — | - | guarded: `td ? walkMembersForSend : forEachReconstructedStorageMember`, incl. base classes (#1732) | - |
| 634 | **ALREADY-CORRECT** | — | - | guarded: the AST path is followed by the #1824 reconstructed-member path | - |
| 867 | **ALREADY-CORRECT** | — | - | guarded: `if (td) walkAtd else walkReconstructed` | - |
| 874 | **ALREADY-CORRECT** | — | - | same, for base classes | - |
| 960 | **ALREADY-CORRECT** | — | - | diagnostic detail only; `!td -> {}` falls back to the flat category reason | - |

#### `src/sema/SymbolCollection.cpp`

| line | verdict | family | question it answers | what an imported symbol gets today | what it should read |
|---|---|---|---|---|---|
| 362 | **ALREADY-CORRECT** | — | - | casts an `ast::TopDecl&` -- local AST walk | - |
| 505 | **ALREADY-CORRECT** | — | - | MODEL ACCESSOR `builtinExceptionBaseOf` -- symbol field first, AST fallback, walk crosses the boundary (#2950) | - |
| 702 | **ALREADY-CORRECT** | — | - | MODEL: `decl->hasPositionalFields` first, AST fallback | - |
| 761 | **ALREADY-CORRECT** | — | - | MODEL: `rec->hasPositionalFields` first, AST fallback | - |
| 976 | **ALREADY-CORRECT** | — | - | guarded `!td -> true`; blocked on the choice-case channel (see #3144) | - |
| 1022 | **ALREADY-CORRECT** | — | - | MODEL: `ts->hasPositionalFields` first, AST fallback | - |
| 1075 | **ALREADY-CORRECT** | — | - | diagnostic detail only; degrades to the flat reason | - |

#### `src/sema/TypeResolution_Checks.cpp`

| line | verdict | family | question it answers | what an imported symbol gets today | what it should read |
|---|---|---|---|---|---|
| 653 | **ALREADY-CORRECT** | — | - | walks `ctx_.module()->children` -- local declaration-site check | - |
| 847 | **ALREADY-CORRECT** | — | - | walks module children and skips `!ts->decl` -- local declaration-site check | - |
| 1218 | **ALREADY-CORRECT** | — | - | checkRecordRestrictions -- local declaration-site check | - |
| 2037 | **ALREADY-CORRECT** | — | - | reads the ENCLOSING type's constraints -- always local | - |
| 5538 | **UNIFY** | F3 | argSatisfiesNew, record arm: does `default(R)` exist? | `!td -> return false` with `decided == false` -> undecided -> silent accept | positional FieldSymbols |
| 5563 | **UNIFY** | F3 | argSatisfiesNew, class arm: parameterless ctor? | falls through to the explicit-ctor scan (finds none) -> `new()` WRONGLY satisfied | positional FieldSymbols |
| 5809 | **UNIFY** | F13 | checkGenericConstraints at an INSTANTIATION site | returns immediately -> EVERY §14.2 constraint on EVERY imported generic type is unchecked | GenericParamSymbol-side constraints |
| 6318 | **ALREADY-CORRECT** | — | - | walks `ctx_.module()->children` -- local declaration-site check | - |
| 6777 | **UNIFY** | F1 | is an imported RECORD component serialization-eligible? | false -> FALSE REJECT of the local type that uses it | built-in-interface list |
| 6812 | **UNIFY** | F1 | same, CHOICE component | false -> false reject | built-in-interface list |
| 6843 | **UNIFY** | F2 | same, CLASS component (has a `serialized` clause?) | false -> false reject | symbol-side shape |
| 6976 | **ALREADY-CORRECT** | — | - | walks `ctx_.module()->children` -- local declaration-site check | - |
| 7056 | **ALREADY-CORRECT** | — | - | walks module children -- local declaration-site check | - |
| 7528 | **ALREADY-CORRECT** | — | - | walks module children -- local declaration-site check | - |
| 7684 | **UNIFY** | F2 | polymorphic registry entry has a clause? | false -> a MISLEADING diagnostic naming the imported type | symbol-side shape |
| 7818 | **UNIFY** | F2 | back-edge target has a graph-shape clause? | false -> false reject | symbol-side shape |

#### `src/sema/TypeResolution_Names.cpp`

| line | verdict | family | question it answers | what an imported symbol gets today | what it should read |
|---|---|---|---|---|---|
| 411 | **ALREADY-CORRECT** | — | - | casts an `ast::TopDecl&` -- local AST walk | - |
| 1145 | **ALREADY-CORRECT** | — | - | reads `currentOuterType_->decl` -- the enclosing type, always local | - |
| 1923 | **UNIFY** | F12 | §14.4/§28.5 `owned` bare-type-parameter field check at a USE site | skipped for an imported generic class -> a `where T: class` instantiation at `string`/a function type is admitted | FieldSymbol::modifiers + importedMemberType |

---

## 6. Found while auditing — not part of the 157

### 6.1 An always-null `dynamic_cast` (#3150)

`LlvmBackend_Setup.cpp:15164` casts `rt->declaration` — a `sema::TypeSymbol*` — to
`const ast::TypeDecl*`. The hierarchies are unrelated, so the cast **always yields null**, for
LOCAL types too. The result is passed to `substitutedChoicePayloadType`, so #2115's
generic-payload substitution has never executed. The two sibling sites
(`LlvmBackend_Disposal.cpp:2077`, `:2197`) spell it correctly as `choiceTy->decl`, and the
buggy site's comment claims to be *"the same `choiceDecl` cast"* they use.

### 6.2 `baseTypes` / `baseTypeRefs` are parallel locally and not for imports (#3151)

The local push is guarded by `if (ref->declaration)` for **both** vectors; the importer's push
to `baseTypeRefs` is **unconditional**. Any imported type with an unresolvable base ahead of a
resolvable one misaligns every lockstep `baseTypeRefs[bi]` read
(`TypeResolution_Checks.cpp:2612/3123/4029/5335`, `LlvmBackend_Disposal.cpp:3674`). Whether it
is live or latent depends on whether `lookupNamed` can return null for an exported base —
confirm before fixing, and the choice of which invariant to keep is a recorded decision, not a
patch.

### 6.3 The seam is ~570 casts, not 159 (#3149)

`ast::TypeDecl` is one spelling. Counting every AST-node cast of a symbol's `decl`:

```
$ grep -rnE 'dynamic_cast<(const )?ast::[A-Za-z]+\*>\(' src/ | grep -cE '(->|\.)decl\b'
520                     # + ~47 more where the cast wraps to the next line
```

`PropertyDecl` 96 · `MethodDecl` 90 · `FieldDecl` 52 · `ConstructorDecl` 51 · `TypeDecl` ~159 ·
`LocalDecl` 30 · `ConstDecl` 15 · `EventDecl` 11 · rest 9. **`TypeDecl` is ~28% of it.**

`MemberSymbol::decl` is null for imports too. `ModuleLoader` sets it only where it re-parsed
body text for a GENERIC type (`:1440` ctor, `:1662` dtor, `:1980` method, `:2358` static
field) — so for a **non-generic** imported class every member symbol has `decl == nullptr`,
and `PropertySymbol::decl` is **never** set (`ModuleLoader.cpp:560`: *"an imported
`PropertySymbol` has no `ast::PropertyDecl`"*).

The concrete instance: **`isAutoPropertySymbol`** (`SymbolCollection.cpp:595`), whose own
header says it is *"Shared by sema and codegen so both agree on what counts as auto-property
storage"*, returns `false` for every imported property at **31 call sites**. In
`isAutoEqualityBearingBodyRecord` that means every auto-property of an imported body record
falls through to `if (dynamic_cast<const PropertySymbol*>(m)) continue;` — skipped as a
"computed member" — so the §9.2 admissibility check runs on **nothing** and the record is
declared auto-equality-bearing regardless of what its fields actually are.

The loader now carries **five** ad-hoc side tables to route around member-decl nullity, each
added by its own bug: `fieldTypes_`/`returnTypes_`, `constValues_` (#328), `propHasGetter_`,
`propSetterVisibility_` (#2415), `propIsAutoStorage_` (#2478). Each fixed one consumer; none
made the next one correct. Adding a sixth is the option that has already failed five times.

---

## 7. Suggested order of work

1. **#3142** (3 one-liners, 2 leaks) — smallest possible blast radius, proves the ASan
   regression-test shape for the rest.
2. **#3145** (§9.7 soundness) — highest severity; the `forEachReconstructedStorageMember`
   pattern from #1732/#1824 is already written and just needs applying to the §9.7 walk.
3. **#3143** (22 sites, no schema change) — the biggest mechanical win; two model accessors
   already exist, so it is a call-the-accessor sweep.
4. **#3141**, then **#3139** → **#3140** (the serialization family depends on the
   built-in-interface channel).
5. **#3146** (the wire already carries it), **#3147** (three residues).
6. **#3144** last — it is the only family genuinely blocked on new schema work. Until it
   lands, its sites must degrade *conservatively* (as `Sendability.cpp:302` already does)
   rather than silently answering "no cases".
7. **#3149** is its own audit, of a larger surface, and should be scoped separately.
