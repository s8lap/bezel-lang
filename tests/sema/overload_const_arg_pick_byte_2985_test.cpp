// Spec-derived regression tests for issue #2985 — §22.6 overload resolution
// did not delegate to the §5.1 constant-narrowing rule, so an integer literal
// or named-const argument never reached an OVERLOADED callee by the worked
// example the spec names.
//
// #2985 (sema, MED) was SHIPPED at 6088e02 (Wave 3 Lane 4 cherry-pick) and
// is on HEAD (575f8cc). The sibling qualified-const fold arm of the same
// defect family landed at 33bd22d (Wave 5 Lane 3, #2986) and is verified by
// qualified_const_int_fold_2986_test.cpp — both files together pin §5.1
// constant narrowing end-to-end (fold → overload dispatch → unique winner).
// Verified 2026-08-18 by Wave 4 Lane E.
//
// §5.1 (v0.240, *Constant narrowing*), verbatim:
//   "An expression of integer type that is a **compile-time constant**
//    ([§17.14](#sec-constant-expressions)) implicitly converts to any integer
//    type whose range contains its value: `var b: byte := 50;`,
//    `Set(level: byte)` called as `Set(3)`, and enum constants of the
//    underlying type ([§10.2](#sec-explicit-values)) are all well-typed by
//    this rule. A constant whose value does not fit the target's range is a
//    **compile error** at the conversion site — never a silent truncation.
//    The rule applies to constants only; a run-time `int` value still
//    requires the explicit `byte(x)` narrowing of [§5.2](#sec-explicit-
//    conversions). Collection-literal element typing ([§15.4](#sec-
//    collection-literals)) and default-argument typing use this same rule."
//
// §22.6 (*Overloading*) classifies each (candidate, arg) cell by whether the
// argument's static type is *exact* or *convert* (a §5.1 conversion) for the
// parameter's type. The argument's static type is set by an EARLIER
// `typecheckExpression` pass — a literal `3` is `int`, a named `const` is its
// declared `int`. Without the fix, `Pick(3)` against `Pick(byte)` /
// `Pick(string)` sees `int → byte` as a NARROWING (not in §5.1's widening
// table) and rejects every candidate; `Pick(3)` against `Pick(byte)` /
// `Pick(int)` resolves only by accident when the `int` candidate is declared
// first.
//
// The fix delegates to narrowIntConstantToTarget (#2925) before classifyArg,
// so the resolver sees the post-narrowing type: `Pick(3)` against `Pick(byte)`
// becomes `byte → byte` (exact), against `Pick(string)` stays inapplicable,
// so the unique winner is `Pick(byte)` exactly as §5.1's worked example
// promises. The same delegation is added to the constructor-overload path in
// checkNewExprArgs.
//
// REGRESSION-TEST POLICY (regression-test-pins-the-bug): every PASS case below
// was REJECTED before the fix (with "no overload … is applicable to these
// arguments" / "the argument … is not implicitly convertible"). Every REJECT
// case below passes both before and after the fix by design — those tests
// exist to prove the fix did not widen acceptance past the §5.1 rule (an
// out-of-range constant against a narrower overload MUST still be rejected).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// AOT runner: compiles a program to native, runs it, returns its exit code.
// Returns -999 if the compile/link/run pipeline failed (so an "exit 0" test
// will not pass by accident when the compiler itself rejected the program).
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Sema-only runner: parse + collect + resolve types + typecheck expressions,
// returning the diagnostics. Used by the REJECT half of the file (the fix
// lifts an acceptance, so the load-bearing half is the rejection guard).
bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasMsg(const bezel::test::SemaResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// PASS — the defect #2985 reported. Each of these was REJECTED before the
// fix with "no overload of `Pick` is applicable to these arguments".
// ---------------------------------------------------------------------------

// §5.1 / §22.6 (#2985): the issue's exact shape. An int literal `3` is
// narrowed to `byte` against `Pick(byte)`; the `Pick(string)` overload stays
// inapplicable. `Pick(byte)` is the unique winner — verified end-to-end
// beyond type-check: the program compiles, links, and returns 42.
BEZEL_TEST(sema_overload_const_arg_2985,
           literal_picks_byte_over_string_spec_5_1_22_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class C "
        "  public static method Pick(x: byte): byte => 42; "
        "  public static method Pick(x: string): string => \"x\"; "
        "  public static method Main(): int "
        "    var b: byte := C.Pick(3); "
        "    if b = 42 then return 0; else return 1; end; "
        "  end Main; "
        "end C;"),
        0);
}

