// Codegen slice CG42 — null-conditional call `x?.M(args)` (§7.5). When the
// callee is a null-conditional member access, the call is skipped if the
// receiver is null (no effect / null result); otherwise the method is
// invoked. Void-returning, reference-returning, AND value-returning calls
// (`int`, `bool`, an enum, a record — lifted to the tagged {hasValue, value}
// `T?` struct, FINDING [3] ADDENDUM) are covered. Observable via the int exit
// code.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

int runProg(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// The `?.` rows below pin a VALUE, so they read the program's stdout (the
// channel the harness documents for observables that do not fit an 8-bit exit
// code). The checked oracle is the strict one: it also verifies that the
// closure reference the getter handed the reader is given back.
std::string stdoutChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return (r.ran && r.exitCode == 0) ? r.stdoutText
                                      : std::string("<did-not-run>");
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

// #738 / §27.7: null-conditional INVOCATION of a nullable FUNCTION VALUE
// `f?(args)`. When f is non-null it is invoked and the result is the return
// type made nullable; when null the call is skipped and the result is null.
// Previously NullCondCallExpr was unimplemented (produced a wrong value).
BEZEL_TEST(null_cond_call, function_value_present_invokes_738) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class C public static method Main(): int "
        "  var f: ((int) -> int)? := lambda(x: int) => x * 2; "
        "  var r := f?(5); "  // 10, as int?
        "  if r is null then return 2; end; "
        "  if r = 10 then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 0);
}
BEZEL_TEST(null_cond_call, function_value_null_skips_738) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class C public static method Main(): int "
        "  var f: ((int) -> int)? := null; "
        "  var r := f?(5); "
        "  if r is null then return 7; end; "
        "  return 1; "
        "end Main; end C;"), 7);
}
BEZEL_TEST(null_cond_call, function_value_reference_return_738) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class C public static method Main(): int "
        "  var f: ((int) -> string)? := lambda(x: int) => \"v\"; "
        "  var r := f?(3); "
        "  if r is null then return 2; end; "
        "  return r!.Length; "  // 'v'.Length = 1
        "end Main; end C;"), 1);
}

// fix #747 R43 / §27.7: a null-conditional call on a VOID-returning nullable
// function (`((int) -> ())?`) is a no-op when null. This aborted the compiler
// with a PHI type-mismatch assertion: the lowering built a `{i8, void}`
// nullable struct + PHI for a result that does not exist. The spec example
// `maybeHandler?(42)` (§27.7) must compile and run.

// Null handler → the call is skipped, no crash, program returns 0.
BEZEL_TEST(null_cond_call, void_function_value_null_is_noop_747) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class C public static method Main(): int "
        "  var maybeHandler: ((int) -> ())? := null; "
        "  maybeHandler?(42); "  // no-op, §27.7
        "  return 0; "
        "end Main; end C;"), 0);
}

// Non-null void handler → the call runs (and observably mutates state); a null
// handler in the same program is skipped. total = 5 + 7 = 12.
BEZEL_TEST(null_cond_call, void_function_value_present_runs_747) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class Box "
        "  public total: int := 0; "
        "  public method Add(n: int) this.total := this.total + n; end Add; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); "
        "  var h: ((int) -> ())? := lambda(n) => b.Add(n); "
        "  h?(5); "
        "  h?(7); "
        "  var none: ((int) -> ())? := null; "
        "  none?(99); "  // skipped
        "  return b.total; "  // 12
        "end Main; end C;"), 12);
}

