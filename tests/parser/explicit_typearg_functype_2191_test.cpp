// Spec-derived parser tests for explicit generic-method type arguments that
// are FUNCTION TYPES (issue #2191).
//
// §A.5 Type / §A.5 GenericArg / §22.12 (v0.226):
//   GenericArg  ::= Type                       -- the same Type production
//   Type        ::= ... | FunctionType | "(" FunctionType ")" | "(" ")"
//   FunctionType::= "(" (FuncParamType ("," FuncParamType)* ","?)? ")"
//                   "->" "owned"? Type
//
// A function type is a valid generic type argument everywhere a Type is
// spelled — including in `new Wrap<(int) -> int>(...)` and in field/parameter
// annotations, which the parser already accepted. But an EXPLICIT generic
// type-argument list on a CALL (`H.Probe<(int) -> int>(o)`) or on a
// receiver (`Wrap<(int) -> int>.Make(o)`) goes through the §22.12 generic-
// vs-relational `<` disambiguation scan, which before this fix did not
// recognize the function-type spelling: it bailed on `(`, `)`, `->`, and the
// FuncParamMods keywords, so the scan returned false and the `<` was read as
// a relational operator — leaving the program with a cascade of "expected ';'
// after statement" / "expected expression" parse errors at the `->`.
//
// The fix (§22.12's "balanced <…> group respecting nesting") tracks the
// function-type parameter-list nesting alongside the angle-bracket nesting,
// admits the `->` separator, the `:` of a named function-type parameter, and
// the reserved FuncParamMods keywords (`owned` / `ref` / `out`; `scoped` is a
// contextual identifier already accepted), and bails when a `)` would close
// a paren group that never opened (so a genuine relational chain is still
// not misread). The authority for what is a valid Type remains parseType.
//
// These tests exercise the parse only (parseAll: lex+parse, no sema). The
// deeper sema/codegen walls a generic method instantiated at a function type
// hits today (§5.1 conversion of a function value to a bare `T` parameter,
// §19.3/#1939 function-type runtime-erasure on a type test) are recorded in
// #2191's body as separate, unrelated walls and are out of scope here; this
// test pins only that the SYNTAX is accepted by the parser.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;

static bool parses(const std::string& source) {
    auto r = parseAll(source);
    return !r.diagnostics.hasErrors();
}

// ── Method-call position: `H.Probe<FUNCTION_TYPE>(o)` ───────────────────────

// §A.5 / §22.12 / #2191: the issue's exact repro spelling — a one-param
// function type as an explicit method-call type argument. Before the fix the
// parser stopped at `Probe<(int)` and reported "expected ';' after return
// statement" / "expected expression" at the `->`.
BEZEL_TEST(parser_explicit_typearg_functype_2191, method_call_one_param_functype) {
    BEZEL_EXPECT_TRUE(parses(
        "module M; "
        "static class H "
        "  public static method Probe<T>(o: Object): int => 0; "
        "  public static method Run(o: Object): int "
        "    => H.Probe<(int) -> int>(o); "
        "end H;"));
}

// §A.5 FunctionType zero-param form `() -> T`.
BEZEL_TEST(parser_explicit_typearg_functype_2191, method_call_zero_param_functype) {
    BEZEL_EXPECT_TRUE(parses(
        "module M; "
        "static class H "
        "  public static method Probe<T>(o: Object): int => 0; "
        "  public static method Run(o: Object): int "
        "    => H.Probe<() -> int>(o); "
        "end H;"));
}

// §A.5 FunctionType multi-param form `(T1, T2) -> R`.
BEZEL_TEST(parser_explicit_typearg_functype_2191, method_call_multi_param_functype) {
    BEZEL_EXPECT_TRUE(parses(
        "module M; "
        "static class H "
        "  public static method Probe<T>(o: Object): int => 0; "
        "  public static method Run(o: Object): int "
        "    => H.Probe<(int, bool) -> string>(o); "
        "end H;"));
}

// §A.5 FunctionType parameter modifiers (`owned` / `ref` / `out` /
// `scoped`) inside the parameter list — reserved keywords the scan must
// admit so the modified function-type spellings parse.
BEZEL_TEST(parser_explicit_typearg_functype_2191, method_call_modified_param_functype) {
    BEZEL_EXPECT_TRUE(parses(
        "module M; "
        "class Conn public constructor() end constructor; end Conn; "
        "static class H "
        "  public static method Probe<T>(o: Object): int => 0; "
        "  public static method Run(o: Object): int "
        "    var a: int := H.Probe<(owned Conn) -> ()>(o); "
        "    var b: int := H.Probe<(ref int) -> ()>(o); "
        "    var c: int := H.Probe<(out int) -> ()>(o); "
        "    var d: int := H.Probe<(out owned int) -> ()>(o); "
        "    return a + b + c + d; "
        "  end Run; "
        "end H;"));
}

