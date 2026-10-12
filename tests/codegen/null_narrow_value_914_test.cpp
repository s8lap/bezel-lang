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

#include "support/aot_result_asserts.hpp"   // fix #3895: failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {

// Run and return the exit code, or -999 if it did not compile/run.
int run(const std::string& body) {
    std::string src =
        "module M; class C public static method Main(): int " + body +
        " end Main; end C;";
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// True if `src` fails to compile with a genuine error diagnostic.
// A bare `!r.ran` would also pass for a crash (SIGSEGV etc.), which is not
// the same as a clean compile-time rejection — the diagnostic produced by a
// crash carries a real location and would satisfy a bare check alone.
bool rejected(const std::string& body) {
    std::string src =
        "module M; class C public static method Main(): int " + body +
        " end Main; end C;";
    auto r = runAot(src);
    return !r.ran && bezel::test::failedAtCompileTimeWithRealLocation(r);
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
        "if b is not null then var v: bool := b; return (if v then 1 else 0); end; "
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
    auto r = runAot(
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
        "  return (if y is not null then y! else 0); end; return 0;"), 101);
}

// §10 / §7.4 (#914): a narrowed `enum?` is a value-type nullable laid out as the
// same {flag, value} struct — it narrows to the non-nullable enum in assignment
// and return (Color.Green == 1, Color.Blue == 2).
BEZEL_TEST(null_narrow_value_914, enum_nullable_narrowed_assign) {
    auto r = runAot(
        "module M; enum Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var c: Color? := Color.Green; "
        "  if c is not null then var v: Color := c; return int(v); end; "
        "  return 9; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 1);
}
BEZEL_TEST(null_narrow_value_914, enum_nullable_narrowed_return) {
    auto r = runAot(
        "module M; enum Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var c: Color? := Color.Blue; if c is null then return 0; end; "
        "  return int(c); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 2);
}

// §9 / §7.4 (#914): a narrowed `record?` narrows to the non-nullable record.
BEZEL_TEST(null_narrow_value_914, record_nullable_narrowed_assign) {
    auto r = runAot(
        "module M; record Pt(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p: Pt? := new Pt(7, 3); "
        "  if p is not null then var q: Pt := p; return q.X; end; "
        "  return 9; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// CONTROL: a genuinely-nullable enum? → non-nullable enum still rejects.
BEZEL_TEST(null_narrow_value_914, enum_genuinely_nullable_rejected) {
    auto r = runAot(
        "module M; enum Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var c: Color? := null; var v: Color := c; return int(v); "
        "end Main; end C;");
    // fix #3895 (issue #3892): distinguishes a genuine rejection from a
    // runtime crash/signal, which a bare `!ran` cannot.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// §28 / §7.4 (#1056): a narrowed class/interface REFERENCE nullable assigns to a
// non-nullable reference (via `move`, since it transfers ownership). The
// narrowing peels the `move` wrapper; codegen stores the (non-null) pointer
// directly (unwrapNarrowedNullable is a no-op for a non-struct pointer).
BEZEL_TEST(null_narrow_value_914, reference_nullable_narrowed_move_assign) {
    auto r = runAot(
        "module M; class Box public V: int := 5; end Box; "
        "class C public static method Main(): int "
        "  var b: Box? := new Box(); "
        "  if b is not null then using var x: Box := move b; return x.V; end; "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// CONTROL: a genuinely-nullable class? `move`d to a non-nullable reference still
// rejects (the value is not narrowed).
BEZEL_TEST(null_narrow_value_914, reference_genuinely_nullable_rejected_1056) {
    auto r = runAot(
        "module M; class Box public V: int := 5; end Box; "
        "class C public static method Main(): int "
        "  var b: Box? := null; using var x: Box := move b; return x.V; "
        "end Main; end C;");
    // fix #3895 (issue #3892): distinguishes a genuine rejection from a
    // runtime crash/signal, which a bare `!ran` cannot.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// §7.4 / §27.6 (#1191): the narrowing acceptance must also apply in CALL-ARGUMENT
// position — a narrowed nullable local passed where the parameter is
// non-nullable. The call-arg compatibility check lost the narrowing context
// (checkAssignable was invoked without `fromExpr`), so the IDENTICAL local that
// var-init / return / assignment accept was wrongly REJECTED in argument
// position. Forwarding `fromExpr` restores the narrowing; codegen unwraps the
// {flag,value} tagged-optional to the parameter's bare value (the same coercion
// local-init / return apply via unwrapNarrowedNullable).

// A narrowed VALUE-type (int?) local as a non-nullable `int` argument. `n*2`=42.
BEZEL_TEST(null_narrow_value_914, narrowed_value_local_in_call_argument_1191) {
    auto r = runAot(
        "module M; class C "
        "  public static method Twice(n: int): int => n * 2; "
        "  public static method Main(): int "
        "    var n: int? := 21; "
        "    if n <> null then return Twice(n); end; return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// A narrowed REFERENCE (string?) local as a non-nullable `string` argument
// (unwrapNarrowedNullable is a no-op for the bare-pointer nullable string).
// "hi".Length = 2.
BEZEL_TEST(null_narrow_value_914, narrowed_reference_local_in_call_argument_1191) {
    auto r = runAot(
        "module M; class C "
        "  public static method Greet(u: string): int => u.Length; "
        "  public static method Main(): int "
        "    var u: string? := \"hi\"; "
        "    if u <> null then return Greet(u); end; return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 2);
}

// A narrowed local in a `new` CONSTRUCTOR argument (the new-ctor path shares the
// same checkArgsAgainstParams call-arg site). record Box(v).v.Length = 5.
BEZEL_TEST(null_narrow_value_914, narrowed_local_in_new_ctor_argument_1191) {
    auto r = runAot(
        "module M; record Box(v: string); "
        "class C public static method Main(): int "
        "  var u: string? := \"hello\"; "
        "  if u <> null then var b := new Box(u); return b.v.Length; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// CONTROL: a GENUINELY-nullable local (conditionally assigned, so NOT narrowed)
// passed to a non-nullable parameter must STILL be rejected.
BEZEL_TEST(null_narrow_value_914, genuinely_nullable_call_argument_rejected_1191) {
    auto r = runAot(
        "module M; class C "
        "  public static method Greet(u: string): int => u.Length; "
        "  public static method Pick(b: bool, s: string): int "
        "    var u: string? := null; if b then u := s; end; return Greet(u); "
        "  end Pick; "
        "  public static method Main(): int => Pick(true, \"hi\"); "
        "end C;");
    // fix #3895 (issue #3892): distinguishes a genuine rejection from a
    // runtime crash/signal, which a bare `!ran` cannot.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// CONTROL: a type-mismatched argument (int → string parameter) is STILL rejected
// — forwarding `fromExpr` only adds narrowing/promotion acceptance, never relaxes
// an ordinary §5.1 type-mismatch.
BEZEL_TEST(null_narrow_value_914, type_mismatch_call_argument_rejected_1191) {
    auto r = runAot(
        "module M; class C "
        "  public static method Greet(u: string): int => u.Length; "
        "  public static method Main(): int => Greet(5); "
        "end C;");
    // fix #3895 (issue #3892): distinguishes a genuine rejection from a
    // runtime crash/signal, which a bare `!ran` cannot.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}
