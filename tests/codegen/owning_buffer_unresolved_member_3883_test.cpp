// Regression tests for issue #3883 — the closed-#2457 audit sweep never
// covered `OwningBuffer<T>` (stdlib §4.14), so `.Equals()`/`.GetHashCode()`/
// `.ToString()` were safely REJECTED at compile time with a diagnostic
// pointing at a closed issue, and remained genuinely unimplemented.
//
// Provenance: c6-l6 was dispatched on #3683 (claiming these members fell
// through to a silent identity comparison), measured two probes, and found
// the opposite — a #2465 guard cleanly rejects the call before codegen is
// ever reached. It closed #3683 as REFUTED and filed #3883 for the real,
// adjacent defect: the feature gap itself. This file implements that slice.
//
// Per stdlib §4.14, `OwningBuffer<T>` is a real, declaration-less, `public
// class` (`public class OwningBuffer<T>: IEnumerable<T?>` for class-typed T;
// `public class OwningBuffer<T>` with no IEnumerable conformance for
// value-typed T) with no Equals/GetHashCode/ToString override shown. It is
// `bz_array_t*`-backed (common/BuiltinRegistry.cpp's kArrayBackedCollections,
// the SAME registry Array/OwningArray/Stack/OwningStack/Queue/OwningQueue —
// #2457 slices 1/2 — are in), so language spec §8.3's "Class" default
// (reference equality / identity-based hash) and §8.5's class default
// (qualified name) apply mechanically. This is the fix's slice 3.
//
// The sema `isAuditedElsewhere` allowlist (ExprTypecheck_Calls.cpp) that
// gates this is all-or-nothing per type name across FOUR members
// (ToString/GetHashCode/Equals/CompareTo, #2465). Unlike its five siblings,
// `OwningBuffer<T>` declares no `IComparable<T>` conformance at all (stdlib
// §4.14's full member list has no `CompareTo`), so this fix pins TWO
// carve-outs alongside the three implemented members:
//   - `.CompareTo(...)` on OwningBuffer must still be rejected with a
//     distinct, accurate "no member" diagnostic. The CALL form
//     (`a1.CompareTo(a2)`) was already caught by a SEPARATE, pre-existing
//     codegen backstop (`LlvmBackend_Calls2.cpp`'s "no dispatch arm emitted
//     any code" reject) even without this carve-out — measured directly,
//     not assumed, after an earlier draft of this fix wrongly claimed the
//     call form would silently succeed. This carve-out's load-bearing job
//     for the CALL form is precision (the generic backstop's own message
//     still cites the closed `#2457`, exactly the stale reference #3883
//     exists to retire) and consistency with the bare-reference case below.
//     The BARE (uncalled) reference form (`var f := a1.CompareTo;`) has NO
//     such backstop and genuinely does degrade to the #2434
//     zero-initialized-slot fallback without this carve-out — confirmed
//     live by temporarily disabling it — so for the bare form the carve-out
//     is the only thing standing between a loud Error and a silent Warning.
//   - a BARE (uncalled) reference to `.ToString`/`.Equals`/`.GetHashCode`
//     (language spec §27.6) must also still be rejected (mirroring the
//     #2470 precedent for Array/OwningArray) rather than silently
//     degrading, since these three are dispatched structurally in codegen
//     rather than through a real MethodSymbol and no function value can be
//     formed for them.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Per tests/README.md: runAot's WIFSIGNALED path also synthesizes an Error
// diagnostic for a runtime crash, so a genuine compile-time rejection must
// be distinguished by asserting on the diagnostic TEXT, never by exit code
// or `!r.ran` alone.
bool hasError(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// ── Equals/GetHashCode: implemented via direct identity, VALUE-correct ──

// Value-typed T. FAILS without the fix: the #2465 placeholder rejects both
// calls ("declared via the universal Object contract ... tracked #2457").
BEZEL_TEST(owning_buffer_unresolved_member_3883,
           equals_and_gethashcode_identity_semantics_value_t_spec_8_3) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var a1 := new OwningBuffer<int>(4); "
        "  var a2 := new OwningBuffer<int>(4); "
        "  var selfEq := a1.Equals(a1); "
        "  var crossEq := a1.Equals(a2); "
        "  var h1a := a1.GetHashCode(); "
        "  var h1b := a1.GetHashCode(); "
        "  a1.Dispose(); a2.Dispose(); "
        "  if not selfEq then return 1; end; "
        "  if crossEq then return 2; end; "
        "  if h1a <> h1b then return 3; end; "
        "  return 42; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// Class-typed T — same §8.3 default, different slot category (nullable
// owned references rather than plain value slots); identity comparison is
// independent of the slot category, so this pins that the codegen
// `isBuiltinIdentityArrayFamily` gate is not accidentally value-T-only.
BEZEL_TEST(owning_buffer_unresolved_member_3883,
           equals_and_gethashcode_identity_semantics_class_t_spec_8_3) {
    const std::string program =
        "module M; "
        "class Item public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end Item; "
        "class C public static method Main(): int "
        "  var a1 := new OwningBuffer<Item>(2); "
        "  var a2 := new OwningBuffer<Item>(2); "
        "  var selfEq := a1.Equals(a1); "
        "  var crossEq := a1.Equals(a2); "
        "  var h1a := a1.GetHashCode(); "
        "  var h1b := a1.GetHashCode(); "
        "  a1.Dispose(); a2.Dispose(); "
        "  if not selfEq then return 1; end; "
        "  if crossEq then return 2; end; "
        "  if h1a <> h1b then return 3; end; "
        "  return 42; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// ── ToString: implemented as the qualified-name literal, VALUE-correct ──

// fix #3119 (spec §8.5, v0.252; stdlib §2.13 rule 1 + rule 3): the literal
// is the QUALIFIED module path with the rule-3 generic-args tail, matching
// `kBuiltinClassModulePaths`'s pre-existing "System.Collections.Generic"
// entry for OwningBuffer (consulted independently by Type.FullName/typeof).
BEZEL_TEST(owning_buffer_unresolved_member_3883,
           tostring_implemented_value_t_literal_spec_8_5) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var a := new OwningBuffer<int>(4); "
        "  var s := a.ToString(); "
        "  a.Dispose(); "
        "  return (if s = \"System.Collections.Generic.OwningBuffer<int>\" "
        "          then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

BEZEL_TEST(owning_buffer_unresolved_member_3883,
           tostring_implemented_class_t_literal_spec_8_5) {
    const std::string program =
        "module M; "
        "class Item public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end Item; "
        "class C public static method Main(): int "
        "  var a := new OwningBuffer<Item>(2); "
        "  var s := a.ToString(); "
        "  a.Dispose(); "
        "  return (if s = \"System.Collections.Generic.OwningBuffer<M.Item>\" "
        "          then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// ── CompareTo: NOT declared on OwningBuffer (stdlib §4.14) — still rejected,
//    now with a distinct "no member" diagnostic, not the closed-#2457 one ──

// `OwningBuffer<T>` is neither in the #920/#1112 `isOwningContainer` nor
// `isNonOwningContainer` call-surface allowlist (ExprTypecheck_Calls.cpp
// explicitly excludes it: "not OwningBuffer / the owning iterators, whose
// surfaces differ"). Without this fix's carve-out, the CALL form is still
// rejected — measured directly: a SEPARATE, pre-existing codegen backstop
// (`LlvmBackend_Calls2.cpp`'s "no dispatch arm emitted any code" reject)
// still catches it, because `CompareTo` never resolves to a real
// MethodSymbol or codegen dispatch arm on this receiver either way. This
// test's own FAIL-WITHOUT is therefore a diagnostic-TEXT change, not a
// reject-vs-accept change: without the carve-out the receiver is rejected
// with the generic backstop's wording (which cites the closed `#2457` —
// exactly the stale reference this issue exists to retire); with it, the
// precise, accurate "no member ... no IComparable<T> conformance" reject
// fires instead, and the stale wording is gone. See
// `compareto_bare_reference_also_rejected` below for the sub-case where the
// carve-out genuinely is the only thing preventing a silent accept.
BEZEL_TEST(owning_buffer_unresolved_member_3883,
           compareto_call_rejected_no_icomparable_conformance_stdlib_4_14) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var a1 := new OwningBuffer<int>(4); "
        "  var a2 := new OwningBuffer<int>(4); "
        "  var c := a1.CompareTo(a2); "
        "  a1.Dispose(); a2.Dispose(); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        hasError(program, "no member `CompareTo` on `OwningBuffer<T>`"));
    // The stale, closed-#2457 wording (from EITHER the old #2465 placeholder
    // or the generic codegen backstop this carve-out preempts) must not ALSO
    // fire.
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics())
        BEZEL_EXPECT_TRUE(
            d.message.find("(#2457)") == std::string::npos &&
            d.message.find("declared via the universal Object contract") ==
                std::string::npos);
}

// A bare (uncalled) `.CompareTo` reference takes the identical carve-out
// (it does not distinguish call form), so it is rejected the same way. This
// IS the load-bearing sub-case: the generic codegen backstop the call-form
// test above relies on as a fallback only fires for a CALL — a bare
// reference reaches sema's `#2434` degraded-zero-initialization path
// instead, which is a Warning, not an Error, so WITHOUT this carve-out the
// program would compile clean to exit 0. Confirmed live by temporarily
// disabling just this carve-out: the bare form degrades to the #2434
// Warning while the call form above still gets an Error either way.
BEZEL_TEST(owning_buffer_unresolved_member_3883,
           compareto_bare_reference_also_rejected) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var a1 := new OwningBuffer<int>(4); "
        "  var f := a1.CompareTo; "
        "  a1.Dispose(); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        hasError(program, "no member `CompareTo` on `OwningBuffer<T>`"));
}

