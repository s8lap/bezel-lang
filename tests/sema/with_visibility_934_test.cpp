// Spec-derived tests for fix #934: §13.1 member visibility on `with`
// expression targets.
//
// §13.1: "private is visible to the declaring class only".
// A `with` expression that targets a private member from outside the
// declaring type must be rejected — the same rule that governs ordinary
// member access via MemberAccess / NamedMember.
//
// Tests:
//   REJECT: private property targeted by `with` from an external class (#934)
//   REJECT: private positional-field targeted by `with` from an external class
//   ACCEPT: public property targeted by `with` — no over-gating
//   ACCEPT: private member targeted by `with` from within the declaring type

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

// Helper: does any diagnostic mention a §13.1 private-visibility error?
bool hasPrivateVisibilityError(const bezel::test::SemaResult& r,
                               std::string_view memberName) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("13.1") != std::string::npos &&
            d.message.find(memberName) != std::string::npos &&
            d.message.find("private") != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

// §13.1, #934 — REJECT: `with` targets a private auto-property from an
// external class. Before the fix, this was a BAD_ACCEPT (silently compiled).
BEZEL_TEST(with_visibility_934, private_property_with_from_external_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "record MyRec "
        "  private property Secret: int; "
        "  public constructor(s: int) Secret := s; end constructor; "
        "end MyRec; "
        "class C public static method Main(): int "
        "  var r := new MyRec(42); "
        "  var r2 := r with Secret := 99 end; "  // private — must be REJECTED
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasPrivateVisibilityError(r, "Secret"));
}

// §13.1, #934 — REJECT: `with` targets a private positional field from an
// external class. Positional fields are FieldSymbols; visibility still applies.
BEZEL_TEST(with_visibility_934, private_field_with_from_external_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "record Pair "
        "  private _first: int; "
        "  private _second: int; "
        "  public constructor(a: int, b: int) _first := a; _second := b; end constructor; "
        "end Pair; "
        "class C public static method Main(): int "
        "  var p := new Pair(1, 2); "
        "  var p2 := p with _first := 99 end; "  // private — must be REJECTED
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasPrivateVisibilityError(r, "_first"));
}

// §13.1, #934 — ACCEPT: `with` targets a PUBLIC property from an external
// class. Public members are always accessible (§13.1). No over-gating.
BEZEL_TEST(with_visibility_934, public_property_with_from_external_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "record R "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end R; "
        "class C public static method Main(): int "
        "  var r := new R(1); "
        "  var r2 := r with V := 99 end; "  // public — must be ACCEPTED
        "  return r2.V; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §13.1, #934 — ACCEPT: `with` targets a private member from INSIDE the
// declaring type. Private IS visible to the declaring type (§13.1).
BEZEL_TEST(with_visibility_934, private_property_with_from_declaring_type_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "record MyRec "
        "  private property Secret: int; "
        "  public constructor(s: int) Secret := s; end constructor; "
        "  public method WithSecret(x: int): MyRec "
        "    return this with Secret := x end; "  // inside MyRec — must be ACCEPTED
        "  end WithSecret; "
        "end MyRec; "
        "class C public static method Main(): int "
        "  var r := new MyRec(42); "
        "  var r2 := r.WithSecret(99); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
