// Spec-derived tests for fix #4073 — §27.8 / §9.5 / §5.4 / §30.14 (v0.294):
// two SEMA receiver classifiers were dynamic_cast<TypeSymbol*>-only with no
// TypeAliasSymbol arm, so a `type X := T` alias receiver was mis-classified
// in both:
//
//   (1) the universal-member arity gate (ExprTypecheck_Calls.cpp ~3951,
//       `targetIsStaticReceiver`), where a call like `AliasT.GetHashCode(1)`
//       is treated as an INSTANCE call to the universal §8.1 surface and
//       then rejected for wrong arity — `X` denotes a TYPE, not a value, so
//       the existing static-receiver guard should suppress the universal
//       check exactly like it does for the direct `T.GetHashCode(1)`
//       spelling (the #2446 false-reject guard's stated intent).
//
//   (2) `ExprTC::isCaptureFreeEntry` (ExprTypecheck_Calls.cpp ~22131), the
//       Domain.Spawn entry classifier for §30.14's capture-free register.
//       A static method reference `T.Entry` is recognized as capture-free;
//       a `type X := T; X.Entry` should be EQUIVALENT (the alias and its
//       target denote the same type, §27.8), but pre-fix the qualifier
//       resolves to a TypeAliasSymbol, not a TypeSymbol, so `X.Entry` falls
//       through to "instance method reference" and the spawn is rejected
//       with "Domain.Spawn entry must be capture-free: ... instance method
//       reference is rejected because captures and receivers are borrows
//       of the origin domain (§30.14, #600)".
//
// Both predicates share the identical missing-arm shape #4038/#4048/#4050/
// #4051/#4070/#4076 all fixed in their own call chains: a classifier that
// resolves a WRITTEN NAME to a Symbol via `dynamic_cast<TypeSymbol*>` with
// no TypeAliasSymbol arm. Fix: in both sites, also accept a TypeAliasSymbol
// and resolve through the shared `sema::resolveAliasToTypeSymbol` free
// function (TypeResolution.hpp/.cpp) that #4050/#4051 introduced — the
// established hoisting pattern the umbrella #4052 enumerates. Both sites
// are sema-only; codegen is unaffected (the arity gate is a pre-call check,
// and capture-freedom is a sema-stated position in §30.14's register that
// codegen consumes via the already-correct call shape). For #4073
// specifically the program is legitimate and the alias form is the same
// program as the direct spelling — only resolving the alias, as this fix
// does, makes the alias form compile and compute the correct value, not a
// guard that would only make it fail cleanly (compare #4062's complementary
// finding for a different codegen site, which #4076's own commit documents
// at length as the wrong fix for #4076's shape).

#include "support/aot_support.hpp"  // runAot
#include "support/aot_result_asserts.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::failedAtCompileTimeWithRealLocation;

// ── (1) Universal-member arity gate, alias-as-type-name receiver ───────────
//
// §27.8 / §9.5 / fix #4073, arity-gate axis (ExprTypecheck_Calls.cpp ~3951):
// A `type` ALIAS receiver calling a UNIVERSAL member name with WRONG ARITY
// must NOT be rejected by the §8.1 universal-arity gate. The alias denotes
// a TYPE (not a value), so the existing `targetIsStaticReceiver` guard must
// suppress the universal check exactly like it does for the direct
// `T.Member(...)` spelling.
//
// The arity gate at line 3967 fires only when the call resolves to a
// universal-member NAME (Equals, GetHashCode, ToString, GetType, Dispose)
// with NO user-defined method of that name on the receiver class — i.e.,
// when the universal-instance fallback is in play. Pre-fix the alias
// receiver's TypeAliasSymbol fell through `dynamic_cast<TypeSymbol*>`, the
// guard stayed false, and the call was rejected with
//   "the universal `Object.GetHashCode` member takes 0 argument(s) but
//    the call supplies 1 (§8.1)"
// — measured against `eae547e` with `type AliasFoo4073 := Foo4073` plus a
// class with NO static GetHashCode (so the universal-instance fallback is
// reached).
//
// Post-fix the alias receiver is recognized as the same TYPE-NAME
// receiver as the direct spelling — the guard's TypeAliasSymbol arm
// returns true exactly like the direct TypeSymbol arm, the gate is
// suppressed, and the program falls through to codegen with the same
// shape (and the same #2491 backstop error) as the direct spelling does.
//
// Pin shape: the alias form must be REJECTED pre-fix and ACCEPTED post-fix
// (with the #2491 backstop) — same as the direct-name form. The
// direct-name control below proves the post-fix shape is identical.
BEZEL_TEST(type_alias_receiver_classifiers_4073,
           class_alias_universal_member_wrong_arity_accepted_arity_gate) {
    auto r = runAot(
        "module M; "
        "public class Foo4073 "
        "  destructor end destructor; "
        "end Foo4073; "
        "type AliasFoo4073 := Foo4073; "
        "class C "
        "  public static method Main(): int "
        "    return AliasFoo4073.GetHashCode(1); "
        "  end Main; "
        "end C;");
    // Pre-fix: REJECTED with "the universal `Object.GetHashCode` member
    // takes 0 argument(s) but the call supplies 1 (§8.1)" — false-reject on
    // the arity gate (the alias receiver's TypeAliasSymbol fell through
    // `dynamic_cast<TypeSymbol*>`, leaving `targetIsStaticReceiver` false
    // so the gate fired on a TYPE-NAME receiver exactly like it would for
    // a value receiver).
    //
    // Post-fix: NOT REJECTED at the arity gate. The alias receiver is now
    // recognized as static-receiver (the same TYPE-NAME shape as the
    // direct spelling), the gate is suppressed, and the program reaches
    // codegen with the same shape as the direct-name form below — the
    // same #2491 "return-value expression failed to lower" backstop is
    // reached because `Foo4073.GetHashCode` doesn't actually exist as a
    // static method (the alias-name receiver is type-correct, but no method
    // is dispatchable, so codegen can't lower the call). The alias form
    // must now produce IDENTICAL diagnostics to the direct form — proves
    // the alias is resolved exactly like the direct spelling.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // Same value as the direct-name control: the alias form is now
    // behaviorally indistinguishable from the direct form.
    BEZEL_EXPECT_EQ(r.exitCode, 0);  // backstop falls through, exit 0
}