// ── Bare references to ToString/Equals/GetHashCode: still rejected (#2470
//    precedent), not silently degraded ──────────────────────────────────

// FAILS without the fix's bare-reference carve-out: no diagnostic at all
// (the #2434 fallback is a Warning, not an Error, so the program still
// "compiles" with `f` a zero-initialized, uncallable slot) — the exact
// "loud -> quiet" regression the standing protocol forbids introducing
// while implementing the CALL form above. PASSES with it: an honest reject.
BEZEL_TEST(owning_buffer_unresolved_member_3883,
           bare_tostring_reference_rejected_not_silently_degraded) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var a := new OwningBuffer<int>(4); "
        "  var f := a.ToString; "
        "  a.Dispose(); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "a bare reference to `ToString` (language spec \xC2\xA7"
        "27.6) is not yet supported on `OwningBuffer<T>`"));
}

BEZEL_TEST(owning_buffer_unresolved_member_3883,
           bare_equals_reference_rejected_not_silently_degraded) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var a := new OwningBuffer<int>(4); "
        "  var f := a.Equals; "
        "  a.Dispose(); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "a bare reference to `Equals` (language spec \xC2\xA7"
        "27.6) is not yet supported on `OwningBuffer<T>`"));
}

BEZEL_TEST(owning_buffer_unresolved_member_3883,
           bare_gethashcode_reference_rejected_not_silently_degraded) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var a := new OwningBuffer<int>(4); "
        "  var f := a.GetHashCode; "
        "  a.Dispose(); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "a bare reference to `GetHashCode` (language spec \xC2\xA7"
        "27.6) is not yet supported on `OwningBuffer<T>`"));
}
