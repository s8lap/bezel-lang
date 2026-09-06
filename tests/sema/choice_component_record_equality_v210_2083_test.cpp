// Spec-derived sema tests for §9.2 / §9.3 / §35.7 (v0.210, #2083): a value-
// like auto-hashable choice component makes the enclosing record
// auto-hashable. Mirrors the §9.2 admissible-component-type widening
// that v0.210 formalized for choices in record / record positional fields.
//
// §9.2 (v0.210): the admissible-component-type list now includes
// "a value-like choice that is itself auto-hashable (§35.7 —
// recursively, this same list applied to every case's payload fields)".
// Before v0.210 the list was the §9.2 v0.158 set (primitives / enums / string
// / record / T? / inline-array), and a choice in a component position was
// always rejected with the §9.2 use-site compile error on `=`/`<>`/Equals/
// GetHashCode. The fix widens isAdmissibleComponentType to admit such a
// choice; isEqualityBearing and the synthesized Equals/GetHashCode dispatch
// follow.

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

// --- §9.2 v0.210 ACCEPT: body record with a value-like choice component -----

// §9.2: a body-only record whose auto-property is a value-like auto-equality-
// bearing choice component is auto-hashable. `=` over the record is
// accepted at the use site (no §9.2 compile error). The participating rule
// covers both payload-free cases (e.g. Red, Blue) and payload cases (e.g.
// Green(value: int)).
BEZEL_TEST(choice_component_record_equality_v210_2083,
           spec_9_2_v210_body_record_value_like_choice_component_accepts_eq) {
    auto r = semaAll(
        "module M; "
        "choice Color Red, Green(value: int), Blue; end Color; "
        "record Wrap "
        "  public property C: Color; "
        "  public constructor(c: Color) C := c; end constructor; "
        "end Wrap; "
        "class C public static method Main(): int "
        "  var w1 := new Wrap(Color.Red); "
        "  var w2 := new Wrap(Color.Red); "
        "  if w1 = w2 then return 1; else return 0; end; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasErrDiagWith(r, "is not auto-hashable"));
    BEZEL_EXPECT_FALSE(hasErrDiagWith(r, "is not available on record"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.3 / §9.2 (combined form): a POSITIONAL record whose positional field is
// a value-like auto-hashable choice is auto-hashable (§9.3
// "eligibility of the combined form" → §9.2 admissibility rule, now widened
// to choices in v0.210).
BEZEL_TEST(choice_component_record_equality_v210_2083,
           spec_9_3_v210_positional_record_value_like_choice_component_accepts_eq) {
    auto r = semaAll(
        "module M; "
        "choice Color Red, Green(value: int), Blue; end Color; "
        "record Wrap(C: Color); "
        "class C public static method Main(): int "
        "  var w1 := new Wrap(Color.Green(7)); "
        "  var w2 := new Wrap(Color.Green(7)); "
        "  if w1 = w2 then return 1; else return 0; end; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasErrDiagWith(r, "is not auto-hashable"));
    BEZEL_EXPECT_FALSE(hasErrDiagWith(r, "is not available on record"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §35.7: a body record with a value-like choice component may also call
// `.Equals(other)` / `.GetHashCode()` directly — the §9.2 use-site compile
// error does NOT fire for an auto-hashable enclosing record.
BEZEL_TEST(choice_component_record_equality_v210_2083,
           spec_9_2_v210_body_record_value_like_choice_component_allows_direct_equals) {
    auto r = semaAll(
        "module M; "
        "choice Color Red, Green(value: int); end Color; "
        "record Wrap "
        "  public property C: Color; "
        "  public constructor(c: Color) C := c; end constructor; "
        "end Wrap; "
        "class C public static method Main(): int "
        "  var w := new Wrap(Color.Green(42)); "
        "  if w.Equals(w) then return 1; else return 0; end; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasErrDiagWith(r, "is not auto-hashable"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- §9.2 v0.210: nested case — choice-in-choice admissible component ------

// §9.2 v0.210 (per-issue plan, recursive admissibility): a body record whose
// auto-property is a value-like choice WHOSE cases include another auto-
// hashable choice payload — the §35.7 "this same list applied to every
// case's payload fields" recursion admits the inner choice. Body record is
// auto-hashable.
BEZEL_TEST(choice_component_record_equality_v210_2083,
           spec_9_2_v210_body_record_value_like_nested_choice_component_accepts_eq) {
    auto r = semaAll(
        "module M; "
        "choice Inner Valued(value: int), Unit; end Inner; "
        "choice Outer First(inner: Inner), Second; end Outer; "
        "record Wrap "
        "  public property O: Outer; "
        "  public constructor(o: Outer) O := o; end constructor; "
        "end Wrap; "
        "class C public static method Main(): int "
        "  var w := new Wrap(Outer.First(Inner.Valued(7))); "
        "  return (if w.O = Outer.First(Inner.Valued(7)) then 1 else 0); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasErrDiagWith(r, "is not auto-hashable"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- §9.2 v0.210 REJECT: enclosing record does NOT inherit equality -------
//
// A BORROW-BEARING choice component (class-typed payload, §35.4.2) keeps the
// enclosing record non-auto-hashable per the §9.2 admissible-component
// rule: borrow-bearing choices are NOT admissible (class-typed payload is
// reference, outside the rule — the same token reject that already applies to
// a class-typed record field).
BEZEL_TEST(choice_component_record_equality_v210_2083,
           spec_9_2_v210_body_record_borrow_bearing_choice_component_rejects_eq) {
    auto r = semaAll(
        "module M; "
        "class C end C; "
        "choice Box Some(value: C), None; end Box; "
        "record Wrap "
        "  public property B: Box; "
        "  public constructor(b: Box) B := b; end constructor; "
        "end Wrap; "
        "class D public static method Main(): int "
        "  var w := new Wrap(Box.None); "
        "  return (if w = w then 1 else 0); "
        "end Main; end D;");
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "is not auto-hashable"));
}

// §9.2: an OWNING choice component (an `owned` marker on a class payload
// per §35.4.1) keeps the enclosing record non-auto-hashable. The
// choice is not admissible in any record component context; the use-site
// error fires on `=`/`<>` for the record.
BEZEL_TEST(choice_component_record_equality_v210_2083,
           spec_9_2_v210_body_record_owning_choice_component_rejects_eq) {
    auto r = semaAll(
        "module M; "
        "class R end R; "
        "choice Fetched Fetched(owned r: R), NotFetched; end Fetched; "
        "record Wrap "
        "  public property O: Fetched; "
        "  public constructor(o: Fetched) O := o; end constructor; "
        "end Wrap; "
        "class D public static method Main(): int "
        "  var w := new Wrap(Fetched.NotFetched); "
        "  return (if w = w then 1 else 0); "
        "end Main; end D;");
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "is not auto-hashable"));
}
