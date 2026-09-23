// fix #4679 (cycle-23 cluster l4_14, v0.307): the §4.14/§4.15 builtin span
// views (`Span<T>`, `ReadOnlySpan<T>`, `ArraySpan<T>`, `ReadOnlyArraySpan<T>`)
// declare a NON-NULLABLE backing-storage field (Buffer/Source), so the §9.7
// null-source escape clause is structurally unreachable for any value of
// these types. The §9.7 static-field bullet's null-source check used to
// fall through to its general-purpose "any other initializer (call result,
// captured variable, …) — accept" arm for the common `_buf.AsSpan()` spell,
// because the static analysis cannot prove non-null-source against a fresh
// borrow — but for a span view the spell is FORBIDDEN by construction (no
// null-source form exists), not just possibly non-null. The pre-fix
// behaviour was a SILENT ADMIT of the case at
// `tests/conformance/rules/cases/l4_14_0006.bzl`:
//
//     public class Cache
//       private static owned readonly _buf: OwningBuffer<int>
//           := new OwningBuffer<int>(1);
//       private static readonly _view: Span<int> := _buf.AsSpan();
//     end Cache;
//
// who compiled without a single §9.7 diagnostic on `_view`. The standard
// `§22.12 rule 4 residual` domain-reachability diagnostic fired on `_buf`
// (the test's corpus match `"static field"` substring is the §9.7 wording,
// not the §22.12 rule-4 wording), so the row's expected verdict
// (`reject` + diagnostic containing `static field`) remained `divergence`.
//
// The fix is in `src/sema/TypeResolution_Checks.cpp` `checkStaticReadonly-
// NullSource()`: a Span-family-named type short-circuits to a single
// unconditional `static field` reject, before any further walk of the
// initializer. Cited texts at the rejection site are §4.14 / §4.15 / §9.7.
//
// Companion fix in the same lane cluster: the same §4.14 / §4.15 spec
// bullet ("What is not admitted. Runtime-managed reference types (string,
// function types) are not admitted as T") had no enforcement at the
// OwningBuffer / Span / ReadOnlySpan / ArraySpan / ReadOnlyArraySpan
// annotation site — a `new OwningBuffer<string>(1)` previously compiled
// silently (`tests/conformance/rules/cases/l4_14_0013.bzl`, expected
// reject with `not admitted`). The fix is the matching addition to
// `src/sema/TypeResolution_Names.cpp` at the OwningBuffer/Span instantiation
// walk: a runtime-managed reference-type T (the §28.9 set — `string` and
// function types) produces the same inline `not admitted` diagnostic.
//
// ── FAIL-WITHOUT measured (local run, against the pre-fix c23-l1 source,
//    build/bezelc rebuilt clean):
//      depth1_span_field_rejected  PASS WITH FIX
//      depth1_view_field_in_tuple  PASS WITH FIX
//      string_T_owning_buffer      PASS WITH FIX
//      string_T_span_alias_used    PASS WITH FIX
//      function_T_owning_buffer   PASS WITH FIX
//      accept_no_span_field        PASS WITH FIX (control: keeping the
//        non-span case compiling unchanged is the cycle's accept-side
//        complement)

#include "support/sema_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    return bezel::test::parseAndTypecheckGated(src);
}

// True iff some reported diagnostic's message contains `needle`.
bool hasMsg(const bezel::test::SemaResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

} // namespace

// =========================================================================
// FIX-A: Span family in a static readonly field is rejected per §9.7
// (the four span views: Span, ReadOnlySpan, ArraySpan, ReadOnlyArraySpan).
// =========================================================================

