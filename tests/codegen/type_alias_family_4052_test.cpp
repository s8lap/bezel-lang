// Spec-derived tests for fix #4052 — §27.8 (v0.294): the UMBRELLA issue for
// the "alias-resolution-gap" family after the #4038/#4048/#4050/#4051 chain
// (and the follow-on #4070/#4076 codegen twins). Per the umbrella, one
// dedicated lane per cycle to drain the remaining
// `dynamic_cast<TypeSymbol*>` / `dynamic_cast<NestedTypeSymbol*>` sites in
// src/sema/ that resolve a WRITTEN NAME through a path with no
// `TypeAliasSymbol` arm.
//
// Four sites confirmed and fixed in this single bundled commit, each a
// concrete, measured false-reject (no crash, no wrong value, just a clean
// rejection of a program the direct-name spelling accepts — a "missed
// admission" / "false-reject" class, distinguished from the silent-wrong-
// answer class #4051 filed):
//
//   A1. `ExprTC::enumTypeNamedBy` (src/sema/ExprTypecheck_Calls.cpp, fix
//       #3044 / §10.4): the §10.4 `E.FromUnderlyingOrNull(value)` branch's
//       enum-type-name recognition had no TypeAliasSymbol arm, so an alias
//       of an enum used as the receiver was misclassified and the call fell
//       through to the #605/#2434 backstops at codegen — measured pre-fix:
//       `AliasEnum.FromUnderlyingOrNull(n)` rejects with two backstop
//       internal-error diagnostics and a NullReferenceException at runtime.
//
//   A2. Codegen twin of A1 in `Emitter::tryLowerEnumFromUnderlyingOrNull`
//       (src/codegen/LlvmBackend_Exprs2.cpp): codegen does NOT consume
//       sema's resolved symbol here (matching the #4076 destructuring-
//       pattern codegen-twin observation) and independently re-resolved
//       the receiver symbol with the same missing arm — without this
//       codegen fix, sema's recognition (A1) classifies the call as `E?`
//       and types it, but codegen rejects here and the call degrades to
//       the labeled-zero backstop even with the sema fix in place.
//
//   A3. The universal-member arity-check receiver gate
//       (`targetIsStaticReceiver`, src/sema/ExprTypecheck_Calls.cpp, fix
//       #2446 / §8.1): the §8.1 universal-member arity check
//       (`Equals`/`ToString`/`GetHashCode`/`GetType`/`Dispose`/
//       `ReferenceEquals`) gates suppression of the universal-surface
//       check on whether the receiver is a type-name. An alias-qualified
//       static receiver (`AliasCls.Equals()` for a class with a
//       user-declared static `Equals` 0-arg, or any alias-qualified
//       type-name receiver sharing a universal-member name) was treated
//       as an INSTANCE receiver and the arity check fired rejecting a
//       legitimate 0-arg call with "the universal `Object.Equals` member
//       takes 1 argument(s) but the call supplies 0 (§8.1)" — a false
//       reject, since the direct-name spelling compiles clean.
//
//   A4. The capture-free entry predicate (`isCaptureFreeEntry`,
//       src/sema/ExprTypecheck_Calls.cpp, fix #600 / §30.14): the
//       `Domain.Spawn` entry-must-be-capture-free check (and the
//       sibling `static event +=` / `<static event>.Subscribe` checks)
//       classified an alias-qualified static method reference
//       (`Domain.Spawn(AliasCls.StaticMethod, ...)`) as NOT capture-free
//       and rejected with "Domain.Spawn entry must be capture-free: … a
//       capturing lambda or an instance method reference is rejected
//       because captures and receivers are borrows of the origin domain
//       (§30.14, #600)" — a false reject, since `AliasCls.StaticMethod`
//       IS capture-free exactly like the direct `Cls.StaticMethod`.
//       Required dropping the `const` on `isCaptureFreeEntry` because
//       `resolveAliasToTypeSymbol` walks `ctx_.resolvedType` and is
//       non-const; all three call sites are non-const contexts
//       (ExprTypecheck_Calls.cpp `Domain.Spawn` arg-0 check,
//       ExprTypecheck.cpp `static event +=` RHS check,
//       ExprTypecheck_Exprs.cpp `Subscribe(...)` arg-0 check), so this
//       is API-compatible.
//
//   A5. The qualified-const fold (`foldIntConstExprTC`,
//       src/sema/ExprTypecheck.cpp, fix #2986 / §17.14 / §5.1 / §22.12):
//       admit a qualified class const `C.GK` as a compile-time integer
//       constant so it can drive §5.1 narrowing. The head is required to
//       be a `TypeSymbol` (a class); an alias of that class falls into
//       the same gate, so `AliasCls.K` is misclassified as NOT a
//       const-bearing class head and the alias-qualified const fails to
//       fold — a "missed ADMISSION" (#4052 noted this as "lowest
//       priority of this list"): the alias spelling is rejected with
//       "the initializer for `x` has a type that is not implicitly
//       convertible to the target type (§5.1)" even though the direct-
//       name spelling compiles clean and narrows correctly.
//
// All four fixes resolve through the shared
// `sema::resolveAliasToTypeSymbol` (TypeResolution.hpp/.cpp) / ExprTC thin
// wrapper (ExprTypecheck.cpp) — the seam #4050/#4051 introduced for this
// exact family. §27.8 verbatim: "Type aliases — generic or not — see
// through to the aliased type for identity, equality, method dispatch, and
// overload resolution" — every one of these four sites is a dispatch /
// classification gate that must see through the alias.
//
// ── INHERITED KNOWN-OPEN HAZARD (#4047) ────────────────────────────────────
//
// The four fixes inherit #4047's chained-alias declaration-order hazard
// exactly the way #4070/#4076 already do: `Resolver::resolveTopDecl`'s
// single forward pass over `src.topDecls` has not yet resolved the inner
// alias's `aliasedType` when an outer alias declared textually before the
// inner alias is itself resolved, so `resolveAliasToTypeSymbol` returns
// null for that shape and this fix has nothing to dispatch on. A single
// alias-of-direct-type (non-chained) is unaffected by declaration order
// regardless of which side of the alias the class/record/enum is declared
// on — its own TypeSymbol is registered in an earlier discovery pass.
// Pinned below for each site as a KNOWN-OPEN control row alongside the
// alias-inner-first row that proves the chain itself is OK and the
// declaration order is the only remaining hazard.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ============================================================================
// A1+A2: §10.4 `E.FromUnderlyingOrNull(value)` through a `type` alias of an
// ENUM. Sema's enum-type-name recognition (A1) AND codegen's identical
// re-resolution (A2) need the TypeAliasSymbol arm; one without the other
// falls through to the #605/#2434 backstops. Hits: hit-spec, miss-spec,
// non-match-falls-through direct-name anti-regression, chained-inner-first
// works, chained-outer-first known-open #4047.
// ============================================================================

