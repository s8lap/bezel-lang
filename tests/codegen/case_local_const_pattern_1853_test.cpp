// §19.3 / §17.14 (v0.226, #1853): a LOCAL `const` used as a `case` pattern
// constant (`case n of K` where `K` is a method-local `const`) resolves to K's
// compile-time value, exactly like a literal or a const MEMBER.
//
// Before the fix, codegen's `constValueForPatternName` searched only
// `ConstMemberSymbol`s (const MEMBERS in `symbols()` and type members); a
// const LOCAL was not in that index, so the bare name `K` resolved to nullptr.
// Before #1847's guard the arm chain was then silently truncated (NO arm fired,
// including the mandatory `_` — a silent miscompile: the issue's repro compiled
// clean and printed NOTHING); after #1847 it became a clean `(§19.3)` backend
// diagnostic. Either way the pattern did not work.
//
// THE FIX: sema's `checkTopLevelNamedPattern` (ExprTypecheck.cpp) records the
// scope-correct `LocalSymbol` on the pattern via `setPatternSymbol` (a Pattern
// is not an Expr, so `exprSymbol` cannot key it; and locals are absent from
// `symbols()`, so a name search could not recover one — nor honour shadowing).
// Codegen's `constValueForPatternName` reads that symbol and folds the local
// const's initializer. §17.14 restricts the form to a COMPILE-TIME-classified
// local const; a RUNTIME const (`const K := n + 1;`, §21.1) is not a constant
// expression and is rejected as a pattern constant at sema (test below).

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
// §19.3: a compile-time error was reported (sema rejects an ill-formed pattern
// constant before codegen). `diagnostics.hasErrors()` is authoritative.
bool compileErrored(const std::string& src) {
    return runAot(src).diagnostics.hasErrors();
}
}  // namespace

// §19.3 / §17.14 (#1853): the issue's repro — a compile-time local int const
// matches its value. FAILS-WITHOUT the fix: the #1847 guard reports the pattern
// as unsupported (and pre-#1847 silently truncated the arm chain → `_` skipped).
BEZEL_TEST(case_local_const_pattern_1853, local_int_const_match) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    const K: int := 7; "
        "    var n := 7; "
        "    return (case n of  K => 0, _ => 1); "  // 0 — K matches n
        "  end Main; "
        "end C;"), 0);
}

// §19.3: a non-matching scrutinee falls through to the wildcard (the const arm
// is a true equality test, not an always-match — the pre-fix silent-truncation
// defect skipped even the `_` arm; this guards against an over-fix that fires
// the const arm unconditionally).
BEZEL_TEST(case_local_const_pattern_1853, local_int_const_nomatch) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    const K: int := 7; "
        "    var n := 8; "
        "    return (case n of  K => 0, _ => 1); "  // 1 — n != K, wildcard
        "  end Main; "
        "end C;"), 1);
}

// §19.3: a local const pattern in STATEMENT position — the matching arm's body
// runs (the pre-fix statement-position truncation skipped the body AND the `_`).
BEZEL_TEST(case_local_const_pattern_1853, local_int_const_stmt_match) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    const K: int := 42; "
        "    var n := 42; "
        "    var r := 9; "
        "    case n of  K => r := 0;, _ => r := 1; end; "
        "    return r; "  // 0
        "  end Main; "
        "end C;"), 0);
}

// §19.3 / §17.14: a compile-time local STRING const — content comparison
// (mirrors the const-MEMBER string form). A string-literal initializer is a
// §17.14 constant expression, so the local const is compile-time-classified.
BEZEL_TEST(case_local_const_pattern_1853, local_string_const_match) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    const S: string := \"hi\"; "
        "    var x := \"hi\"; "
        "    return (case x of  S => 5, _ => 6); "  // 5
        "  end Main; "
        "end C;"), 5);
}

BEZEL_TEST(case_local_const_pattern_1853, local_string_const_nomatch) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    const S: string := \"hi\"; "
        "    var x := \"bye\"; "
        "    return (case x of  S => 5, _ => 6); "  // 6
        "  end Main; "
        "end C;"), 6);
}

// §19.3: a `,`-alternative (`|`) mixing a local const and a literal — the const
// arm matches either member (binding-free alternative members, §19.3).
BEZEL_TEST(case_local_const_pattern_1853, local_const_alt_with_literal) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    const K: int := 42; "
        "    var n := 99; "
        "    return (case n of  K | 99 => 0, _ => 1); "  // 0 — 99 matches
        "  end Main; "
        "end C;"), 0);
}

// §19.3: a local const pattern whose const is declared in an ENCLOSING scope
// (not the immediate case block) still resolves — the recorded symbol is the
// scope-correct one, not a flat name lookup.
BEZEL_TEST(case_local_const_pattern_1853, local_const_from_outer_scope) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    const K: int := 3; "
        "    var n := 3; "
        "    var r := 1; "
        "    if n = 3 then "
        "      case n of  K => r := 7;, _ => r := 2; end; "
        "    end; "
        "    return r; "  // 7 — K (outer-scope const) matches
        "  end Main; "
        "end C;"), 7);
}

// §17.14 / §19.3 (#1853): a RUNTIME const (`const K := n + 1;` where n is a
// non-const var — §21.1) is NOT a compile-time constant expression, so it may
// not be a `case` pattern constant. Sema rejects it up front (before #1853 the
// bare `isConst` local was accepted unconditionally, then miscompiled / hit the
// #1847 backend guard). A clean §17.14/§19.3 error is the spec-faithful outcome.
BEZEL_TEST(case_local_const_pattern_1853, runtime_const_rejected) {
    BEZEL_EXPECT_EQ(compileErrored(
        "module M; class C "
        "  public static method Main(): int "
        "    var n: int := 7; "
        "    const K := n + 1; "          // runtime const (n is non-const)
        "    return (case n of  K => 0, _ => 1); "
        "  end Main; "
        "end C;"), true);
}

// Regression guard (#1847's value-reject is preserved): a bare identifier
// naming a runtime VALUE (a `var` local) is NOT a const and stays rejected.
BEZEL_TEST(case_local_const_pattern_1853, var_local_still_rejected) {
    BEZEL_EXPECT_EQ(compileErrored(
        "module M; class C "
        "  public static method Main(): int "
        "    var k: int := 7; "
        "    var n := 7; "
        "    return (case n of  k => 0, _ => 1); "  // k is a var, not a const
        "  end Main; "
        "end C;"), true);
}

// Regression guard: a const MEMBER (`public const K`) used as a pattern still
// resolves (the local-const path must not divert or shadow the member path).
BEZEL_TEST(case_local_const_pattern_1853, member_const_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public const K: int := 11; "
        "  public static method Main(): int "
        "    var n := 11; "
        "    return (case n of  K => 22, _ => 0); "  // 22 — member const
        "  end Main; "
        "end C;"), 22);
}
