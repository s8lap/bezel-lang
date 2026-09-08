// Spec-derived tests for fix #4051 — §9.5 / §5.4 / §27.8 (v0.294): calling a
// `type` alias of a record/class like a function skipped the §9.5
// "constructed with `new`, not by calling the type name" rejection, and
// silently degraded to a WRONG VALUE with zero diagnostics — the WORST of
// the #4038/#4048/#4050/#4051 alias family: a crash at least announces
// itself; this one shipped bad data.
//
// `ExprTC::typeCall` (src/sema/ExprTypecheck_Calls.cpp) resolves the call
// expression's callee symbol and dispatches on its dynamic type:
// `dynamic_cast<NestedTypeSymbol*>` then `dynamic_cast<TypeSymbol*>`, with no
// `TypeAliasSymbol` arm — the identical missing-arm shape #4038 and #4048
// closed in their own call chains. `AliasOfRecord(1, 2)`'s callee resolves
// to a `TypeAliasSymbol`, so BOTH the §5.4 enum-cast arm and the §9.5
// "constructed with new" rejection were skipped entirely; the call reached
// codegen with nothing to lower and degraded through the #2434/#2491
// backstops to a compiles-clean, runs-clean, wrong value (measured pre-fix:
// `AliasOfRecord(1, 2)` returned 0 instead of the §9.5 compile error the
// direct `Record(1, 2)` spelling correctly produces).
//
// Fix: resolve the alias through the shared `sema::resolveAliasToTypeSymbol`
// free function (TypeResolution.hpp/.cpp) BEFORE dispatching on
// `TypeDeclKind`, so both the §5.4 enum-cast arm and the §9.5 rejection see
// through the alias exactly as they already do for a direct type name. This
// also correctly extends §5.4's enum-cast-argument validation to an
// alias-of-enum callee, which shared the identical gap (a value-typed grid
// cell, tested below).

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
constexpr const char* kCtorMsg = "not by calling the type name";
}  // namespace

// #4051 issue repro, verbatim (§9.5): calling a RECORD alias like a
// function must be rejected exactly like the direct name, not silently
// degrade to a wrong value.
BEZEL_TEST(type_alias_call_construction_4051, record_alias_call_rejected) {
    auto r = runAot(
        "module M; "
        "type AliasRecCall4051 := RecCall4051; "
        "public record RecCall4051(A: int, B: int); "
        "class C "
        "  public static method Main(): int "
        "    var r := AliasRecCall4051(1, 2); "
        "    return r.A + r.B; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kCtorMsg));
}

// §9.5 / §27.8: a CLASS alias in the same shape.
BEZEL_TEST(type_alias_call_construction_4051, class_alias_call_rejected) {
    auto r = runAot(
        "module M; "
        "type AliasClassCall4051 := ClassCall4051; "
        "public class ClassCall4051 "
        "  destructor end destructor; "
        "end ClassCall4051; "
        "class C "
        "  public static method Main(): int "
        "    var r := AliasClassCall4051(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kCtorMsg));
}

// Anti-regression: the DIRECT (non-alias) form must still be rejected
// identically — pins the fix's new resolution arm did not disturb the
// existing TypeSymbol arm's behavior.
BEZEL_TEST(type_alias_call_construction_4051, direct_record_call_still_rejected) {
    auto r = runAot(
        "module M; "
        "public record RecCallDirect4051(A: int, B: int); "
        "class C "
        "  public static method Main(): int "
        "    var r := RecCallDirect4051(1, 2); "
        "    return r.A + r.B; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kCtorMsg));
}

// §5.4 / §27.8: a bonus grid cell the SAME fix falls out of — an alias of an
// ENUM used as a checked cast (`AliasOfEnum(3)`) shares `typeCall`'s
// `dynamic_cast<TypeSymbol*>(calleeSym)` dispatch with the §9.5 record/class
// arm, so it was silently degrading identically pre-fix. Value-checked:
// §5.4 "the checked cast E(n) succeeds exactly when n equals the underlying
// value of some declared constant of E".
BEZEL_TEST(type_alias_call_construction_4051, enum_alias_checked_cast_correct_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasEnumCast4051 := ColorCast4051; "
        "enum ColorCast4051 Red, Green, Blue; end ColorCast4051; "
        "class C "
        "  public static method Main(): int "
        "    var c: ColorCast4051 := AliasEnumCast4051(1); "
        "    return (case c of ColorCast4051.Green => 7, _ => 0); "
        "  end Main; "
        "end C;"), 7);
}

// §5.4 / §27.8: the enum-cast alias grid cell's REJECTING half — an
// out-of-range constant must still be rejected through the alias exactly as
// through the direct name (anti-regression for the shared dispatch block).
BEZEL_TEST(type_alias_call_construction_4051,
           enum_alias_checked_cast_out_of_range_rejected) {
    auto r = runAot(
        "module M; "
        "type AliasEnumCastBad4051 := ColorCastBad4051; "
        "enum ColorCastBad4051 Red, Green, Blue; end ColorCastBad4051; "
        "class C "
        "  public static method Main(): int "
        "    var c: ColorCastBad4051 := AliasEnumCastBad4051(99); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "is not the underlying value of any declared constant"));
}

// ── REGRESSION for #4047 (§27.8, v0.294) ─────────────────────────────────
//
// A CHAINED alias (outer declared textually BEFORE inner) used to inherit
// #4047's resolution-order gap: `Resolver::resolveTopDecl`'s single
// forward pass had not yet resolved the inner alias's `aliasedType` when
// the outer alias's own `aliasedType` (a reference to the inner alias)
// was resolved, so `sema::resolveAliasToTypeSymbol` returned null and
// #4051's fix had nothing to dispatch on — the original silent-wrong-
// value bug reproduced (#2434/#2491 backstop degrading to 0).
//
// Fix #4047 adds `Resolver::resolveAliasForwardReferences` (a fixed-
// point re-resolution sweep over the top-level alias list) so the
// reverse declaration order now resolves exactly like the working
// inner-before-outer order — and the §9.5 record-call rejection that
// #4051's fix dispatches then fires on the same name, with the same
// diagnostic, regardless of declaration order. A DIRECT (non-chained)
// alias declared before its target class/record was UNAFFECTED (its
// target's TypeSymbol exists from symbol collection regardless of
// declaration order; only a TypeAliasSymbol needs its OWN `aliasedType`
// resolved first) — confirmed separately, not itself a #4047 case, and
// not pinned here.
BEZEL_TEST(type_alias_call_construction_4051,
           chained_alias_REVERSE_ORDER_call_4047) {
    auto r = runAot(
        "module M; "
        "type AliasRecCallRevOuter4051 := AliasRecCallRevInner4051; "
        "type AliasRecCallRevInner4051 := RecCallRevOuter4051; "
        "public record RecCallRevOuter4051(A: int, B: int); "
        "class C "
        "  public static method Main(): int "
        "    var r := AliasRecCallRevOuter4051(1, 2); "
        "    return r.A + r.B; "
        "  end Main; "
        "end C;");
    // FIXED (#4047): was a #2434/#2491-degraded 0, now the §9.5
    // rejection that #4051's fix dispatches — same diagnostic the
    // direct-record-call control pins above.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kCtorMsg));
}
