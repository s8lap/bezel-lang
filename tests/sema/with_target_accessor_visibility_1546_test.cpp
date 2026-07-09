// Spec-derived tests for #1546 (v0.180 conformance batch): §9.5
// non-destructive-update `with` — accessor-level visibility is irrelevant
// to a `with` target.
//
// docs/Bezel_Language_Spec.md §9.5 (v0.180):
//   "A `with` target must be accessible at the use site under the member's
//   own declared accessibility (§13) ... Because `with` copies storage
//   directly and never invokes setters (below), accessor-level visibility
//   is irrelevant — a `private set` does not restrict `with`, by the same
//   logic that lets read-only positional-field properties, which have no
//   setter at all, be targets."
//
// #934 (tests/sema/with_visibility_934_test.cpp) already covers the
// member-level §13.1 accessibility gate (private member rejected from
// outside, accepted from inside; public member always accepted). This file
// adds the accessor-level nuance #934 didn't exercise: a `public` property
// declared with a more restrictive `private set` is still a valid `with`
// target from OUTSIDE the declaring type, because `with` bypasses the
// setter entirely.
//
// Tests:
//   ACCEPT: public property with `private set` targeted by `with` from an
//           external class (accessor visibility does not gate `with`)
//   ACCEPT: same, from within the declaring type (control)

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

} // namespace

// §9.5 (v0.180) — ACCEPT: `with` targets a `public` auto-property whose
// setter accessor is `private`. The property itself is public (§13.1 is
// satisfied at the member level); `with` never calls the setter, so the
// setter's own restriction does not apply.
BEZEL_TEST(with_target_accessor_visibility_1546,
           public_property_private_set_with_from_external_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "record R "
        "  public property X: int private set; "
        "  public readonly property Y: int; "
        "  public constructor(x: int, y: int) X := x; Y := y; end constructor; "
        "end R; "
        "class C public static method Main(): int "
        "  var r := new R(1, 2); "
        "  var r2 := r with X := 42 end; "  // property is public — must be ACCEPTED
        "  return r2.X; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.5 (v0.180) — ACCEPT (control): the same target from inside the
// declaring type, where both the property and the setter would be visible
// regardless.
BEZEL_TEST(with_target_accessor_visibility_1546,
           public_property_private_set_with_from_declaring_type_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "record R "
        "  public property X: int private set; "
        "  public readonly property Y: int; "
        "  public constructor(x: int, y: int) X := x; Y := y; end constructor; "
        "  public method WithX(newX: int): R "
        "    return this with X := newX end; "
        "  end WithX; "
        "end R; "
        "class C public static method Main(): int "
        "  var r := new R(1, 2); "
        "  var r2 := r.WithX(42); "
        "  return r2.X; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