// §27.1 / §A.5 (v0.175): `scoped` callback-parameter modifier inside a
// function-type GenericArg.
BEZEL_TEST(parser_explicit_typearg_functype_2191, method_call_scoped_param_functype) {
    BEZEL_EXPECT_TRUE(parses(
        "module M; "
        "class Task public constructor() end constructor; end Task; "
        "static class H "
        "  public static method Probe<T>(o: Object): int => 0; "
        "  public static method Run(o: Object): int "
        "    => H.Probe<(scoped () -> Task) -> ()>(o); "
        "end H;"));
}

// §A.5 "(" FunctionType ")" — a higher-order function type as a GenericArg:
// `((int) -> int) -> bool`. Exercises nested paren depth tracking in the
// disambiguation scan.
BEZEL_TEST(parser_explicit_typearg_functype_2191, method_call_higher_order_functype) {
    BEZEL_EXPECT_TRUE(parses(
        "module M; "
        "static class H "
        "  public static method Probe<T>(o: Object): int => 0; "
        "  public static method Run(o: Object): int "
        "    => H.Probe<((int) -> int) -> bool>(o); "
        "end H;"));
}

// §A.5 unit type `()` as a GenericArg.
BEZEL_TEST(parser_explicit_typearg_functype_2191, method_call_unit_type_arg) {
    BEZEL_EXPECT_TRUE(parses(
        "module M; "
        "static class H "
        "  public static method Probe<T>(o: Object): int => 0; "
        "  public static method Run(o: Object): int "
        "    => H.Probe<()>(o); "
        "end H;"));
}

// §A.5 nullable return slot: `(int) -> int?` — the `?` binds to the return
// type, not the function type, per the §27.2 note.
BEZEL_TEST(parser_explicit_typearg_functype_2191, method_call_nullable_return_functype) {
    BEZEL_EXPECT_TRUE(parses(
        "module M; "
        "static class H "
        "  public static method Probe<T>(o: Object): int => 0; "
        "  public static method Run(o: Object): int "
        "    => H.Probe<(int) -> int?>(o); "
        "end H;"));
}

// §A.5 FuncParamType `Identifier ":" Type` — a named function-type
// parameter inside a GenericArg, admitted by the scan via `:`.
BEZEL_TEST(parser_explicit_typearg_functype_2191, method_call_named_param_functype) {
    BEZEL_EXPECT_TRUE(parses(
        "module M; "
        "static class H "
        "  public static method Probe<T>(o: Object): int => 0; "
        "  public static method Run(o: Object): int "
        "    => H.Probe<(x: int) -> int>(o); "
        "end H;"));
}

// ── Receiver position: `Wrap<FUNCTION_TYPE>.Member` ────────────────────────

// §22.12 / #2191: same function-type admission on a receiver-position type
// argument list (the `> .` form of the disambiguation).
BEZEL_TEST(parser_explicit_typearg_functype_2191, receiver_position_functype) {
    BEZEL_EXPECT_TRUE(parses(
        "module M; "
        "class Wrap<T> "
        "  public constructor() end constructor; "
        "  public static method Make(o: Object): int => 0; "
        "end Wrap; "
        "public static class P "
        "  public static method Run(o: Object): int "
        "    var a: int := Wrap<(int) -> int>.Make(o); "
        "    var b: int := Wrap<() -> int>.Make(o); "
        "    var c: int := Wrap<((int) -> int) -> bool>.Make(o); "
        "    return a + b + c; "
        "  end Run; "
        "end P;"));
}

// ── Regression guards: the relational-vs-generic boundary is unchanged ────

// §17.1 / §22.12 / #520 control: a plain `a < b` must still parse as a
// relational comparison, not be swallowed by the type-arg scan. Admitting
// function-type tokens must not loosen the disambiguation for relational
// chains that have no function-type shape.
BEZEL_TEST(parser_explicit_typearg_functype_2191, relational_lt_not_misinterpreted) {
    BEZEL_EXPECT_TRUE(parses(
        "module M; public static class P "
        "  public static method Run(a: int, b: int, c: int): bool "
        "    => a < b and b > c; "
        "end P;"));
}

// §17.1 / §22.12 control: a relational `<` whose RHS is a parenthesized
// expression (`a < (b + c)`) must still parse as relational — the new `(`
// admission is gated by paren-depth tracking and the `+` bail, so a
// balanced paren group whose interior is not a Type still falls through.
BEZEL_TEST(parser_explicit_typearg_functype_2191, relational_paren_rhs_not_misinterpreted) {
    BEZEL_EXPECT_TRUE(parses(
        "module M; public static class P "
        "  public static method Run(a: int, b: int, c: int): bool "
        "    => a < (b + c); "
        "end P;"));
}