// #4052 issue repro, verbatim (§10.4 / §27.8): the hit case — an alias of an
// enum used as the receiver of the compiler-provided static
// `FromUnderlyingOrNull`. Pre-fix: rejected with two backstop diagnostics
// ("codegen cannot dispatch member 'FromUnderlyingOrNull' on receiver of
// type '<Enum>'" / "initializer failed to lower") and a runtime
// NullReferenceException. Post-fix: 2 (the underlying value of Priority.High
// with auto-increment Low=0, Medium=1, High=2).
BEZEL_TEST(type_alias_family_4052, enum_alias_FromUnderlyingOrNull_hit) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Priority4052 Low, Medium, High; end Priority4052; "
        "type AliasPriority4052 := Priority4052; "
        "class C "
        "  public static method Main(): int "
        "    var n: int := 2; "
        "    var p := AliasPriority4052.FromUnderlyingOrNull(n); "
        "    return int(p!); "
        "  end Main; "
        "end C;"), 2);
}

// §10.4 miss case through the alias — `FromUnderlyingOrNull` is TOTAL, it
// returns null for an out-of-range value rather than faulting. Pre-fix:
// same backstop rejection as the hit case (the rejection is not value-
// dependent — codegen rejects the call shape, never lowering the argument).
// Post-fix: 42 (the null branch of the explicit null check).
BEZEL_TEST(type_alias_family_4052, enum_alias_FromUnderlyingOrNull_miss) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Priority4052b Low, Medium, High; end Priority4052b; "
        "type AliasPriority4052b := Priority4052b; "
        "class C "
        "  public static method Main(): int "
        "    var n: int := 9; "
        "    var p: Priority4052b? := AliasPriority4052b.FromUnderlyingOrNull(n); "
        "    if p = null then return 42; else return 0; end if; "
        "  end Main; "
        "end C;"), 42);
}

