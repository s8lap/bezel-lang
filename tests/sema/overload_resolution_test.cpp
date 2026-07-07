// Spec-derived tests for §22.6 "Implicit conversions and tie-breaking"
// (anchor sec-implicit-conversion-ranking). Each test mirrors one of the
// six worked examples in the spec, plus a few discriminating cases that
// would regress to the prior arity-only "first-wins" behavior.
//
// Selection tests use the return-type-binding discrimination technique:
// the two overloads return DISTINCT types, the spec-wrong overload is
// placed FIRST in source order (so a first-wins resolver would pick it),
// and the call result is bound to a typed local. If the wrong overload
// were chosen, the local-initializer assignability check (§5.1) would
// fire. So "no errors" attests that §22.6 selected the spec-correct one.

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

// §22.6 Example 1 — exact match preferred over a conversion.
// The wrong overload Log(long) is placed first; §22.6 must still pick the
// exact Log(int) for an `int` argument. Log(int) returns int, Log(long)
// returns bool; binding to `var x: int` would error if Log(long) won.
BEZEL_TEST(sema_overload_22_6, example1_exact_preferred_over_conversion) {
    auto r = parseSemaAll(
        "module M; class Logger "
        "  public method Log(level: long): bool => true; "
        "  public method Log(level: int): int => level; "
        "  public method Use(n: int) var x: int := this.Log(n); end Use; "
        "end Logger;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.6 Example 1 — an `int` literal is an exact match for Log(int).
BEZEL_TEST(sema_overload_22_6, example1_int_literal_exact) {
    auto r = parseSemaAll(
        "module M; class Logger "
        "  public method Log(level: long): bool => true; "
        "  public method Log(level: int): int => level; "
        "  public method Use() var x: int := this.Log(42); end Use; "
        "end Logger;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.6 Example 1 — a `long` literal (42L) is an exact match for Log(long).
BEZEL_TEST(sema_overload_22_6, example1_long_literal_exact) {
    auto r = parseSemaAll(
        "module M; class Logger "
        "  public method Log(level: int): int => level; "
        "  public method Log(level: long): bool => true; "
        "  public method Use() var x: bool := this.Log(42L); end Use; "
        "end Logger;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.6 Example 2 — two equally-ranked conversions are ambiguous.
// int -> long and int -> float are both §5.1 implicit and co-equal; the
// call to Convert(a) with `a: int` is a compile error.
BEZEL_TEST(sema_overload_22_6, example2_two_equal_conversions_ambiguous) {
    auto r = parseSemaAll(
        "module M; class Converter "
        "  public method Convert(n: long): int => 1; "
        "  public method Convert(n: float): int => 2; "
        "  public method Use(a: int) _ := this.Convert(a); end Use; "
        "end Converter;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §22.6 Example 2 — an explicit cast disambiguates (long(a) -> Convert(long)).
BEZEL_TEST(sema_overload_22_6, example2_explicit_cast_disambiguates) {
    auto r = parseSemaAll(
        "module M; class Converter "
        "  public method Convert(n: long): int => 1; "
        "  public method Convert(n: float): int => 2; "
        "  public method Use(a: int) _ := this.Convert(long(a)); end Use; "
        "end Converter;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.6 Example 3 — exact on one argument, conversion on the other.
// Apply(int,long) is placed first; Apply(int,int) is exact on both and is
// unambiguously better for (int, int). It returns int; Apply(int,long)
// returns bool; binding to `var x: int` would error if the wrong one won.
BEZEL_TEST(sema_overload_22_6, example3_exact_on_one_arg_dominates) {
    auto r = parseSemaAll(
        "module M; class Pair "
        "  public method Apply(x: int, y: long): bool => true; "
        "  public method Apply(x: int, y: int): int => x + y; "
        "  public method Use(a: int, b: int) "
        "    var r: int := this.Apply(a, b); end Use; "
        "end Pair;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.6 Example 4 — each candidate exact on a DIFFERENT argument: ambiguous.
// Compute(int,long) is exact on arg a, convert on arg b; Compute(long,int)
// is convert on a, exact on b. Neither dominates → compile error.
BEZEL_TEST(sema_overload_22_6, example4_each_exact_on_different_arg_ambiguous) {
    auto r = parseSemaAll(
        "module M; class Calculator "
        "  public method Compute(a: int, b: long): int => 1; "
        "  public method Compute(a: long, b: int): int => 2; "
        "  public method Use(x: int, y: int) "
        "    _ := this.Compute(x, y); end Use; "
        "end Calculator;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §22.6 Example 4 — casting one argument makes one candidate exact on both.
BEZEL_TEST(sema_overload_22_6, example4_cast_makes_one_exact_on_both) {
    auto r = parseSemaAll(
        "module M; class Calculator "
        "  public method Compute(a: int, b: long): int => 1; "
        "  public method Compute(a: long, b: int): int => 2; "
        "  public method Use(x: int, y: int) "
        "    _ := this.Compute(x, long(y)); end Use; "
        "end Calculator;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.6 Example 5 — no reachable candidate.
// float -> int is not implicit (float's only implicit target is double);
// float -> decimal is not implicit. No candidate is applicable → error.
BEZEL_TEST(sema_overload_22_6, example5_no_reachable_candidate) {
    auto r = parseSemaAll(
        "module M; class Transformer "
        "  public method Transform(x: int): int => x; "
        "  public method Transform(x: decimal): int => 0; "
        "  public method Use(n: float) _ := this.Transform(n); end Use; "
        "end Transformer;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §22.6 Example 6 — applicability filtering leaves one winner.
// byte -> sbyte is NOT implicit (signed/unsigned same-or-narrower width);
// byte -> ushort IS implicit. Process(ushort) is the only applicable
// candidate, so the call resolves with no ambiguity. Process(sbyte) is
// first in source; a first-wins resolver would (wrongly) pick it.
// Process(ushort) returns int, Process(sbyte) returns bool — binding to
// `var x: int` confirms the ushort overload was selected.
BEZEL_TEST(sema_overload_22_6, example6_applicability_leaves_one_winner) {
    auto r = parseSemaAll(
        "module M; class Handler "
        "  public method Process(n: sbyte): bool => true; "
        "  public method Process(n: ushort): int => 1; "
        "  public method Use(val: byte) var x: int := this.Process(val); "
        "  end Use; "
        "end Handler;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.6 — a subtype argument is an exact match against its own type and a
// (ranked-lower) conversion against a supertype parameter. F(Derived) is
// placed second; for a Derived argument it is exact while F(Base) converts,
// so F(Derived) is unambiguously better. It returns int; F(Base) returns
// bool — binding to `var x: int` confirms F(Derived) was chosen.
BEZEL_TEST(sema_overload_22_6, subtype_exact_preferred_over_supertype_convert) {
    auto r = parseSemaAll(
        "module M; class Base end Base; class Derived : Base end Derived; "
        "class C "
        "  public method F(b: Base): bool => true; "
        "  public method F(d: Derived): int => 1; "
        "  public method Use(d: Derived) var x: int := this.F(d); end Use; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.6 — both arguments exact in one candidate, exact-or-convert in the
// other: the all-exact candidate is unambiguously better. Sanity check
// that an all-exact call is never flagged ambiguous.
BEZEL_TEST(sema_overload_22_6, all_exact_call_not_ambiguous) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F(a: int, b: int): int => a; "
        "  public method F(a: int, b: long): int => 1; "
        "  public method Use(x: int, y: int) "
        "    _ := this.F(x, y); end Use; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.6 / §22.7 — the rule applies identically to constructor overload
// sets. Two same-arity ctors whose single int argument matches one exactly
// and the other (long) by conversion: `new T(5)` resolves to T(int)
// unambiguously (exact preferred over conversion), no error.
BEZEL_TEST(sema_overload_22_6, ctor_exact_preferred_over_conversion) {
    auto r = parseSemaAll(
        "module M; "
        "class T "
        "  public constructor(a: long) end constructor; "
        "  public constructor(a: int) end constructor; "
        "end T; "
        "class C public method M() _ := new T(5); end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.6 / §22.7 — ambiguous constructor call: int argument converts
// equally to long and to float. `new T(a)` with `a: int` is a compile
// error citing the candidate constructors.
BEZEL_TEST(sema_overload_22_6, ctor_two_equal_conversions_ambiguous) {
    auto r = parseSemaAll(
        "module M; "
        "class T "
        "  public constructor(a: long) end constructor; "
        "  public constructor(a: float) end constructor; "
        "end T; "
        "class C public method M(a: int) _ := new T(a); end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §22.6 / §22.7 — no reachable constructor: a float argument is not
// implicitly convertible to int or to decimal. `new T(n)` is an error.
BEZEL_TEST(sema_overload_22_6, ctor_no_reachable_candidate) {
    auto r = parseSemaAll(
        "module M; "
        "class T "
        "  public constructor(a: int) end constructor; "
        "  public constructor(a: decimal) end constructor; "
        "end T; "
        "class C public method M(n: float) _ := new T(n); end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §22.6 "Defaulted-arity tie-break" (v0.181, #1561) — a candidate applicable
// WITHOUT filling any default is preferred over one that fills one or more,
// provided it is better-or-equal at every supplied position. `Log(int, int
// := 0)` is placed FIRST (so a first-wins/arity fallback would pick it, and
// pre-fix the two were simply reported ambiguous) and returns bool;
// `Log(int)` is placed second and returns int. `this.Log(5)` binds to `var
// x: int`, which type-checks only if the no-defaults candidate — `Log(int)`
// per the spec's own `M(5)` example — was selected.
BEZEL_TEST(sema_overload_22_6, defaulted_arity_no_defaults_preferred) {
    auto r = parseSemaAll(
        "module M; class Foo "
        "  public method Log(n: int, m: int := 0): bool => true; "
        "  public method Log(n: int): int => n; "
        "  public method Use() var x: int := this.Log(5); end Use; "
        "end Foo;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.6 defaulted-arity tie-break — supplying the optional argument makes
// only the two-parameter form applicable (arity alone excludes `Log(int)`),
// so `Log(5, 7)` is unaffected by the tie-break and still resolves cleanly
// to the two-parameter overload.
BEZEL_TEST(sema_overload_22_6, defaulted_arity_full_arity_call_unaffected) {
    auto r = parseSemaAll(
        "module M; class Foo "
        "  public method Log(n: int): bool => true; "
        "  public method Log(n: int, m: int := 0): int => n + m; "
        "  public method Use() var x: int := this.Log(5, 7); end Use; "
        "end Foo;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.6 defaulted-arity tie-break — "two candidates that both fill defaults
// remain subject to the ambiguity rule (the compiler declines to guess
// which omission the caller meant)". `Log(a: int, b: int := 0)` and
// `Log(a: int, c: string := "x")` both bind only `a` for `Log(5)`, so both
// fill a default — the tie-break (which requires one side to fill NO
// defaults) does not apply, and the call stays ambiguous.
BEZEL_TEST(sema_overload_22_6, defaulted_arity_both_fill_defaults_still_ambiguous) {
    auto r = parseSemaAll(
        "module M; class Foo "
        "  public method Log(a: int, b: int := 0): int => a + b; "
        "  public method Log(a: int, c: string := \"x\"): int => a; "
        "  public method Use() _ := this.Log(5); end Use; "
        "end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §22.6 / §22.7 — the defaulted-arity tie-break applies identically to
// constructor overload sets. `new T(5)` resolves to the no-defaults
// `T(int)` constructor rather than being reported ambiguous against
// `T(int, int := 0)`.
BEZEL_TEST(sema_overload_22_6, ctor_defaulted_arity_no_defaults_preferred) {
    auto r = parseSemaAll(
        "module M; "
        "class T "
        "  public constructor(a: int, b: int := 0) end constructor; "
        "  public constructor(a: int) end constructor; "
        "end T; "
        "class C public method M() _ := new T(5); end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
