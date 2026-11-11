// #6677 (spec §7.7, §5.1, v0.357): `T? ?? S?` where `S?` converts to `T?` by
// the nullable lift.
//
//   §7.7: "for a left operand of type `T?`, the right operand must be
//          implicitly convertible (§5.1) to `T` or to `T?`; the result type is
//          `T` when the right operand is non-nullable and `T?` when it is
//          nullable".
//   §5.1 (Nullable conversions): "`S?` implicitly converts to `T?` whenever
//          `S` implicitly converts to `T` — a present value converts by the
//          underlying conversion, `null` converts to `null` (`int?` → `long?`)".
//
// So `long? ?? int?` is well-typed, its result is `long?`, and the right
// operand reaches the result converted. MEASURED on the pre-fix base (r30-c1
// a44c7bec8): `var a: long? := null; var b: int? := 4; var r := a ?? b;
// Console.WriteLine(r ?? 0L);` killed bezelc with SIGSEGV (rc 139). Sema
// admitted the program (#6214's check accepts the lift), and the backend's
// nullable-value `??` arm converted only a BARE right operand (#1046, #5017):
// the right operand's `{i8, i32}` carrier met the left's `i64` payload in one
// PHI node.
//
// Every row prints through the result, so a wrong payload, a wrong
// extension (sign for a signed source, zero for an unsigned one) or a lost
// null all show in stdout. Every row is red on the base; the shapes that were
// already right there (a same-type nullable right operand, #1104; a bare
// one, #1046) are corpus candidates, not gtest rows (lane contract §7).
//
// Not covered here, tracked elsewhere: a REFERENCE-typed left operand with a
// value-typed right one (`Object? ?? int?`, `Object? ?? 5`) is #6629.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

std::string prog(const std::string& body, const std::string& members = "") {
    return "module M;\n"
           "import System;\n"
           "public class Program\n" + members +
           "  public static method Main()\n" + body +
           "  end Main;\n"
           "end Program;\n";
}

void expectPrints(const std::string& src, const std::string& expected) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// The issue's repro verbatim. Pre-fix: bezelc SIGSEGV.
BEZEL_TEST(coalesce_nullable_lift_6677, issue_repro_long_nullable_coalesce_int_nullable_spec_7_7) {
    expectPrints(prog("    var a: long? := null;\n"
                      "    var b: int? := 4;\n"
                      "    var r := a ?? b;\n"
                      "    Console.WriteLine(r ?? 0L);\n"),
                 "4\n");
}

// §5.1: "a present value converts by the underlying conversion, `null`
// converts to `null`" — and a present left operand is the result unchanged
// (a value no `int` can hold, so a payload read at the wrong width shows).
BEZEL_TEST(coalesce_nullable_lift_6677, present_null_and_left_present_states_spec_5_1) {
    expectPrints(prog("    var a: long? := null;\n"
                      "    var b: int? := -4;\n"
                      "    var n: int? := null;\n"
                      "    var c: long? := 7000000000;\n"
                      "    var r1 := a ?? b;\n"
                      "    var r2 := a ?? n;\n"
                      "    var r3 := c ?? b;\n"
                      "    Console.WriteLine(r1 ?? 0L);\n"
                      "    Console.WriteLine(r2 is null);\n"
                      "    Console.WriteLine(r3 ?? 0L);\n"),
                 "-4\nTrue\n7000000000\n");
}

// §5.1's table: `uint` -> `long`, `byte` -> `int`/`ulong`, `ushort` -> `int`
// are value-preserving, so an unsigned source zero-extends; `sbyte`/`short`
// -> `int` sign-extend.
BEZEL_TEST(coalesce_nullable_lift_6677, integer_sources_extend_by_their_own_signedness_spec_5_1) {
    expectPrints(prog("    var a: long? := null;\n"
                      "    var u: uint? := 4000000000;\n"
                      "    Console.WriteLine((a ?? u) ?? 0L);\n"
                      "    var i: int? := null;\n"
                      "    var sb: sbyte? := -5;\n"
                      "    var ub: byte? := 250;\n"
                      "    var sh: short? := -300;\n"
                      "    var us: ushort? := 65000;\n"
                      "    Console.WriteLine((i ?? sb) ?? 0);\n"
                      "    Console.WriteLine((i ?? ub) ?? 0);\n"
                      "    Console.WriteLine((i ?? sh) ?? 0);\n"
                      "    Console.WriteLine((i ?? us) ?? 0);\n"
                      "    var w: ulong? := null;\n"
                      "    Console.WriteLine((w ?? ub) ?? 0);\n"),
                 "4000000000\n-5\n250\n-300\n65000\n250\n");
}

// §5.1's table: `int`/`long`/`uint` -> `double`, `float` -> `double`,
// `int` -> `float`.
BEZEL_TEST(coalesce_nullable_lift_6677, floating_targets_convert_the_payload_spec_5_1) {
    expectPrints(prog("    var a: double? := null;\n"
                      "    var i: int? := -3;\n"
                      "    var f: float? := 2.5f;\n"
                      "    var l: long? := 9L;\n"
                      "    var u: uint? := 4000000000;\n"
                      "    var n: int? := null;\n"
                      "    Console.WriteLine((a ?? i) ?? 0.5);\n"
                      "    Console.WriteLine((a ?? f) ?? 0.5);\n"
                      "    Console.WriteLine((a ?? l) ?? 0.5);\n"
                      "    Console.WriteLine(((a ?? u) ?? 0.5) > 3999999999.0);\n"
                      "    Console.WriteLine((a ?? n) ?? 0.5);\n"
                      "    var g: float? := null;\n"
                      "    var k: int? := 3;\n"
                      "    Console.WriteLine((g ?? k) ?? 0.5f);\n"),
                 "-3\n2.5\n9\nTrue\n0.5\n3\n");
}