// Anti-regression: the DIRECT (non-alias) `Priority.FromUnderlyingOrNull(n)`
// spelling must still compile and compute identically — pins that the
// new TypeAliasSymbol arm did not disturb the existing TypeSymbol arm
// in either sema or codegen.
BEZEL_TEST(type_alias_family_4052, enum_direct_FromUnderlyingOrNull_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Priority4052c Low, Medium, High; end Priority4052c; "
        "class C "
        "  public static method Main(): int "
        "    var n: int := 2; "
        "    var p := Priority4052c.FromUnderlyingOrNull(n); "
        "    return int(p!); "
        "  end Main; "
        "end C;"), 2);
}

// KNOWN-OPEN #4047 control: a CHAINED alias (outer declared textually
// BEFORE inner) inherits the resolution-order gap — pinned to current
// behavior. The chained INNER-FIRST row below proves the chain itself
// is fine and only the declaration order is the hazard. Measured
// actual: the upstream ordering gap means sema's enumTypeNamedBy falls
// through (TypeAliasSymbol → null), codegen's tryLowerEnumFromUnderlyingOrNull
// also falls through (TypeAliasSymbol → null), and the call degrades
// to the #2434 backstop — program compiles clean, runs, and returns
// the labeled-zero value (0) instead of 2. The driver emits a
// Severity::Warning #2434 backstop to stderr but that warning does NOT
// enter `r.diagnostics` (printed, not recorded via DiagnosticEngine).
// Pin by asserting the program ran with the zero-initialized degraded
// value — identical to pre-fix and post-fix states.
BEZEL_TEST(type_alias_family_4052,
           enum_chained_alias_outer_first_FromUnderlyingOrNull_known_open_4047) {
    auto r = runAot(
        "module M; "
        "type AliasEnumOuter4052 := AliasEnumInner4052; "
        "type AliasEnumInner4052 := EnumChained4052; "
        "enum EnumChained4052 A := 1, B := 2; end EnumChained4052; "
        "class C "
        "  public static method Main(): int "
        "    var n: int := 2; "
        "    var p := AliasEnumOuter4052.FromUnderlyingOrNull(n); "
        "    return int(p!); "
        "  end Main; "
        "end C;");
    // Known-open #4047: chained outer-first ordering gap. Pin by
    // asserting the SAME degraded-zero behavior holds on this fix:
    // program ran (silent wrong answer), exit 0, r.diagnostics empty.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.diagnostics.errorCount(), 0u);
}

// Companion to the known-open control: declaring the chain INNER-FIRST
// resolves correctly through the alias — proves the chain itself is OK,
// only the declaration order is the hazard.
BEZEL_TEST(type_alias_family_4052,
           enum_chained_alias_inner_first_FromUnderlyingOrNull_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasEnumInner4052b := EnumChained4052b; "
        "type AliasEnumOuter4052b := AliasEnumInner4052b; "
        "enum EnumChained4052b A := 1, B := 2; end EnumChained4052b; "
        "class C "
        "  public static method Main(): int "
        "    var n: int := 2; "
        "    var p := AliasEnumOuter4052b.FromUnderlyingOrNull(n); "
        "    return int(p!); "
        "  end Main; "
        "end C;"), 2);
}

// ============================================================================
// A3: §8.1 universal-member arity check through a `type` alias. An alias-
// qualified type-name receiver sharing a universal-member name must
// suppress the §8.1 arity check exactly like the direct name. Hits: alias
// of class with static `Equals()` (0-arg), alias of class calling the
// universal `Equals` directly, anti-regression for the direct-name
// spelling, chained-outer-first known-open #4047.
// ============================================================================

// #4052 issue repro, verbatim (§8.1 / §27.8): an alias-qualified
// type-name receiver sharing the universal-member name (`Equals`)
// where the user class declares NO static `Equals` — the §8.1
// universal-member arity check (which gates on `targetIsStaticReceiver`
// and expects 1 arg for `Equals`) must NOT fire for an alias-qualified
// type-name receiver just as it does not fire for the direct-name
// spelling. Pre-fix: rejected with "the universal `Object.Equals`
// member takes 1 argument(s) but the call supplies 0 (§8.1)". Post-fix:
// compiles and runs (0, since the universal `Equals()` synthesized
// with 0 args returns false). The shape uses NO user-declared static
// `Equals` so `calleeSym` is null and the §8.1 arity block is reached
// — the path the fix actually gates.
BEZEL_TEST(type_alias_family_4052, class_alias_universal_Equals_0arg) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Cls4052 "
        "  destructor end destructor; "
        "end Cls4052; "
        "type AliasCls4052 := Cls4052; "
        "class C "
        "  public static method Main(): int "
        "    if AliasCls4052.Equals() then return 1; else return 0; end if; "
        "  end Main; "
        "end C;"), 0);
}

