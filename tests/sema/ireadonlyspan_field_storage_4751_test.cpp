// Spec-derived regression test for issue #4751 — §4.16 + §9.7
// "IReadOnlySpan<TElement>" interface parameter stored in a class instance
// field. v0.307.
//
// §9.7's instance-field storage bullet (v0.307, the umbrella rule):
//
//   "A borrow-bearing record value may not be stored in an **instance field of
//    a class** … A class instance's lifetime is not bounded by any borrow
//    source's scope, and a record-carried borrow — unlike a **direct**
//    class-typed borrow field … — may be consumed through access paths with
//    **no per-access disposed-flag check** (a span's buffer window is the
//    canonical case). The checked-mode trust regime is therefore unavailable
//    and the store is **rejected statically**."
//
// stdlib §4.16 (v0.307), the stdlib-specific restatement of that rule on the
// `IReadOnlySpan<TElement>` interface:
//
//   "A method declaring `IReadOnlySpan<TElement>` cannot store the parameter
//    in a field that outlives the call, return it from a method whose return-
//    lifetime rules do not establish source lifetime, or capture it in a
//    closure that escapes. These are the standard borrow-bearing-record
//    constraints; nothing new is added by the interface."
//
// The pre-fix compiler extended the umbrella rule through three predicates
// (`isBorrowBearingRecord`, `isBorrowBearingRecordOrSpan` builtin-span,
// `isBorrowBearingChoice`), none of which classify the INTERFACE type
// `IReadOnlySpan<TElement>` — the predicate name is by declaration, and the
// interface is one. So a method `Remember(s: IReadOnlySpan<int>) { _s := s; }`
// over `class Holder { private _s: IReadOnlySpan<int>?; … }` compiled clean
// (rc=0, no diagnostic) and the field would carry a borrow-bearing record at
// runtime — the canonical escape §9.7 rejects.
//
// Measured pre-fix (r19-c24 = afee220e):
//   * method body twin (issue repro, row 1): silent accept, exit 0
//   * the byte-identical RHS typed `ArraySpan<int>` (the existing rule's
//     canonical case, control row 2): rejected with the §9.7 span wording
// Post-fix the §9.7 type rule fires for `IReadOnlySpan<TElement>` too, and
// both rows reject with their respective diagnostics.
//
// The fix sits inside `MethodOwnership::checkBorrowStoreEscape`, alongside
// the existing record / builtin-span / choice legs (§9.7's umbrella rule is a
// TYPE rule and consults no borrow SOURCE, so the extension is local to that
// leg — the lambda-body widening #3701 took care of reaches it for free).
//
// // per §9.7 + §4.16, v0.307 (issue #4751)

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// §4.16 / §9.7 interface-parameter field-store diagnostic — the
// interface-specific wording. Distinct from the existing record/span/choice
// strings (`a borrow-bearing record may not be stored…`, `a span may not be
// stored…`, `a borrow-bearing choice may not be stored…`) so a row that pins
// the interface rule does not match an unrelated rejection on a different
// carrier. The message names the parameter carrier (the spec's voice) and
// the §9.7 type-rule authority.
constexpr const char* kInterfaceAssign =
    "an IReadOnlySpan<TElement> parameter value may not be stored in an "
    "instance field of a class";

}  // namespace

// =============================================================================
// ROW 1 — REJECT. The issue's repro verbatim: an IReadOnlySpan<int>
// parameter stored in a class instance field. The field is declared
// `IReadOnlySpan<int>?` (the §9.7 null-source DECLARATION form, which §9.7
// admits), but the parameter's runtime value is a borrow-bearing record
// (a span), and the store of that parameter into the field is the §4.16 /
// §9.7 violation. Pre-fix: silent accept (rc=0, no §9.7 diagnostic).
// Post-fix: §4.16 / §9.7 rejection.
// =============================================================================
BEZEL_TEST(ireadonlyspan_field_storage_4751,
           ireadonlyspan_parameter_into_field_rejected_4_16_9_7) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Holder "
        "  private _s: IReadOnlySpan<int>?; "
        "  public constructor() end constructor; "
        "  public method Remember(s: IReadOnlySpan<int>) "
        "    _s := s; "
        "  end Remember; "
        "end Holder; "
        "class C public static method Main(): int return 0; end Main; end C; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kInterfaceAssign));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "§4.16"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "§9.7"));
}

// =============================================================================
// ROW 2 — REJECT (CONTROL — CONCRETE SPAN). The byte-identical RHS spelled
// with the concrete span type `ArraySpan<int>` (a borrow-bearing RECORD per
// §9.7's own definition; the predicate's canonical case). Pre-existing
// umbrella rule must keep rejecting this — the new interface leg is an
// ADDITION, not a replacement. Diagnostic carries the existing span wording
// so a row that pins the interface rule does NOT match this case.
// =============================================================================
BEZEL_TEST(ireadonlyspan_field_storage_4751,
           array_span_parameter_into_field_still_rejected_9_7) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Holder "
        "  private _s: ArraySpan<int>?; "
        "  public constructor() end constructor; "
        "  public method Remember(s: ArraySpan<int>) "
        "    _s := s; "
        "  end Remember; "
        "end Holder; "
        "class C public static method Main(): int return 0; end Main; end C; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "a span may not be stored"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "§9.7"));
}

// =============================================================================
// ROW 3 — ACCEPT (CONTROL — LOCAL). The interface-parameter RHS stored into a
// LOCAL is not §4.16 / §9.7's destination ("an instance field of a class") —
// it is the §28.3 lifetime rule's domain, where a same-scope parameter
// outlives the local's binding. The new interface leg fires on
// `storeTargetIsClassField`, which is false for a local target, so the
// rule declines and the program compiles clean.
// =============================================================================
BEZEL_TEST(ireadonlyspan_field_storage_4751,
           ireadonlyspan_parameter_to_local_accepted_4_16) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Sum(s: IReadOnlySpan<int>): int "
        "    var copy := s; "
        "    return copy.Length; "
        "  end Sum; "
        "end C; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// ROW 4 — ACCEPT (CONTROL — METHOD-PARAMETER-PASSING). The §4.16 boxing path:
// an IReadOnlySpan<TElement> parameter passed as the argument to another
// IReadOnlySpan<TElement>-typed parameter. This is the canonical use case
// for the interface and must keep working post-fix. The new interface leg's
// field-target gate prevents it from reaching this shape.
// =============================================================================
BEZEL_TEST(ireadonlyspan_field_storage_4751,
           ireadonlyspan_parameter_passthrough_accepted_4_16) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Len(s: IReadOnlySpan<int>): int "
        "    return Inner(s); "
        "  end Len; "
        "  public static method Inner(s: IReadOnlySpan<int>): int "
        "    return s.Length; "
        "  end Inner; "
        "end C; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}