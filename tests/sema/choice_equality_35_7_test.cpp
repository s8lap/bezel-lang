// Spec-derived sema tests for §35.7 (v0.200, #2045): choice equality,
// hashing, and the use-site compile error on `=`/`<>`/Equals/GetHashCode
// for non-value-like choices.
//
// §35.7: "A choice that is NOT auto-hashable — any borrow-bearing
// or owning choice (class-typed payloads are outside the admissible-
// component rule, exactly as class-typed record fields are), and any
// value-like choice with an inadmissible payload — draws §9.2's use-site
// compile error on `=`, `<>`, and direct `Equals`/`GetHashCode` calls,
// unless the author declares the operators under §25.2's co-declaration
// rule."
//
// These tests exercise both halves of §35.7: the classification-driven rule
// (a non-value-like choice rejects `=`/`<>` and direct Equals/GetHashCode),
// and the recursive per-payload admissibility rule for value-like choices
// (§35.7's "auto-hashable when every payload field is §9.2-admissible"
// — a value-like choice with an inadmissible payload, e.g. a function-typed
// payload, is NOT auto-hashable and draws the same use-site error;
// #2110, v0.226). The use-site `=`/`<>` check routes through the same
// isAutoEqualityBearingChoice predicate that drives isEqualityBearing, the
// §14.2 `where T: hashable` constraint, and checkHashContainerKeys.

#include <string>
#include <string_view>

#include "ast/Node.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCollect;

namespace {

// fix #3985: delegates to the shared gated helper (sema_support.hpp)
// instead of duplicating the parse-error-gate logic locally.
bezel::test::SemaResult semaAll(std::string_view src) {
    return bezel::test::parseAndTypecheckGated(src);
}

bool hasErrDiagWith(const bezel::test::SemaResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

bool lacksErrDiagWith(const bezel::test::SemaResult& r,
                      std::string_view needle) {
    return !hasErrDiagWith(r, needle);
}

}  // namespace

// --- §35.7 ACCEPT: value-like choice allows `=`/`<>` (no §35.7 error) ------

BEZEL_TEST(choice_equality_35_7,
           spec_35_7_value_like_choice_equality_no_35_7_error) {
    // §35.7 first paragraph: a value-like choice with admissible payloads
    // is auto-hashable. The §35.7 use-site error does NOT fire
    // for `=` over a value-like choice.
    auto r = semaAll(
        "module M; choice Opt Some(value: int), None; end Opt; "
        "class C public static method Main(): int "
        "  var a: Opt := Opt.None; "
        "  var b: Opt := Opt.None; "
        "  if a = b then return 1; else return 0; end; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(lacksErrDiagWith(r, "is not available on choice"));
}

// --- §35.7 REJECT: borrow-bearing choice rejects `=`/`<>` -----------------

BEZEL_TEST(choice_equality_35_7,
           spec_35_7_borrow_bearing_choice_rejects_equality_op) {
    // §35.7: a borrow-bearing choice draws §9.2's use-site compile error
    // on `=`. The choice has a plain class payload, which makes it borrow-
    // bearing per §35.4.2.
    auto r = semaAll(
        "module M; class C end C; "
        "choice Box Some(value: C), None; end Box; "
        "class D public static method Main(): int "
        "  var a: Box := Box.None; "
        "  var b: Box := Box.None; "
        "  if a = b then return 1; else return 0; end; "
        "end Main; end D;");
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "is not available on choice"));
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "borrow-bearing"));
}

BEZEL_TEST(choice_equality_35_7,
           spec_35_7_borrow_bearing_choice_rejects_not_equal_op) {
    // §35.7: the rule covers both `=` and `<>`.
    auto r = semaAll(
        "module M; class C end C; "
        "choice Box Some(value: C), None; end Box; "
        "class D public static method Main(): int "
        "  var a: Box := Box.None; "
        "  var b: Box := Box.None; "
        "  if a <> b then return 1; else return 0; end; "
        "end Main; end D;");
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "is not available on choice"));
}

// --- §35.7 REJECT: owning choice rejects `=`/`<>` -------------------------

BEZEL_TEST(choice_equality_35_7,
           spec_35_7_owning_choice_rejects_equality_op) {
    // §35.7: an owning choice (an `owned` marker on a class payload per
    // §35.4.1) is move-only — value equality is unavailable.
    auto r = semaAll(
        "module M; class R end R; "
        "choice FetchOutcome Fetched(owned response: R), NotModified; "
        "end FetchOutcome; "
        "class D public static method Main(): int "
        "  var a: FetchOutcome := FetchOutcome.NotModified; "
        "  var b: FetchOutcome := FetchOutcome.NotModified; "
        "  if a = b then return 1; else return 0; end; "
        "end Main; end D;");
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "is not available on choice"));
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "owning"));
}

// --- §35.7 REJECT: value-like choice with an inadmissible payload (#2110) ----
// The recursive half of §35.7: a value-like choice whose case payload fails
// the §9.2 admissible-component rule is NOT auto-hashable, and `=`/
// `<>` draw the same use-site error as a non-value-like choice. The function
// type `() -> int` is classification-neutral per §35.4 (the choice stays
// value-like), yet is outside the §9.2 admissible-component list, so it is
// rejected by the recursive rule. v0.226, #2110.

BEZEL_TEST(choice_equality_35_7,
           spec_35_7_valuelike_inadmissible_payload_rejects_equality_op_2110) {
    // §35.7 / §3.6 (v0.226, #2110): `=` on a value-like choice with a
    // function-typed payload rejects — the payload is not a §9.2 admissible
    // equality component.
    auto r = semaAll(
        "module M; "
        "choice Ev On(f: () -> int), Off; end Ev; "
        "class C public static method Main(): int "
        "  var a: Ev := Ev.Off; "
        "  var b: Ev := Ev.Off; "
        "  if a = b then return 1; else return 0; end; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "is not available on choice"));
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "admissible equality component"));
}