// §5.1's table: every integer type -> `decimal`. The payload converts by the
// source's own signedness (a `ulong` above long.MaxValue stays positive).
// Pre-fix: "PHI node operands are not the same type as the result!".
BEZEL_TEST(coalesce_nullable_lift_6677, decimal_target_converts_the_payload_spec_5_1) {
    expectPrints(prog("    var a: decimal? := null;\n"
                      "    var i: int? := 3;\n"
                      "    var l: long? := -9000000000;\n"
                      "    var u: uint? := 4000000000;\n"
                      "    var ul: ulong? := 18000000000000000000;\n"
                      "    var b: byte? := 200;\n"
                      "    var n: int? := null;\n"
                      "    var r := a ?? i;\n"
                      "    Console.WriteLine(r ?? 0m);\n"
                      "    Console.WriteLine((a ?? l) ?? 0m);\n"
                      "    Console.WriteLine((a ?? u) ?? 0m);\n"
                      "    Console.WriteLine((a ?? ul) ?? 0m);\n"
                      "    Console.WriteLine((a ?? b) ?? 0m);\n"
                      "    Console.WriteLine((a ?? n) is null);\n"),
                 "3\n-9000000000\n4000000000\n18000000000000000000\n200\nTrue\n");
}

// §7.7: "a right-associative chain stays nullable until a non-nullable
// operand terminates it".
BEZEL_TEST(coalesce_nullable_lift_6677, chain_of_lifted_operands_spec_7_7) {
    expectPrints(prog("    var a: long? := null;\n"
                      "    var b: int? := null;\n"
                      "    var c: short? := 12;\n"
                      "    var r := a ?? b ?? 0L;\n"
                      "    Console.WriteLine(r);\n"
                      "    var r2 := a ?? b ?? c;\n"
                      "    Console.WriteLine(r2 ?? -1L);\n"),
                 "0\n12\n");
}

// The result is a `long?` at every position that consumes it: a typed local,
// a `return`, a call argument, and the operand of a further operator.
BEZEL_TEST(coalesce_nullable_lift_6677, result_is_the_left_nullable_type_at_every_position_spec_7_7) {
    expectPrints(prog("    var a: long? := null;\n"
                      "    var b: int? := 4;\n"
                      "    var r: long? := a ?? b;\n"
                      "    Console.WriteLine(r ?? 0L);\n"
                      "    Console.WriteLine(Program.Pick(a, b) ?? 0L);\n"
                      "    Program.Show(a ?? b);\n"
                      "    var w: long := (a ?? b) ?? 1L;\n"
                      "    Console.WriteLine(w + 10000000000L);\n",
                      "  public static method Pick(a: long?, b: int?): long?\n"
                      "    return a ?? b;\n"
                      "  end Pick;\n"
                      "  public static method Show(v: long?)\n"
                      "    Console.WriteLine(v ?? -1L);\n"
                      "  end Show;\n"),
                 "4\n4\n4\n10000000004\n");
}

// The right operand as a call result and as a static field; and §7.7's
// evaluation rule — "the right operand is evaluated only when the left
// operand's value is `null`" — holds for a lifted right operand too.
BEZEL_TEST(coalesce_nullable_lift_6677, call_and_field_right_operands_and_short_circuit_spec_7_7) {
    expectPrints(prog("    var a: long? := null;\n"
                      "    Console.WriteLine((a ?? Program.Mk(true)) ?? 0L);\n"
                      "    Console.WriteLine((a ?? Program.Mk(false)) is null);\n"
                      "    Console.WriteLine((a ?? Program.sf) ?? 0L);\n"
                      "    var p: long? := 5L;\n"
                      "    var r := p ?? Program.Loud();\n"
                      "    Console.WriteLine(r ?? 0L);\n",
                      "  private static sf: int? := 77;\n"
                      "  public static method Mk(some: bool): int?\n"
                      "    if some then return 41; end;\n"
                      "    return null;\n"
                      "  end Mk;\n"
                      "  public static method Loud(): int?\n"
                      "    Console.WriteLine(\"evaluated\");\n"
                      "    return 1;\n"
                      "  end Loud;\n"),
                 "41\nTrue\n77\n5\n");
}

// The lift inside a loop body, inside a lambda body, and with the left
// operand read from a generic class's `T?` field at T = long.
BEZEL_TEST(coalesce_nullable_lift_6677, loop_lambda_and_generic_field_left_operand_spec_7_7) {
    expectPrints("module M;\n"
                 "import System;\n"
                 "public class Box<T>\n"
                 "  public V: T?;\n"
                 "  public constructor(v: T?)\n"
                 "    this.V := v;\n"
                 "  end constructor;\n"
                 "end Box;\n"
                 "public class Program\n"
                 "  public static method Main()\n"
                 "    var a: long? := null;\n"
                 "    var total: long := 0L;\n"
                 "    for var i in 0..3 do\n"
                 "      var b: int? := i;\n"
                 "      total := total + ((a ?? b) ?? 100L);\n"
                 "    end for;\n"
                 "    Console.WriteLine(total);\n"
                 "    var f := lambda(x: long?, y: int?) => (x ?? y) ?? -1L;\n"
                 "    Console.WriteLine(f(null, 8));\n"
                 "    Console.WriteLine(f(null, null));\n"
                 "    var bx := new Box<long>(null);\n"
                 "    var k: int? := 5;\n"
                 "    Console.WriteLine((bx.V ?? k) ?? 0L);\n"
                 "  end Main;\n"
                 "end Program;\n",
                 "6\n8\n-1\n5\n");
}