// Direct-name (non-alias) control — pins that the direct spelling has
// always been accepted past the arity gate (the pre-existing TypeSymbol
// arm, never broken by the pre-fix gap). The alias form must produce the
// SAME shape as this control post-fix.
BEZEL_TEST(type_alias_receiver_classifiers_4073,
           direct_class_universal_member_wrong_arity_anti_regression) {
    auto r = runAot(
        "module M; "
        "public class Foo4073d "
        "  destructor end destructor; "
        "end Foo4073d; "
        "class C "
        "  public static method Main(): int "
        "    return Foo4073d.GetHashCode(1); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);  // backstop falls through, exit 0
}

// Anti-regression: an INSTANCE call through an ALIAS-TYPED LOCAL with the
// wrong arity for the §8.1 universal surface must STILL be rejected —
// the alias-as-static-receiver fix must not have accidentally let the
// arity check slip on instance calls. `x.GetHashCode(1)` with `x: AliasT`
// and `AliasT := T` (a class) takes 0 universal args, so 1 arg is wrong.
// Pre-fix AND post-fix the arity check fires for the instance call — the
// fix only adds an arm to the static-receiver guard, which fires only when
// the receiver's NAME denotes a type (an IdentifierExpr target whose
// resolved exprSymbol is a TypeSymbol-or-TypeAliasSymbol).
BEZEL_TEST(type_alias_receiver_classifiers_4073,
           instance_call_via_alias_typed_local_arity_still_rejected) {
    auto r = runAot(
        "module M; "
        "public class Foo4073 "
        "  destructor end destructor; "
        "end Foo4073; "
        "type AliasFoo4073 := Foo4073; "
        "class C "
        "  public static method Main(): int "
        "    using var x := new Foo4073(); "
        "    return x.GetHashCode(1); "
        "  end Main; "
        "end C;");
    // Pre-fix AND post-fix: BOTH rejected at sema with the universal-arity
    // message (the value-receiver path is unchanged — the fix's new arm
    // only fires on IdentifierExpr receiver targets whose exprSymbol is a
    // TypeAliasSymbol, a type-NAME receiver).
    BEZEL_EXPECT_FALSE(r.ran);
    bool sawArityRejection = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("universal `Object.GetHashCode`") !=
                std::string::npos &&
            d.message.find("takes 0 argument(s)") !=
                std::string::npos) {
            sawArityRejection = true;
            break;
        }
    }
    BEZEL_EXPECT_TRUE(sawArityRejection);
}