// Direct-name anti-regression: the §8.1 universal-member arity check
// suppression must still apply to the direct-name spelling — pins that
// the new TypeAliasSymbol arm did not disturb the existing TypeSymbol
// arm's static-receiver classification.
BEZEL_TEST(type_alias_family_4052, class_direct_universal_Equals_0arg_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Cls4052b "
        "  destructor end destructor; "
        "end Cls4052b; "
        "class C "
        "  public static method Main(): int "
        "    if Cls4052b.Equals() then return 1; else return 0; end if; "
        "  end Main; "
        "end C;"), 0);
}

// KNOWN-OPEN #4047 control: a CHAINED alias (outer declared textually
// BEFORE inner) inherits the resolution-order gap — same shape as A1's
// control. Measured actual: the upstream ordering gap means
// `resolveAliasToTypeSymbol` returns null for the outer alias, so
// `targetIsStaticReceiver` stays false, and the §8.1 universal-member
// arity check fires with the SAME diagnostic as the pre-fix state —
// "the universal `Object.Equals` member takes 1 argument(s) but the
// call supplies 0 (§8.1)". Pin by asserting the SAME §8.1 rejection
// holds on this fix.
BEZEL_TEST(type_alias_family_4052,
           class_chained_alias_outer_first_static_Equals_known_open_4047) {
    auto r = runAot(
        "module M; "
        "type AliasClsOuter4052 := AliasClsInner4052; "
        "type AliasClsInner4052 := ClsChained4052; "
        "public class ClsChained4052 "
        "  destructor end destructor; "
        "  public static method Equals(): bool => true; "
        "end ClsChained4052; "
        "class C "
        "  public static method Main(): int "
        "    if AliasClsOuter4052.Equals() then return 1; else return 0; end if; "
        "  end Main; "
        "end C;");
    // Known-open #4047: chained outer-first ordering gap — pinned to
    // current behavior, not affected by this fix. The §8.1 arity
    // rejection is identical to pre-fix and post-fix states.
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "the universal `Object.Equals` member"));
}

// ============================================================================
// A4: §30.14 capture-free entry classification through a `type` alias.
// An alias-qualified static method reference used as the entry of
// `Domain.Spawn` must be classified as capture-free (static reference)
// exactly like the direct name. Hits: alias of class with static method,
// direct-name anti-regression, chained-outer-first known-open #4047.
// ============================================================================

// #4052 issue repro, verbatim (§30.14 / §27.8): an alias of a class used as
// the receiver of a static method reference passed as the entry of
// `Domain.Spawn`. Pre-fix: rejected with "Domain.Spawn entry must be
// capture-free: … a capturing lambda or an instance method reference is
// rejected …" — even though `AliasCls.Run` is structurally identical to
// `Cls.Run` (a static reference). Post-fix: compiles and the spawn runs,
// no capture-free rejection. (The async/spawn plumbing has its own
// surface errors — #2457 / domain serial — so we only assert the
// rejection text is NOT present and the program ran, not its exit code.)
BEZEL_TEST(type_alias_family_4052, class_alias_DomainSpawn_static_method_ref) {
    auto r = runAot(
        "module M; "
        "class Entry4052 "
        "  public static method Run(): int => 7; "
        "end Entry4052; "
        "type AliasEntry4052 := Entry4052; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    await Domain.Spawn(AliasEntry4052.Run); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(
        diagnosticMessageContains(r, "Domain.Spawn entry must be capture-free"));
}

// Direct-name anti-regression: the capture-free classification must still
// apply to the direct-name spelling — pins that the new TypeAliasSymbol
// arm did not disturb the existing TypeSymbol arm's static-method-ref
// classification.
BEZEL_TEST(type_alias_family_4052,
           class_direct_DomainSpawn_static_method_ref_regression) {
    auto r = runAot(
        "module M; "
        "class Entry4052b "
        "  public static method Run(): int => 7; "
        "end Entry4052b; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    await Domain.Spawn(Entry4052b.Run); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(
        diagnosticMessageContains(r, "Domain.Spawn entry must be capture-free"));
}