// fix #8979 (§7.5 / §27.1 / §28.9, v0.359): a null-conditional CALL of a
// function-typed PROPERTY — `hn?.P()` where `P` is `property P: () -> int`.
//
// THE DEFECT. §7.5: "`x?.M` returns null if `x` is null; otherwise it
// performs the access." The member is resolved as `x.M` is, so the `?.`
// spelling of a function-typed property INVOKES the property's function value
// (§27.1: a property of function type is a callable value), exactly as
// `x.M(args)` does.
//
// MEASURED on the base: the call compiled with two `warning: internal:`
// lines — the #2477 "codegen cannot dispatch member 'P'" note and the
// #2434 "initializer expression failed to lower to a value" note — and the
// program's `P` slot was a ZERO value: the non-null receiver produced
// `P=null` (the property's function value, which returns 5, was never
// invoked), at exit 0. The `.` spelling `h.P()` printed 5 and the `?.`
// spelling of a METHOD (`hn?.M()`) printed 7, so only the `?.` + invoke-a-
// function-typed-property shape was broken.
//
// ROOT CAUSE: the `?.` call lowering lowers a synthetic `.` TWIN call through
// the `.` spelling's OWN dispatch, but the twin's callee carried NO SYMBOL
// (the member symbol rides on the NullCondMemberExpr), so the twin reached
// `lowerCall` with no callee symbol, missed every symbol-based dispatch arm
// — including the PropertySymbol arm fix #2589 added for `obj.P(args)` — and
// fell through to the #2477 zero-init fallback.
//
// THE ROW prints the invoked value for the non-null receiver and the null
// case, so the null case is pinned by the SAME row (the base prints
// "P=null" for BOTH, so the row is red on the base).
BEZEL_TEST(codegen_cg42, safenav_call_of_function_typed_property_8979) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public class H "
        "  private f: () -> int; "
        "  public constructor() f := lambda(): int => 5; end; "
        "  public property P: () -> int "
        "    get => do return f; end; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    using var h := new H(); "
        "    var hn: H? := h; "
        "    var r := hn?.P(); "
        "    if r = null then Console.WriteLine(\"P=null\"); "
        "    else Console.WriteLine($\"P={r!}\"); end; "
        "    var hn2: H? := null; "
        "    var r2 := hn2?.P(); "
        "    if r2 = null then Console.WriteLine(\"P2=null\"); "
        "    else Console.WriteLine($\"P2={r2!}\"); end; "
        "    return 0; "
        "  end; "
        "end Program;"),
        "P=5\nP2=null\n");
}

// fix #8979 (§7.5 / §27.1, v0.359): the AUTO-property spelling of the same
// shape, with an ARGUMENT — `hn?.AP(10)` invokes the property's function
// value with 10 and yields its result (11), not the property's value. The
// base degraded the slot to zero and printed "AP=null".
BEZEL_TEST(codegen_cg42, safenav_call_of_function_typed_auto_property_8979) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public class H "
        "  public property AP: (int) -> int := lambda(x: int) => x + 1; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    using var h := new H(); "
        "    var hn: H? := h; "
        "    var r := hn?.AP(10); "
        "    if r = null then Console.WriteLine(\"AP=null\"); "
        "    else Console.WriteLine($\"AP={r!}\"); end; "
        "    return 0; "
        "  end; "
        "end Program;"),
        "AP=11\n");
}

// fix #8979 (§7.5 / §27.1, v0.359): the FUNCTION-TYPED FIELD sibling — the
// same root cause (the `?.` twin carried no symbol, so lowerCall's
// FieldSymbol arm — fix #2300 — never fired), the same one-hunk fix. The
// base degraded the slot to zero and printed "F=null"; the `.` spelling
// `h.F(10)` printed 110.
BEZEL_TEST(codegen_cg42, safenav_call_of_function_typed_field_8979) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public class H "
        "  public F: (int) -> int := lambda(x: int) => x + 100; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    using var h := new H(); "
        "    var hn: H? := h; "
        "    var r := hn?.F(10); "
        "    if r = null then Console.WriteLine(\"F=null\"); "
        "    else Console.WriteLine($\"F={r!}\"); end; "
        "    return 0; "
        "  end; "
        "end Program;"),
        "F=110\n");
}

// fix #8979 (§7.5 / §27.1, v0.359): the same call through an INTERFACE-typed
// receiver, where the property read dispatches through the itable
// (`lowerVirtualPropertyGet`) rather than to a concrete getter. The `.`
// spelling is the positive control inside the same program: it reads 9 on
// the base and on the tip, while the `?.` spelling degraded to null on the
// base.
BEZEL_TEST(codegen_cg42, safenav_call_of_function_typed_property_via_interface_8979) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module M; "
        "public interface I "
        "  readonly property P: () -> int; "
        "end I; "
        "public class C : I "
        "  private f: () -> int; "
        "  public constructor() f := lambda(): int => 9; end; "
        "  public property P: () -> int "
        "    get => do return f; end; "
        "  end; "
        "end C; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    using var c := new C(); "
        "    var ic: I? := c; "
        "    Console.WriteLine($\"dot={ic!.P()!}\"); "
        "    var r := ic?.P(); "
        "    if r = null then Console.WriteLine(\"safenav=null\"); "
        "    else Console.WriteLine($\"safenav={r!}\"); end; "
        "    return 0; "
        "  end; "
        "end Program;"),
        "dot=9\nsafenav=9\n");
}