BEZEL_TEST(choice_equality_35_7,
           spec_35_7_valuelike_inadmissible_payload_rejects_not_equal_op_2110) {
    // §35.7 (v0.226, #2110): the recursive per-payload rule covers `<>` too.
    auto r = semaAll(
        "module M; "
        "choice Ev On(f: () -> int), Off; end Ev; "
        "class C public static method Main(): int "
        "  var a: Ev := Ev.Off; "
        "  var b: Ev := Ev.Off; "
        "  if a <> b then return 1; else return 0; end; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "is not available on choice"));
}

// --- §35.7 ACCEPT (contrast): the same shape with an admissible payload ------

BEZEL_TEST(choice_equality_35_7,
           spec_35_7_valuelike_admissible_payload_still_allows_equality_2110) {
    // §35.7 (v0.226, #2110): the recursive rule rejects ONLY the inadmissible-
    // payload case — a value-like choice whose payload IS a §9.2 admissible
    // component (int) remains auto-hashable, and `=` stays allowed.
    // The §35.7 use-site error does NOT fire.
    auto r = semaAll(
        "module M; "
        "choice Ev On(v: int), Off; end Ev; "
        "class C public static method Main(): int "
        "  var a: Ev := Ev.Off; "
        "  var b: Ev := Ev.Off; "
        "  if a = b then return 1; else return 0; end; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(lacksErrDiagWith(r, "is not available on choice"));
}

// --- §35.7 REJECT: direct .Equals() / .GetHashCode() on non-value-like -----

BEZEL_TEST(choice_equality_35_7,
           spec_35_7_owning_choice_rejects_direct_equals_call) {
    // §35.7: "direct `Equals`/`GetHashCode` calls" also draw the use-site
    // error on non-value-like choices. Mirrors §9.2's record rule.
    auto r = semaAll(
        "module M; class R end R; "
        "choice FetchOutcome Fetched(owned response: R), NotModified; "
        "end FetchOutcome; "
        "class D public static method Main(): int "
        "  var a: FetchOutcome := FetchOutcome.NotModified; "
        "  return (if a.Equals(a) then 1 else 0); "
        "end Main; end D;");
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "is not available on choice"));
}

BEZEL_TEST(choice_equality_35_7,
           spec_35_7_borrow_bearing_choice_rejects_gethashcode_call) {
    // §35.7: `.GetHashCode()` on a borrow-bearing choice rejects.
    auto r = semaAll(
        "module M; class C end C; "
        "choice Box Some(value: C), None; end Box; "
        "class D public static method Main(): int "
        "  var a: Box := Box.None; "
        "  return a.GetHashCode(); "
        "end Main; end D;");
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "is not available on choice"));
}

// --- §35.7 REJECT: direct .Equals()/.GetHashCode() on value-like, inadmissible
// payload (#2110) -----------------------------------------------------------
// The recursive half of §35.7 on the DIRECT-CALL path: a value-like choice
// whose case payload fails the §9.2 admissible-component rule is NOT auto-
// hashable, so direct `.Equals()`/`.GetHashCode()` draw the same use-
// site error as on a non-value-like choice. The function type `() -> int` is
// classification-neutral per §35.4 (the choice stays value-like), yet outside
// the §9.2 admissible-component list. v0.226, #2110.

BEZEL_TEST(choice_equality_35_7,
           spec_35_7_valuelike_inadmissible_payload_rejects_direct_equals_call_2110) {
    // §35.7 / §3.6 (v0.226, #2110): direct `.Equals()` on a value-like choice
    // with a function-typed payload rejects — the payload is not a §9.2
    // admissible equality component. Mirrors the `=` use-site test for the
    // direct-call path the spec names alongside `=`/`<>`.
    auto r = semaAll(
        "module M; "
        "choice Ev On(f: () -> int), Off; end Ev; "
        "class C public static method Main(): int "
        "  var a: Ev := Ev.Off; "
        "  return (if a.Equals(a) then 1 else 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "is not available on choice"));
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "admissible equality component"));
}

BEZEL_TEST(choice_equality_35_7,
           spec_35_7_valuelike_inadmissible_payload_rejects_direct_gethashcode_call_2110) {
    // §35.7 (v0.226, #2110): the recursive per-payload rule covers direct
    // `.GetHashCode()` too — a value-like choice with a function-typed
    // payload is not hashable, so hashing it is a use-site compile error.
    auto r = semaAll(
        "module M; "
        "choice Ev On(f: () -> int), Off; end Ev; "
        "class C public static method Main(): int "
        "  var a: Ev := Ev.Off; "
        "  return a.GetHashCode(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "is not available on choice"));
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "admissible equality component"));
}

// --- §35.7 ACCEPT: value-like choice allows .Equals() / .GetHashCode() ----

BEZEL_TEST(choice_equality_35_7,
           spec_35_7_value_like_choice_allows_direct_equals_call) {
    // §35.7: a value-like choice with admissible payloads is auto-
    // hashable — direct Equals/GetHashCode are allowed. The
    // §35.7 use-site error does NOT fire.
    auto r = semaAll(
        "module M; choice Opt Some(value: int), None; end Opt; "
        "class C public static method Main(): int "
        "  var a: Opt := Opt.None; "
        "  return (if a.Equals(a) then 1 else 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(lacksErrDiagWith(r, "is not available on choice"));
}
