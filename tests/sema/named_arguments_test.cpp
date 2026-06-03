// Spec-derived tests for §22.5 — optional parameters and named arguments.
//
// §22.5: "Named arguments may appear in any order after positional
// arguments." A named argument `name := expr` binds to the parameter of
// that name; positional arguments fill parameters in order. Every required
// (non-default) parameter must be supplied positionally or by name; an
// unbound optional parameter takes its default. A named argument that names
// no parameter, a parameter bound twice, and a positional argument after a
// named one are each errors. After binding, per-argument assignability
// (§5.1) and overload resolution (§22.6) proceed on the bound positions.

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

// §22.5 example: `Log(level := "ERROR", message := "Failed")` — all
// parameters bound by name, in an order different from the declaration.
BEZEL_TEST(sema_named_22_5, bind_by_name_all_named_reordered_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F(a: int, b: int) end F; "
        "  public method G() this.F(b := 2, a := 1); end G; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.5: positional arguments fill in order, a named argument binds the
// rest — `F(1, c := 3, b := 2)`-style mix of positional then named.
BEZEL_TEST(sema_named_22_5, positional_then_named_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F(a: int, b: int, c: int) end F; "
        "  public method G() this.F(1, c := 3, b := 2); end G; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.5: a named argument that names no parameter is an error.
BEZEL_TEST(sema_named_22_5, unknown_parameter_name_diagnosed) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F(a: int, b: int) end F; "
        "  public method G() this.F(a := 1, q := 2); end G; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §22.5: a parameter may be bound at most once — here `a` is filled both
// positionally and again by name.
BEZEL_TEST(sema_named_22_5, duplicate_binding_positional_then_named_diagnosed) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F(a: int, b: int) end F; "
        "  public method G() this.F(1, a := 2); end G; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §22.5: a parameter bound twice by name is an error.
BEZEL_TEST(sema_named_22_5, duplicate_binding_two_named_diagnosed) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F(a: int, b: int) end F; "
        "  public method G() this.F(a := 1, a := 2); end G; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §22.5: "Named arguments may appear ... after positional arguments" — so a
// positional argument that follows a named one is rejected.
BEZEL_TEST(sema_named_22_5, positional_after_named_diagnosed) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F(a: int, b: int) end F; "
        "  public method G() this.F(a := 1, 2); end G; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §22.5: every required (non-default) parameter must be supplied; binding
// only `a` leaves required `b` unfilled.
BEZEL_TEST(sema_named_22_5, missing_required_parameter_diagnosed) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F(a: int, b: int) end F; "
        "  public method G() this.F(a := 1); end G; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §22.5 example `Log("System started", timestamp := false)` — a named
// argument fills one optional parameter while another optional parameter
// (and any positional) takes its default. All required supplied → ok.
BEZEL_TEST(sema_named_22_5, named_fills_optional_others_default_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F(a: int, b: int := 5, c: int := 9) end F; "
        "  public method G() this.F(1, c := 7); end G; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.5 + §5.1: after binding, each bound argument is checked for
// assignability against the parameter it fills — here a `bool` named
// argument is bound to an `int` parameter, which §5.1 does not permit.
BEZEL_TEST(sema_named_22_5, bound_argument_type_mismatch_diagnosed) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F(a: int, b: int) end F; "
        "  public method G() this.F(b := true, a := 1); end G; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §22.5 + §5.1: a reordered named-arg call that is type-correct in the
// parameter it actually fills must NOT be flagged — the int literal binds
// to the int parameter `a` regardless of source order.
BEZEL_TEST(sema_named_22_5, reordered_named_args_type_correct_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F(a: int, b: long) end F; "
        "  public method G() this.F(b := 2L, a := 1); end G; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.5 + §22.6: a named-argument call participates in overload selection.
// `F(value := n)` where n is int binds the `value` parameter on both
// overloads; the int overload is the exact match (§22.6), the long overload
// requires a widening conversion — so the call is unambiguously resolved and
// no error is produced.
BEZEL_TEST(sema_named_22_5, named_arg_participates_in_overload_resolution_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F(value: int): int => value; "
        "  public method F(value: long): int => 0; "
        "  public method G(): int "
        "    var n: int := 3; "
        "    return this.F(value := n); "
        "  end G; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.5: an unknown parameter name in an overloaded call binds to no
// candidate, so resolution finds no applicable overload (the call is not
// silently accepted). At minimum it must surface a diagnostic for the bad
// name on the sole-arity candidate path.
BEZEL_TEST(sema_named_22_5, named_arg_overload_unknown_name_diagnosed) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F(value: int): int => value; "
        "  public method G(): int return this.F(nope := 3); end G; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §22.5 / §22.7: constructor named arguments bind and are checked. A
// reordered, type-correct `new T(...)` named-arg call is accepted.
BEZEL_TEST(sema_named_22_5, constructor_named_args_reordered_ok) {
    auto r = parseSemaAll(
        "module M; class P "
        "  public X: int := 0; public Y: int := 0; "
        "  public constructor(x: int, y: int) X := x; Y := y; end constructor; "
        "end P; "
        "class C public method G() var p := new P(y := 3, x := 10); end G; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.5 / §22.7: a constructor named-arg call that leaves a required
// parameter unfilled is diagnosed.
BEZEL_TEST(sema_named_22_5, constructor_named_args_missing_required_diagnosed) {
    auto r = parseSemaAll(
        "module M; class P "
        "  public X: int := 0; public Y: int := 0; "
        "  public constructor(x: int, y: int) X := x; Y := y; end constructor; "
        "end P; "
        "class C public method G() var p := new P(x := 10); end G; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §22.5 / §22.7: a constructor named-arg call naming a non-existent
// parameter is diagnosed.
BEZEL_TEST(sema_named_22_5, constructor_named_args_unknown_name_diagnosed) {
    auto r = parseSemaAll(
        "module M; class P "
        "  public X: int := 0; public Y: int := 0; "
        "  public constructor(x: int, y: int) X := x; Y := y; end constructor; "
        "end P; "
        "class C public method G() var p := new P(x := 10, z := 3); end G; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