// §5.1 / §22.6 (#2985): the SYMMETRIC direction — `Pick("hi")` picks
// `Pick(string)`. Before the fix this happened to compile (string has no
// narrowing path), so this is a NO-OP control: the fix must not regress it.
// Verified by returning 7 from the string branch.
BEZEL_TEST(sema_overload_const_arg_2985,
           string_literal_picks_string_branch_spec_5_1_22_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class C "
        "  public static method Pick(x: byte): byte => 42; "
        "  public static method Pick(x: string): string => \"x\"; "
        "  public static method Main(): int "
        "    var s: string := C.Pick(\"hi\"); "
        "    if s = \"x\" then return 7; else return 1; end; "
        "  end Main; "
        "end C;"),
        7);
}

// §5.1 / §22.6 (#2985): a named `const` local typed `int` carries the same
// narrowing into the overload resolver as a literal — `foldIntConstExprTC`
// resolves the local const and the value (3) fits `byte`. Tests the
// `lookupName`-threaded resolver path inside narrowIntConstantToTarget, which
// is the non-trivial difference between this case and the literal case above.
BEZEL_TEST(sema_overload_const_arg_2985,
           named_const_picks_byte_over_string_spec_5_1_22_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class C "
        "  public static method Pick(x: byte): byte => 11; "
        "  public static method Pick(x: string): string => \"x\"; "
        "  public static method Main(): int "
        "    const k: int := 3; "
        "    var b: byte := C.Pick(k); "
        "    if b = 11 then return 0; else return 1; end; "
        "  end Main; "
        "end C;"),
        0);
}

// §5.1 / §22.6: the `Pick(3)` shape against `Pick(byte)` / `Pick(int)`, with
// `Pick(byte)` declared FIRST. #2985 pinned `Pick(byte)` here (11), because
// the per-candidate loop narrowed 3 to byte and graded that cell Exact.
//
// fix #6722 (§22.6 *The argument's static type*, v0.340) FLIPPED it: the
// literal is read at its static type `int`, and §5.1's constant narrowing
// "enters resolution as an *applicability* rule only: a candidate the
// narrowing reaches *converts* at that position, never exact". "`M(3)`
// against `M(int)` beside `M(byte)` selects `M(int)`" — 99, in either order.
// (The test name is the #2985 one, kept so the flip is traceable.)
BEZEL_TEST(sema_overload_const_arg_2985,
           byte_first_int_second_picks_byte_spec_5_1_22_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class C "
        "  public static method Pick(x: byte): byte => 11; "
        "  public static method Pick(x: int): int => 99; "
        "  public static method Main(): int "
        "    var n: int := C.Pick(3); "
        "    if n = 99 then return 0; else return 1; end; "
        "  end Main; "
        "end C;"),
        0);
}

// §5.1 / §22.6: the SYMMETRIC test — `Pick(int)` declared first, then
// `Pick(byte)`. The call is NOT ambiguous (the test name is #2985's, kept so
// history stays traceable): per §22.6 v0.340 (fix #6722) the literal `3` is
// read at its static type `int`, so `Pick(int)` is the exact match and
// `Pick(byte)`, reached only by §5.1's constant narrowing, converts. "`M(3)`
// against `M(int)` beside `M(byte)` selects `M(int)`".
BEZEL_TEST(sema_overload_const_arg_2985,
           int_first_byte_second_is_ambiguous_spec_5_1_22_6) {
    auto r = parseSemaAll(
        "module M; static class C "
        "  public static method Pick(x: int): int => 99; "
        "  public static method Pick(x: byte): byte => 11; "
        "  public static method Main(): int "
        "    var n: int := C.Pick(3); "
        "    return n; "
        "  end Main; "
        "end C;");
    // Accepted: `Pick(int)` is exact (#6722 pins the selected value).
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §5.1 / §22.6 (#2985): constructor overload delegation. `new T(3)` against
// `constructor(x: byte)` / `constructor(x: string)` — the constructor-overload
// path in checkNewExprArgs also delegates to narrowIntConstantToTarget after
// the fix. Verified end-to-end by `b = 42`.
BEZEL_TEST(sema_overload_const_arg_2985,
           new_expr_picks_byte_ctor_over_string_ctor_spec_5_1_22_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class T "
        "  public _v: byte; "
        "  public constructor(x: byte) _v := x; end constructor; "
        "  public constructor(x: string) _v := 0; end constructor; "
        "end T; "
        "static class C "
        "  public static method Main(): int "
        "    var t := new T(3); "
        "    if t._v = 3 then return 0; else return 1; end; "
        "  end Main; "
        "end C;"),
        0);
}

// §5.1 / §22.6 (#2985): the NO-OP control — a non-overloaded call to a
// single-byte overload was already well-typed before the fix (checkAssign-
// able → narrowIntConstantToTarget handles it). This test pins that the fix
// did not regress the single-overload path: `Set(3)` against only `Set(byte)`.
BEZEL_TEST(sema_overload_const_arg_2985,
           single_byte_overload_unaffected_spec_5_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class C "
        "  public static method Set(x: byte): byte => x; "
        "  public static method Main(): int "
        "    var b: byte := C.Set(3); "
        "    if b = 3 then return 0; else return 1; end; "
        "  end Main; "
        "end C;"),
        0);
}