// KNOWN-OPEN #4047 control: a CHAINED alias (outer declared textually
// BEFORE inner) inherits the resolution-order gap — same shape as A1's
// control. Measured actual: the upstream ordering gap means
// `resolveAliasToTypeSymbol` returns null for the outer alias, so the
// entry is misclassified as not-capture-free, and the rejection IS the
// §30.14 capture-free diagnostic — identical to the pre-fix state, this
// fix has nothing to dispatch on. Pin by asserting the SAME §30.14
// rejection holds on this fix.
BEZEL_TEST(type_alias_family_4052,
           class_chained_alias_outer_first_DomainSpawn_known_open_4047) {
    auto r = runAot(
        "module M; "
        "type AliasEntryOuter4052 := AliasEntryInner4052; "
        "type AliasEntryInner4052 := EntryChained4052; "
        "class EntryChained4052 "
        "  public static method Run(): int => 7; "
        "end EntryChained4052; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    await Domain.Spawn(AliasEntryOuter4052.Run); "
        "    return 0; "
        "  end Main; "
        "end C;");
    // Known-open #4047: chained outer-first ordering gap — pinned to
    // current behavior, not affected by this fix. The rejection text
    // IS the §30.14 capture-free diagnostic, identical to the pre-fix
    // and post-fix states.
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "Domain.Spawn entry must be capture-free"));
}

// ============================================================================
// A5: §17.14 / §5.1 / §22.12 qualified-const fold through a `type` alias.
// An alias-qualified class member const `AliasCls.K` must drive §5.1
// narrowing exactly like `Cls.K`. Hits: alias of class with `const K`,
// direct-name anti-regression, chained-outer-first known-open #4047.
// ============================================================================

// #4052 issue repro, verbatim (§17.14 / §5.1 / §22.12 / §27.8): a class
// member const `K = 5` accessed via the alias as a `byte` initializer —
// the §5.1 narrowing must succeed since the const value is in `byte`
// range. Pre-fix: rejected with "the initializer for `x` has a type that
// is not implicitly convertible to the target type (§5.1)". Post-fix:
// 5 (the narrowed byte value).
BEZEL_TEST(type_alias_family_4052, class_alias_const_narrows_to_byte) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Cls4052c "
        "  public const K: int := 5; "
        "end Cls4052c; "
        "type AliasCls4052c := Cls4052c; "
        "class C "
        "  public static method Main(): int "
        "    var x: byte := AliasCls4052c.K; "
        "    return x; "
        "  end Main; "
        "end C;"), 5);
}

// Direct-name anti-regression: the §17.14 qualified-const fold must
// still apply to the direct-name spelling — pins that the new
// TypeAliasSymbol arm did not disturb the existing TypeSymbol arm's
// const-fold behavior.
BEZEL_TEST(type_alias_family_4052, class_direct_const_narrows_to_byte_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Cls4052d "
        "  public const K: int := 5; "
        "end Cls4052d; "
        "class C "
        "  public static method Main(): int "
        "    var x: byte := Cls4052d.K; "
        "    return x; "
        "  end Main; "
        "end C;"), 5);
}

// KNOWN-OPEN #4047 control: a CHAINED alias (outer declared textually
// BEFORE inner) inherits the resolution-order gap. The fold's alias-
// resolution arm is identical to the other three, so this control pins
// the same upstream ordering gap with the same KNOWN-OPEN posture.
// Measured actual: the upstream ordering gap means
// `resolveAliasToTypeSymbol` returns null for the outer alias, so the
// fold's alias-resolution arm fails, the fold returns nullopt, and
// §5.1's narrowing check falls through — the program compiles clean,
// runs, and returns the labeled-zero value (0) instead of 5. The
// driver emits a Severity::Warning #2434 backstop to stderr but that
// warning does NOT enter `r.diagnostics` (it's printed, not recorded
// via the DiagnosticEngine). Pin by asserting the program ran with
// the zero-initialized degraded value — the inherited state, identical
// to pre-fix and post-fix, this fix has nothing to dispatch on.
BEZEL_TEST(type_alias_family_4052,
           class_chained_alias_outer_first_const_known_open_4047) {
    auto r = runAot(
        "module M; "
        "type AliasClsOuter4052c := AliasClsInner4052c; "
        "type AliasClsInner4052c := ClsChained4052c; "
        "public class ClsChained4052c "
        "  public const K: int := 5; "
        "end ClsChained4052c; "
        "class C "
        "  public static method Main(): int "
        "    var x: byte := AliasClsOuter4052c.K; "
        "    return x; "
        "  end Main; "
        "end C;");
    // Known-open #4047: chained outer-first ordering gap. Pin by
    // asserting the SAME degraded-zero behavior holds on this fix:
    // program ran (silent wrong answer), exit 0, r.diagnostics empty.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.diagnostics.errorCount(), 0u);
}
