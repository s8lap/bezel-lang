// Regression tests for issue #3907 — `FrozenArray<T>.From(...)`'s
// UNANNOTATED, static-factory-result TypeRef shape silently bypassed the
// #3896 (stdlib §4.19 / language spec §8.3/§8.5) Object-contract audit.
//
// residual_collections_unresolved_member_3896_test.cpp's own comment (and
// its "EVERY local below uses an EXPLICIT type annotation" note on the
// FrozenArray section) documents that #3896's sema/codegen changes were
// gated on the BARE-ANNOTATION TypeRef shape (`var a: FrozenArray<T> :=
// ...`, `declaration == nullptr`, a real astNode head) and deliberately did
// NOT reach the static-factory-result shape (`var a :=
// FrozenArray<T>.From(...)`, `declaration == builtinClassSym("FrozenArray")`,
// `headName() == ""`) — the far more idiomatic form, used throughout that
// very test file's own OTHER (non-FrozenArray) helper functions. Measured on
// the pre-fix tree: `a.ToString()` on the unannotated form compiled clean
// (exit 0) and returned "1" (the mismatch branch) instead of "42" — a
// SILENTLY WRONG runtime value, no diagnostic at all.
//
// Root cause (found by a live checkpoint trace through
// `ExprTC::resolveMemberAccess`, ExprTypecheck_Calls.cpp): the declaration-
// set shape's `targetType` is set from `tt->declaration` early in that
// function (the general "declared-type member lookup" resolution, well
// before the `isAuditedElsewhere` block reasoned about in the issue), which
// routes the receiver through the ordinary "declared class type, no member
// found" fallback instead of through `isAuditedElsewhere`'s FrozenArray
// carve-outs — so widening `isAuditedElsewhere`'s own gate (the issue's
// literal suggestion) is a no-op: that code is provably unreachable for this
// shape. The actual fix has two independent parts:
//
//   1. CODEGEN (LlvmBackend_Calls2.cpp `lowerCallDispatch`): an EARLY
//      `cg_isClass` interceptor (predating the `isAuditedElsewhere`-audited
//      literal-chain fix by several thousand lines in the same function)
//      matched ANY receiver whose `declaration->kind == TypeDeclKind::Class`
//      — true for `builtinClassSym("FrozenArray")`, since `builtinClassSym`
//      sets `kind = Class` unconditionally (SemaContext.hpp) — and routed
//      `.ToString()` through `lowerObjectToString` (vtable slot 3),
//      misreading FrozenArray's `bz_array_t*` payload as if it had a class
//      vtable header. Excluded via `builtinTypeName(cg_recvTy) !=
//      "FrozenArray"` (a no-op exclusion for every genuine user class, whose
//      TypeSymbol always has `decl != nullptr`).
//   2. SEMA (ExprTypecheck_Calls.cpp `resolveMemberAccess`): the generic
//      `if (!member) { ... }` fallback (reached because `lookupMember` finds
//      nothing on the synthetic, empty-`members` FrozenArray TypeSymbol)
//      needs the SAME `CompareTo`-reject / bare-reference-reject carve-outs
//      `isAuditedElsewhere` already gives the bare-annotation shape, added
//      as a narrowly-scoped (`targetType->name == "FrozenArray" &&
//      targetType->decl == nullptr && targetType->moduleName.empty()`)
//      sibling check.
//
// Cite: stdlib §4.19, language spec §8.1/§8.3/§8.5/§27.6, v0.295.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <initializer_list>
#include <string>

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Per tests/README.md: a genuine compile-time rejection must be pinned by
// diagnostic TEXT, never by exit code or `!r.ran` alone (a runtime crash
// also synthesizes an Error diagnostic via the WIFSIGNALED path).
bool hasErrorAll(const std::string& program,
                  std::initializer_list<const char*> keywords) {
    auto r = runAot(program);
    if (!bezel::test::failedAtCompileTimeWithRealLocation(r)) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        bool all = true;
        for (const char* k : keywords) {
            if (d.message.find(k) == std::string::npos) { all = false; break; }
        }
        if (all) return true;
    }
    return false;
}

bool hasError(const std::string& program, const char* keyword) {
    return hasErrorAll(program, {keyword});
}

std::string program(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           "end Main; end C;";
}

}  // namespace

// ═══════════ Primary bug: the measured "silently wrong value" repro ═══════
// The exact program from the issue's "Measured repro" section — the
// UNANNOTATED construction form (`var a := FrozenArray<int>.From(...)`).
// Pre-fix: compiles clean, RUNS, returns 1 (the wrong-string branch).
// Post-fix: returns 42 (the correct-qualified-name branch) — parity with
// the already-correct annotated form residual_collections_unresolved_
// member_3896_test.cpp pins.
BEZEL_TEST(frozen_array_3907,
           unannotated_construction_tostring_correct_value_spec_8_5_stdlib_4_19) {
    const std::string p = program(
        "  var a := FrozenArray<int>.From([1, 2, 3]); "
        "  var s := a.ToString(); "
        "  return (if s = \"System.Collections.Generic.FrozenArray<int>\" "
        "          then 42 else 1); ");
    BEZEL_EXPECT_EQ(run(p), 42);
}

// Accept-side control: the ALREADY-correct annotated form must remain
// correct (unaffected by this fix — #3896 already covers this shape).
BEZEL_TEST(frozen_array_3907,
           annotated_construction_tostring_correct_value_control_spec_8_5) {
    const std::string p = program(
        "  var a: FrozenArray<int> := FrozenArray<int>.From([1, 2, 3]); "
        "  var s := a.ToString(); "
        "  return (if s = \"System.Collections.Generic.FrozenArray<int>\" "
        "          then 42 else 1); ");
    BEZEL_EXPECT_EQ(run(p), 42);
}