// §5.1 / §22.6 (#2985): the NO-OP control — a non-overloaded call to a
// single-long overload from an `int` literal widening (NOT narrowing; `int`
// already widens to `long` per §5.1's table). Before AND after the fix this
// is Exact-or-Convert via the ordinary widening rule; the resolver path was
// untouched.
BEZEL_TEST(sema_overload_const_arg_2985,
           single_long_overload_unaffected_spec_5_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class C "
        "  public static method Set(x: long): long => x; "
        "  public static method Main(): int "
        "    var n: long := C.Set(42); "
        "    if n = 42 then return 0; else return 1; end; "
        "  end Main; "
        "end C;"),
        0);
}

// ---------------------------------------------------------------------------
// REJECT — the load-bearing half. The fix LIFTS a rejection, so it must not
// lift any of these. Each passed before the fix and must still pass after it.
// ---------------------------------------------------------------------------

// §5.1 / §22.6: an out-of-range constant against the ONLY narrower-int
// candidate is rejected — `Pick(byte)` cannot absorb 300 (byte is 0..255).
// The narrowing's `fitsInIntType` guard fails; narrowIntConstantToTarget is a
// no-op; classifyArg sees `int → byte` as NotApplicable; the call is rejected
// with the §22.6 "no overload is applicable" diagnostic.
BEZEL_TEST(sema_overload_const_arg_2985,
           out_of_range_constant_still_rejected_spec_5_1_22_6) {
    auto r = parseSemaAll(
        "module M; static class C "
        "  public static method Pick(x: byte): byte => 42; "
        "  public static method Pick(x: string): string => \"x\"; "
        "  public static method Main(): int "
        "    var b: byte := C.Pick(300); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §5.1 / §22.6: a run-time `int` variable against `Pick(byte)` / `Pick(int)`
// does NOT narrow — only a compile-time constant does. `int` is a runtime
// variable; narrowIntConstantToTarget returns `from` unchanged because
// foldIntConstExprTC returns nullopt for a non-constant; classifyArg sees
// `int → byte` as NotApplicable; the `int` overload wins by being the only
// applicable candidate. (Asserting only the Win path — `r.diagnostics` are
// clean.)
BEZEL_TEST(sema_overload_const_arg_2985,
           runtime_variable_does_not_narrow_spec_5_1_22_6) {
    auto r = parseSemaAll(
        "module M; static class C "
        "  public static method Pick(x: byte): byte => 42; "
        "  public static method Pick(x: int): int => 99; "
        "  public static method Main(): int "
        "    var n: int := 3; "
        "    var k: int := C.Pick(n); "
        "    return k; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §5.1 / §22.6: an out-of-range constant against a narrower-int AND an
// integer overload still produces the §22.6 "ambiguous" diagnostic if both
// would be exact (which, with the constant's value 300, is the case for
// `short` (yes) and `int` (yes)). Wait — that test would not be ambiguous
// because `int` is the broader one. Better: two narrower candidates where
// the constant fits NEITHER after narrowing — e.g. `300` against `byte` and
// `sbyte` (300 doesn't fit either), and no other candidate exists. Then the
// call is rejected with the unreachable diagnostic.
BEZEL_TEST(sema_overload_const_arg_2985,
           constant_too_big_for_every_narrower_int_still_rejected_spec_5_1) {
    auto r = parseSemaAll(
        "module M; static class C "
        "  public static method Pick(x: byte): byte => 42; "
        "  public static method Pick(x: sbyte): sbyte => 0; "
        "  public static method Main(): int "
        "    var b: byte := C.Pick(300); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "no overload"));
}

// §5.1 / §22.6: a ref argument against an overloaded callee keeps the
// exact-type rule — `ref` aliases the caller's storage so no §5.1 conversion
// is admitted. A `ref` to a runtime-int variable against `ref x: byte` /
// `ref x: int` resolves to the `int` overload; a `ref` to a const int that
// could narrow is NOT narrowed (the fix's modifier guard skips it). This
// test pins that the modifier-skip did not regress the exact-match path.
BEZEL_TEST(sema_overload_const_arg_2985,
           ref_argument_skips_narrowing_spec_22_3_22_6) {
    auto r = parseSemaAll(
        "module M; static class C "
        "  public static method Pick(ref x: byte): int => 42; "
        "  public static method Pick(ref x: int): int => 99; "
        "  public static method Main(): int "
        "    var k: int := 3; "
        "    return C.Pick(ref k); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
