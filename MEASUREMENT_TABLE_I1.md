# Family I1 — measurement table on HEAD `b57a712e`

Re-verified 2026-09-18 (cycle-9 phase-1, lane I1-anchor).

The task is to hoist three eager substitution walkers
(`Resolver::applyGenericSubst` / `Resolver::substituteAliasGenericArgs` /
`ExprTC::substituteGenericFieldType`) into one `sema::substituteGenericArgs`,
then re-measure the 8 open members and close those that the hoist's missing
arms cover.

## The 3 walkers

| Walker | Class | Location | Coverage |
|---|---|---|---|
| `Resolver::applyGenericSubst` | `Resolver` | `src/sema/TypeResolution_Checks.cpp:4479-4502` | top-level bare parameter only (no recursion) |
| `Resolver::substituteAliasGenericArgs` | `Resolver` | `src/sema/TypeResolution_Names.cpp:1826-1912` | fully recursive over Named/inlineArgs, InlineArray, Function; nullable preservation; nested-inline-array re-validation (alias-specific) |
| `ExprTC::substituteGenericFieldType` | `ExprTC` | `src/sema/ExprTypecheck.cpp:3016-3127` | fully recursive over same kind set (Named/inlineArgs, InlineArray, Function); nullable preservation; importedName fallback (cross-module) |

## Measurement on `b57a712e`

| Issue | State | Today's behavior | Mechanism | In hoist scope? |
|---|---|---|---|---|
| **#4212 (anchor)** | OPEN | 3 walkers agree today; nothing keeps them agreeing | structural duplication | YES (the hoist) |
| **#4172** | OPEN | Repro from issue body **passes** today on b57a712e (post-#4188 `substituteAliasGenericArgs` is called and substitutes); the issue's first variant compiled and ran printing "hi". The `#4172`-shaped bug remains for `Array<T>` constructor expressions (`new Array<int>(3)` reports "initializer … not implicitly convertible to the target type"). | alias-transparency edge: an alias `type IdP11d<T> := T;` works (bare-param shortcut at `TypeResolution_Names.cpp:2881` returns the concrete int), but a more nested shape (e.g. `type IdP11d<T> := Array<T>; var x: IdP11d<int> := new Array<int>(3);`) does NOT substitute through to the new expression's type because the substitution is per-reference-site, and the initializer's type is resolved against a scope that may not see the alias's substitution. | YES (missing arm in the walker: substitution needs to also be visible at the new-expression type-resolve site) |
| **#3861** | OPEN | `unifyArgSubst` keyed on arity alone (`src/sema/ExprTypecheck_Calls.cpp:1047-1052`) | separate unification code path, NOT in the 3 walkers | NO (separate fix) |
| **#2755** | OPEN | `Inner<U>.Head()` in a generic body — `genericSubstSemaTy_` not substituted for the static-access call form (codegen path, `LlvmBackend_Exprs2.cpp`) | codegen static-access substitution NOT in the sema walkers; out of scope per brief | NO (codegen, out of scope) |
| **#3566** | OPEN | `instanceDescriptorFor` for monomorphized comparator installs derived-class param names (`src/codegen/LlvmBackend_Setup.cpp`) | codegen monomorphization NOT in the sema walkers | NO (codegen, out of scope) |
| **#2742** | OPEN | `checkHashContainerKeys`' Pass 2 (`src/sema/TypeResolution_Checks.cpp:~1829-1855`) validates generic-method param against enclosing TYPE's binding, not the method's | separate `checkHashContainerKeys` walk; different scope of param resolution | NO (separate fix) |
| **#2735** | OPEN | Owning-element per-instantiation sweep at `checkOwningElementInstantiations` (`src/sema/TypeResolution_Checks.cpp`) does not reach CROSS-MODULE instantiations (no AST in importing compilation → no `owningElementSites_` entry recorded) | separate owning-element sweep | NO (separate fix) |
| **#2736** | OPEN | Same `checkOwningElementInstantiations` (a) misses generic-method params (resolved against `s.enclosingType->genericParams`, not the method's own params) and (b) does not list `OwningTask<T>` / `IOwning*` interfaces per §14.4 | separate owning-element sweep | NO (separate fix) |

## Outcome of the hoist

The hoist closes #4212 by collapsing the 3 walkers into 1. Of the other 7
members, only #4172 has the slightest overlap with the sema walkers (the
alias resolution path's substitution site), and #2755 is described in the
brief as a likely missing arm in the unified walker — but the in-tree repro
shows it's a codegen-side `genericSubstSemaTy_` substitution failure, NOT a
sema walker arm; per the brief, codegen sites are out of scope.

So the plan is: hoist + add 1 row for #4172's `Array<T>` alias
constructor-initializer case (where the alias substitution needs to reach the
`new Array<int>(3)`-side type resolution) + the sema snapshot rows. Members
#3861/#2755/#3566/#2742/#2735/#2736 are explicitly filed for separate
work.

## Spec citations

- §14.1 (Generic types and instantiation)
- §27.8 (Type aliases, including the "Each instantiation of the alias is
  interchangeable with the corresponding instantiation of the aliased type"
  sentence that grounds #4172 and #4188)
- §28.2 / §28.3 (read for the diagnostic shape the alias path inherits)
- v0.298 (per the proposal §2.5 citation)