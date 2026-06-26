// Codegen — a value-type (primitive) nullable proved non-null by null-flow
// narrowing reads as its non-nullable value in assignment / store / return
// contexts (issue #914; spec §7.4 / §12.7).
//
// After `if n is null then return 0; end;` (or `is not null`, or the else of
// `is null`), a nullable `int?`/`bool?`/… variable is known non-null and is
// assignable to a non-nullable target. Sema accepts the coercion WITHOUT
// rewriting the declared nullable layout (so null-comparisons / `is` tests on
// the variable still see the tagged-optional struct — the soundness boundary);
// codegen unwraps the {flag, value} slot at the non-nullable-target coercion
// sites (local-init, assignment store, return).
//
// Covered here: var-init, pre-declared store, return, and a narrowed PARAMETER,
// for int? and bool?. NOT covered (a separate lifted-arith / enum path — must
// still cleanly REJECT, never miscompile): `n + 1` and `enum?`.

#include <string>

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

// Run and return the exit code, or -999 if it did not compile/run.
int run(const std::string& body) {
    std::string src =
        "module M; class C public static method Main(): int " + body +
        " end Main; end C;";
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

// True if `src` fails to compile (a clean reject — used for the controls).
bool rejected(const std::string& body) {
    std::string src =
        "module M; class C public static method Main(): int " + body +
        " end Main; end C;";
    auto r = runJit(src);
    return !r.ran;
}

}  // namespace

// var-init after a guard-clause `is null` early return.
BEZEL_TEST(null_narrow_value_914, var_init_guard_clause) {
    BEZEL_EXPECT_EQ(run(
        "var n: int? := 100; if n is null then return 0; end; "
        "var v: int := n; return v;"), 100);
}

// var-init inside an `is not null` block.
BEZEL_TEST(null_narrow_value_914, var_init_is_not_null_block) {
    BEZEL_EXPECT_EQ(run(
        "var n: int? := 42; "
        "if n is not null then var v: int := n; return v; end; return 0;"), 42);
}

// return a narrowed value-nullable directly.
BEZEL_TEST(null_narrow_value_914, return_narrowed) {
    BEZEL_EXPECT_EQ(run(
        "var n: int? := 42; if n is null then return 0; end; return n;"), 42);
}

// store into a pre-declared non-nullable local.
BEZEL_TEST(null_narrow_value_914, store_pre_declared) {
    BEZEL_EXPECT_EQ(run(
        "var n: int? := 77; var v: int := 0; "
        "if n is not null then v := n; end; return v;"), 77);
}

// bool? narrows the same way.
BEZEL_TEST(null_narrow_value_914, bool_nullable_narrowed) {
    BEZEL_EXPECT_EQ(run(
        "var b: bool? := true; "
        "if b is not null then var v: bool := b; return (1 if v else 0); end; "
        "return 9;"), 1);
}

// else-of-`is null` narrows.
BEZEL_TEST(null_narrow_value_914, else_of_is_null) {
    BEZEL_EXPECT_EQ(run(
        "var n: int? := 55; "
        "if n is null then return 0; else var v: int := n; return v; end;"), 55);
}

// a narrowed PARAMETER (params are borrows; the narrowing + unwrap apply too).
BEZEL_TEST(null_narrow_value_914, narrowed_parameter) {
    auto r = runJit(
        "module M; class C "
        "  public static method F(n: int?): int "
        "    if n is null then return 0; end; var v: int := n; return v; "
        "  end F; "
        "  public static method Main(): int return F(55); end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 55);
}

// CONTROL: a genuinely-nullable value (not narrowed) → non-nullable target must
// still be REJECTED.
BEZEL_TEST(null_narrow_value_914, genuinely_nullable_still_rejected) {
    BEZEL_EXPECT_TRUE(rejected("var n: int? := null; var v: int := n; return v;"));
}

// CONTROL (soundness boundary): a null-comparison / `is` test on the narrowed
// variable still works — the declared nullable layout is preserved, so this
// neither crashes nor mis-evaluates. n is non-null here, so `n = null` is false.
BEZEL_TEST(null_narrow_value_914, null_comparison_on_narrowed_var_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "var n: int? := 5; "
        "if n is not null then if n = null then return 1; end; return 2; end; "
        "return 0;"), 2);
}

// CONTROL: reassigning null inside the narrowed block re-widens the variable
// (the narrowing is dropped for the `:=`), so `n := null` type-checks.
BEZEL_TEST(null_narrow_value_914, reassign_null_clears_narrowing) {
    BEZEL_EXPECT_EQ(run(
        "var n: int? := 5; "
        "if n is not null then n := null; "
        "  if n is null then return 7; end; return 1; end; return 0;"), 7);
}

// §7.11 / §7.4 (#914): ARITHMETIC on a narrowed value-nullable is NOT lifted —
// both payloads are definitely present, so `n + 1` is `int`, not `int?`, and is
// assignable to / returnable as a non-nullable target.
BEZEL_TEST(null_narrow_value_914, arithmetic_narrowed_operand) {
    BEZEL_EXPECT_EQ(run(
        "var n: int? := 100; "
        "if n is not null then var x: int := n + 1; return x; end; return 0;"),
        101);
}
BEZEL_TEST(null_narrow_value_914, arithmetic_two_narrowed_operands) {
    BEZEL_EXPECT_EQ(run(
        "var n: int? := 10; var m: int? := 5; "
        "if n is not null then if m is not null then return n + m; end; end; "
        "return 0;"), 15);
}
BEZEL_TEST(null_narrow_value_914, arithmetic_return_narrowed) {
    BEZEL_EXPECT_EQ(run(
        "var n: int? := 21; if n is null then return 0; end; return n * 2;"), 42);
}

// CONTROL: a narrowed operand combined with a GENUINELY-nullable operand keeps
// the lifted `T?` result — assigning that to a non-nullable target rejects.
BEZEL_TEST(null_narrow_value_914, arithmetic_mixed_with_nullable_stays_lifted) {
    BEZEL_EXPECT_TRUE(rejected(
        "var n: int? := 100; var k: int? := null; "
        "if n is not null then var x: int := n + k; return x; end; return 0;"));
}

// CONTROL: a narrowed arithmetic result is still assignable to a NULLABLE
// target (stays/​re-wraps as T?), unaffected.
BEZEL_TEST(null_narrow_value_914, arithmetic_narrowed_to_nullable_target) {
    BEZEL_EXPECT_EQ(run(
        "var n: int? := 100; "
        "if n is not null then var y: int? := n + 1; "
        "  return (y! if y is not null else 0); end; return 0;"), 101);
}