// ── (2) `isCaptureFreeEntry` alias arm ─────────────────────────────────────
//
// §30.14 / fix #4073, capture-free axis (ExprTypecheck_Calls.cpp ~22131):
// a static method reference `AliasT.Entry` (Domain.Spawn entry) must be
// recognized as CAPTURE-FREE exactly like the direct `T.Entry` spelling,
// because §27.8 makes the alias and its underlying type the same TYPE and
// §30.14's capture-free register admits "a static method reference" — the
// alias-receiver spelling is the same reference, just named through an
// alias. Pre-fix the alias receiver's TypeAliasSymbol fell through
// `dynamic_cast<TypeSymbol*>` and `isCaptureFreeEntry` returned false, so
// the spawn was rejected with
//   "Domain.Spawn entry must be capture-free: a non-capturing lambda or a
//    static method reference — a capturing lambda or an instance method
//    reference is rejected because captures and receivers are borrows of
//    the origin domain (§30.14, #600)"
// — measured pre-fix below.
//
// The value-verify: `Domain.Spawn(AliasT.Entry)` spawns the entry, the task
// runs to completion returning 42, `await t` joins, Main returns 42.
// Verifies the alias-receiver spelling works end-to-end, not just that
// the sema-time rejection is gone (LANE_RULES §15).
BEZEL_TEST(type_alias_receiver_classifiers_4073,
           spawn_alias_receiver_static_method_capture_free_accepted) {
    auto r = runAot(
        "module M; "
        "public class Foo4073s "
        "  public static method Entry(): int => 42; "
        "  destructor end destructor; "
        "end Foo4073s; "
        "type AliasFoo4073s := Foo4073s; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var t := Domain.Spawn(AliasFoo4073s.Entry); "
        "    await t; "
        "    return 42; "
        "  end Main; "
        "end C;");
    // Pre-fix: rejected at sema with the §30.14/#600 capture-free message.
    // Post-fix: compiles clean and the spawned entry's task runs to 42.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// Direct-name (non-alias) control — the pre-existing TypeSymbol arm
// already accepted this spelling, so this control must continue to compile
// and compute 42 identically to the alias form.
BEZEL_TEST(type_alias_receiver_classifiers_4073,
           spawn_direct_receiver_static_method_anti_regression) {
    auto r = runAot(
        "module M; "
        "public class Foo4073sd "
        "  public static method Entry(): int => 42; "
        "  destructor end destructor; "
        "end Foo4073sd; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var t := Domain.Spawn(Foo4073sd.Entry); "
        "    await t; "
        "    return 42; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// KNOWN-OPEN pin for the inherited #4047 declaration-order hazard. A
// CHAINED alias declared textually BEFORE its inner alias inherits the
// same resolution-order gap every other member of the family pins
// (#4038, #4048, #4050, #4051, #4076 all pin a version of this). The
// OUTER alias's `aliasedType` (which is the INNER alias) is resolved
// before the INNER alias itself has been resolved by `Resolver::
// resolveTopDecl`'s single forward pass, so `resolveAliasToTypeSymbol`
// returns null for the OUTER alias and this fix has nothing to dispatch
// on. Measured: rejected at sema with the §30.14/#600 message, identical
// to the pre-fix behavior. The alias-THEN-target ordering (the inner
// alias first, the outer alias second) works — and is the relevant
// ALIAS-FIRST order that #4073's pre-fix failure manifests in (since the
// alias must be declared BEFORE the call site, in source order).
//
// This row is intentionally left green both pre-fix and post-fix: it
// documents that the #4047 ordering hazard is INHERITED, not introduced
// by this fix. A future fix to `Resolver::resolveTopDecl`'s single
// forward pass would resolve it; that's a resolution-shape change too
// broad for a narrow alias-focused sweep like this one.
BEZEL_TEST(type_alias_receiver_classifiers_4073,
           chained_alias_outer_first_spawn_known_open_4047) {
    auto r = runAot(
        "module M; "
        "type AliasOuter4073 := AliasInner4073; "
        "public class Foo4073sc "
        "  public static method Entry(): int => 42; "
        "  destructor end destructor; "
        "end Foo4073sc; "
        "type AliasInner4073 := Foo4073sc; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var t := Domain.Spawn(AliasOuter4073.Entry); "
        "    await t; "
        "    return 42; "
        "  end Main; "
        "end C;");
    // KNOWN-OPEN (#4047): pre-fix and post-fix both reject with the
    // §30.14/#600 message because `resolveAliasToTypeSymbol` returns null
    // for the outer alias (its aliasedType, the inner alias, has not yet
    // been resolved when the outer alias's resolution runs). The correct
    // behavior is for this to ACCEPT and return 42 — that's the
    // resolution-order gap, separately tracked by #4047.
    // narrow #3892 (v0.294): a bare `!ran` cannot tell a §30.14 reject
    // from a runtime crash/signal/timeout/ASan report; the genuine §30.14
    // reject carries a real `<path>:<line>:<col>` prefix, the synthetics
    // never do.
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}