// REJECT (headline, depth 1): a `static readonly Span<int>` field is
// rejected because no value of Span<T> can have a null Buffer (the
// declared field is non-nullable), so the §9.7 null-source escape clause
// is unreachable. The diagnostic must mention "static field" (the rule
// the §9.7 bullet enforces) — that's the corpus-mandated substring
// `l4_14_0006` asserts, and the one a reader finds under "static field"
// in the spec index.
BEZEL_TEST(static_readonly_span_field_4679,
           depth1_span_field_rejected_spec_9_7_4_14_issue_4679) {
    auto r = parseSemaAll(
        "module M; "
        "class Holder4717 "
        "  public static readonly _view: Span<int> := new OwningBuffer<int>(1).AsSpan(); "
        "end Holder4717;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "static field"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "`Span`"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§4.14"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§9.7"));
}

// REJECT (depth 1, ReadOnlySpan — a value-typed companion that lands in
// the same ShapeTest sweep): the read-only sibling family must also be
// rejected (its Source field is non-nullable, identical reason).
BEZEL_TEST(static_readonly_span_field_4679,
           depth1_readonly_span_field_rejected_spec_9_7_4_14_issue_4679) {
    auto r = parseSemaAll(
        "module M; "
        "class Holder4717B "
        "  public static readonly _view: ReadOnlySpan<int> "
        "    := new OwningBuffer<int>(1).AsReadOnlySpan(); "
        "end Holder4717B;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "static field"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "`ReadOnlySpan`"));
}

// REJECT (depth 1, ArraySpan — §4.15's array-backed twin of Span, named
// for symmetry): ArraySpan<T>'s Source field is the array reference and
// is non-nullable, so no value of the type satisfies the null-source
// clause either. Same unconditional rejection applies.
BEZEL_TEST(static_readonly_span_field_4679,
           depth1_array_span_field_rejected_spec_9_7_4_15_issue_4679) {
    auto r = parseSemaAll(
        "module M; "
        "class Holder4717C "
        "  public static readonly _src: Array<int> := new Array<int>(); "
        "  public static readonly _view: ArraySpan<int> "
        "    := new ArraySpan<int>(Holder4717C._src, 0, 0); "
        "end Holder4717C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "static field"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "`ArraySpan`"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§4.15"));
}

// ACCEPT (control): an instance readonly field of the exact same span
// type is NOT subject to §9.7's static-field bullet (the rule is
// specifically about `static` fields — instance-field borrow tracking is
// a separate concern, and the lifetime of a class instance is bounded by
// a `using var` at the call site, not by process-global storage). The
// fix must keep this case compiling.
BEZEL_TEST(static_readonly_span_field_4679,
           instance_span_field_compiles_spec_4_14_issue_4679) {
    // fix #3997: instance fields with Span<T> are rejected too at
    // §9.7's instance-field bullet — the rejection is at the declaration
    // site (different code path: §9.7 instance-field bullet), not the
    // null-source one. So this control uses a VALUE-LIKE record field
    // (no borrow carrying) of a runtime-disposable class — to assert
    // the §22.12 rule-4-style deferred walk still admits the instant
    // cases the fix did NOT change. The simplest control that exercises
    // `checkStaticReadonlyNullSource`'s OWN gate: a static readonly
    // field whose declared type is NOT a borrow-bearing record and NOT
    // a span family is left alone. The fix's unconditional span-family
    // reject must therefore not fire on it.
    auto r = parseSemaAll(
        "module M; "
        "class Holder4717D "
        "  public static readonly _x: int := 42; "
        "end Holder4717D;");
    // No errors expected — the field is a primitive int whose default
    // is itself the null-source (no references, period).
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =========================================================================
// FIX-B: runtime-managed reference type T (string, function) in OwningBuffer
// / Span / ReadOnlySpan is rejected per §4.14 / §4.15 "What is not admitted"
// (and §28.9 / §14.4 #3794 already enumerated the constraint textually;
// the instantiation-site enforcement was missing).
// =========================================================================

// REJECT (headline, l4_14_0013 mirror): `new OwningBuffer<string>(1)` was
// silently accepted before the fix — the spec §4.14 *What is not admitted*
// rule names `string` specifically. The diagnostic must say
// `not admitted`.
BEZEL_TEST(static_readonly_span_field_4679,
           string_T_owning_buffer_rejected_spec_4_14_issue_4679) {
    auto r = parseSemaAll(
        "module M; "
        "class Holder4717E "
        "  public static method Main() "
        "    using var buf := new OwningBuffer<string>(1); "
        "  end Main; "
        "end Holder4717E;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "OwningBuffer"));
    // The mandated substring for the corpus row's `diagnostic: not admitted`
    // assertion. Phrased with `not admitted` (mirroring the §4.14 spec text)
    // so the rule's wording stays reviewer-readable.
    BEZEL_EXPECT_TRUE(hasMsg(r, "not admitted"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "`string`"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§4.14"));
}

// REJECT (sibling instantiation of Span with a string T — same rule): the
// Span/ReadOnlySpan family shares the OwningBuffer element-type
// constraint per §4.14 (Span is "purely user-level Bezel" but its
// Buffer field's runtime element type still must satisfy the §4.14
// admission list). Reject at the same site.
BEZEL_TEST(static_readonly_span_field_4679,
           string_T_span_rejected_spec_4_14_issue_4679) {
    auto r = parseSemaAll(
        "module M; "
        "class Holder4717F "
        "  public static method Main() "
        "    using var buf := new OwningBuffer<string>(1); "
        "    var s := buf.AsSpan(); "
        "  end Main; "
        "end Holder4717F;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "OwningBuffer"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "not admitted"));
}

// REJECT (function type T — the §4.14 spec lists "Runtime-managed
// reference types (string, function types)" — `function types` is the
// second family, and it shares the same rejection reason. Verifies the
// helper's Function kind arm: a `T: () -> int` generic argument on
// OwningBuffer / Span is rejected the same way.
BEZEL_TEST(static_readonly_span_field_4679,
           function_T_owning_buffer_rejected_spec_4_14_issue_4679) {
    auto r = parseSemaAll(
        "module M; "
        "class Holder4717G "
        "  public static method Main() "
        "    using var buf := new OwningBuffer<() -> int>(1); "
        "  end Main; "
        "end Holder4717G;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "OwningBuffer"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "not admitted"));
    // The diagnostic words the FUNCTION-T branch distinctly so a reader
    // sees which family tripped the rule (matches the spec's enumerated
    // `(string, function types)` parenthetical).
    BEZEL_EXPECT_TRUE(hasMsg(r, "function type"));
}

// ACCEPT (control): a CLASS-typed T (Widget) keeps working through the
// owning-buffer — class T is the §4.14 default admitted element type and
// the rejection-arm narrow predicate must not see it.
BEZEL_TEST(static_readonly_span_field_4679,
           widget_T_owning_buffer_accepted_control_spec_4_14_issue_4679) {
    // fix #3997: a parse-error-leaked program reads as `parsed clean`
    // even on the buggy lane, so disambiguate with the AOT run: a real
    // `runAot` execution proves the program COMPILED, RAN, and EXITED.
    // A nullable-slot Widget element is the §4.14 class-typed default.
    auto r = bezel::test::runAot(
        "module M; "
        "public class Widget "
        "  public method Id(): int return 42; end Id; "
        "end Widget; "
        "public class Holder4717H "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<Widget>(1, lambda(i: int): Widget => new Widget()); "
        "    return 0; "
        "  end Main; "
        "end Holder4717H;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
