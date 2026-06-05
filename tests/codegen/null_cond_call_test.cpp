// Codegen slice CG42 — null-conditional call `x?.M(args)` (§7.5). When the
// callee is a null-conditional member access, the call is skipped if the
// receiver is null (no effect / null result); otherwise the method is
// invoked. Void-returning, reference-returning, AND value-returning calls
// (`int`, `bool`, an enum, a record — lifted to the tagged {hasValue, value}
// `T?` struct, FINDING [3] ADDENDUM) are covered. Observable via the int exit
// code.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int runProg(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg42, void_call_runs_when_non_null) {
    // Two `?.` calls on a non-null receiver both run (each bumps V).
    BEZEL_EXPECT_EQ(runProg(
        "module M; class N "
        "  public V: int := 0; "
        "  public method Bump() V := V + 1; end Bump; "
        "end N; "
        "class C public static method Main(): int "
        "  var n: N? := new N(); n?.Bump(); n?.Bump(); return n!.V; "
        "end Main; end C;"), 2);
}

BEZEL_TEST(codegen_cg42, void_call_skipped_when_null) {
    // A `?.` call on a null receiver is skipped — Bump (which would deref
    // `this`) is never entered, so the program completes and returns 7.
    BEZEL_EXPECT_EQ(runProg(
        "module M; class N "
        "  public V: int := 0; "
        "  public method Bump() V := V + 1; end Bump; "
        "end N; "
        "class C public static method Main(): int "
        "  var n: N? := null; n?.Bump(); return 7; "
        "end Main; end C;"), 7);
}

BEZEL_TEST(codegen_cg42, ref_returning_call_non_null) {
    // A reference-returning `?.` call on a non-null receiver yields the
    // method's result.
    BEZEL_EXPECT_EQ(runProg(
        "module M; class N "
        "  public V: int := 0; "
        "  public method Self(): N => this; "
        "end N; "
        "class C public static method Main(): int "
        "  var n := new N(); n.V := 5; var nn: N? := n; "
        "  var r := nn?.Self(); return r!.V; "
        "end Main; end C;"), 5);
}

BEZEL_TEST(codegen_cg42, ref_returning_call_null_yields_null) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class N "
        "  public method Self(): N => this; "
        "end N; "
        "class C public static method Main(): int "
        "  var nn: N? := null; var r := nn?.Self(); "
        "  if r = null then return 8; end; return 0; "
        "end Main; end C;"), 8);
}

BEZEL_TEST(codegen_cg42, call_with_arguments) {
    // Arguments are evaluated and passed when the call runs.
    BEZEL_EXPECT_EQ(runProg(
        "module M; class N "
        "  public V: int := 0; "
        "  public method Add(d: int) V := V + d; end Add; "
        "end N; "
        "class C public static method Main(): int "
        "  var n: N? := new N(); n?.Add(40); n?.Add(2); return n!.V; "
        "end Main; end C;"), 42);
}

// FINDING [3] ADDENDUM — a value-returning (`int`) `?.` call on a non-null
// receiver yields the result lifted to `int?` (§7.5: always nullable).
BEZEL_TEST(codegen_cg42, value_int_returning_call_non_null) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class N "
        "  public V: int := 0; "
        "  public method GetV(): int => V; "
        "end N; "
        "class C public static method Main(): int "
        "  var n := new N(); n.V := 42; var nn: N? := n; "
        "  var x: int? := nn?.GetV(); return x!; "
        "end Main; end C;"), 42);
}

// FINDING [3] ADDENDUM — a value-returning `?.` call on a NULL receiver yields a
// null `int?` (the method is not invoked; previously: garbage).
BEZEL_TEST(codegen_cg42, value_int_returning_call_null_yields_null) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class N "
        "  public method GetV(): int => 1; "
        "end N; "
        "class C public static method Main(): int "
        "  var nn: N? := null; var x: int? := nn?.GetV(); "
        "  if x = null then return 8; end; return 0; "
        "end Main; end C;"), 8);
}

// FINDING [3] ADDENDUM — value-returning `?.` call feeds `??`: the non-null case
// supplies the call result, the null case the default.
BEZEL_TEST(codegen_cg42, value_int_returning_call_feeds_coalesce) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class N "
        "  public V: int := 0; "
        "  public method GetV(): int => V; "
        "end N; "
        "class C public static method Main(): int "
        "  var n := new N(); n.V := 7; var nn: N? := n; "
        "  var got: int := nn?.GetV() ?? 99; "
        "  var no: N? := null; var def: int := no?.GetV() ?? 99; "
        "  return got + def; "  // 7 + 99
        "end Main; end C;"), 106);
}

// FINDING [3] ADDENDUM — a `bool`-returning `?.` call lifts to `bool?`.
BEZEL_TEST(codegen_cg42, value_bool_returning_call_non_null) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class N "
        "  public V: int := 0; "
        "  public method IsPos(): bool => V > 0; "
        "end N; "
        "class C public static method Main(): int "
        "  var n := new N(); n.V := 5; var nn: N? := n; "
        "  var x: bool? := nn?.IsPos(); "
        "  if x = null then return 0; elif x! then return 1; else return 2; end; "
        "end Main; end C;"), 1);
}