// ═══════════ Equals / GetHashCode identity default — accept-side control ══
// §8.3's "Class" default (reference equality / identity-based hash) must
// keep working correctly for the unannotated shape too — proves the
// codegen `cg_isClass` exclusion narrows ONLY the ToString interceptor and
// does not disturb the (separately-gated) Equals/GetHashCode dispatch.
BEZEL_TEST(frozen_array_3907,
           unannotated_equals_and_gethashcode_identity_spec_8_3_stdlib_4_19) {
    const std::string p = program(
        "  var a1 := FrozenArray<int>.From([1, 2, 3]); "
        "  var a2 := FrozenArray<int>.From([1, 2, 3]); "  // same CONTENTS, distinct objects
        "  var selfEq := a1.Equals(a1); "
        "  var crossEq := a1.Equals(a2); "
        "  var h1a := a1.GetHashCode(); "
        "  var h1b := a1.GetHashCode(); "
        "  if not selfEq then return 1; end; "
        "  if crossEq then return 2; end; "                // reference equality: distinct objects, unequal
        "  if h1a <> h1b then return 3; end; "
        "  return 42; ");
    BEZEL_EXPECT_EQ(run(p), 42);
}

// ═══════════ CompareTo — precise stdlib §4.19 rejection, both shapes ══════
// stdlib §4.19's class listing declares `IEnumerable<T>` only — no
// `IComparable<T>` — so `CompareTo` has no member on FrozenArray<T> at all.
// Pre-fix (unannotated shape): rejected too, but via codegen's generic "no
// dispatch arm emitted any code for this call ... (#2457)" backstop
// (confirmed live) — NOT this precise stdlib-citing message, so this row
// still fails-without on message TEXT even though `r.ran` is already false
// on both sides.
BEZEL_TEST(frozen_array_3907,
           unannotated_compareto_call_rejected_precise_message_stdlib_4_19) {
    const std::string p = program(
        "  var a1 := FrozenArray<int>.From([1, 2, 3]); "
        "  var a2 := FrozenArray<int>.From([4, 5, 6]); "
        "  var c := a1.CompareTo(a2); "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(
        hasError(p, "no member `CompareTo` on `FrozenArray<T>` (stdlib "
                    "\xC2\xA7""4.19 declares no `IComparable<T>` "
                    "conformance for this type)"));
}

// Parity control: the annotated shape gets the SAME precise message (via
// #3896's own kResidualAuditedNames carve-out) — both shapes now agree.
BEZEL_TEST(frozen_array_3907,
           annotated_compareto_call_rejected_precise_message_parity) {
    const std::string p = program(
        "  var a1: FrozenArray<int> := FrozenArray<int>.From([1, 2, 3]); "
        "  var a2: FrozenArray<int> := FrozenArray<int>.From([4, 5, 6]); "
        "  var c := a1.CompareTo(a2); "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(
        hasError(p, "no member `CompareTo` on `FrozenArray<T>` (stdlib "
                    "\xC2\xA7""4.19 declares no `IComparable<T>` "
                    "conformance for this type)"));
}

// ═══════════ Bare (uncalled) ToString/Equals/GetHashCode reference ════════
// language spec §27.6: a bare reference to a real universal Object member
// that this builtin dispatches structurally (no real MethodSymbol) forms no
// function value. Pre-fix (unannotated shape): NOT rejected at all —
// `r.ran == true`, exit 0 — the reference silently degraded through the
// #2434 zero-initialized-slot fallback (an internal Warning, not a compile
// Error). Post-fix: a real compile-time Error, matching the annotated
// shape's existing #3896 carve-out text exactly.
BEZEL_TEST(frozen_array_3907,
           unannotated_bare_tostring_reference_rejected_not_degraded_stdlib_4_19) {
    const std::string p = program(
        "  var a := FrozenArray<int>.From([1, 2, 3]); "
        "  var f := a.ToString; "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(hasErrorAll(
        p, {"a bare reference to `ToString`",
            "not yet supported on `FrozenArray<T>`"}));
}

BEZEL_TEST(frozen_array_3907,
           unannotated_bare_equals_reference_rejected_not_degraded_stdlib_4_19) {
    const std::string p = program(
        "  var a := FrozenArray<int>.From([1, 2, 3]); "
        "  var f := a.Equals; "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(hasErrorAll(
        p, {"a bare reference to `Equals`",
            "not yet supported on `FrozenArray<T>`"}));
}

BEZEL_TEST(frozen_array_3907,
           unannotated_bare_gethashcode_reference_rejected_not_degraded_stdlib_4_19) {
    const std::string p = program(
        "  var a := FrozenArray<int>.From([1, 2, 3]); "
        "  var f := a.GetHashCode; "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(hasErrorAll(
        p, {"a bare reference to `GetHashCode`",
            "not yet supported on `FrozenArray<T>`"}));
}

// Parity control: the annotated shape's existing #3896 bare-reference
// carve-out (residual_collections_unresolved_member_3896_test.cpp) gives
// the IDENTICAL wording — both shapes now agree end to end.
BEZEL_TEST(frozen_array_3907,
           annotated_bare_tostring_reference_rejected_parity) {
    const std::string p = program(
        "  var a: FrozenArray<int> := FrozenArray<int>.From([1, 2, 3]); "
        "  var f := a.ToString; "
        "  return 0; ");
    BEZEL_EXPECT_TRUE(hasErrorAll(
        p, {"a bare reference to `ToString`",
            "not yet supported on `FrozenArray<T>`"}));
}
