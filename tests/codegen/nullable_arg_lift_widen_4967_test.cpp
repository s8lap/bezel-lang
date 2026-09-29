// Regression tests for issue #4967 — §5.1 "Nullable conversions" `S? → T?`
// at a CALL ARGUMENT: an `int?` argument bound to a `long?` formal (and
// `float?` → `double?`, `uint?` → `ulong?`).
//
// The argument is already the tagged `{i8, S}` struct, so buildOrderedArgs'
// exact-payload wrap and #4922's widen-then-wrap both skipped it, and it
// reached the call as `{i8, i32}` against `{i8, i64}`: "Call parameter type
// does not match function signature!" — IR verification failure. The
// omitted-default fill (`b: long? := a`, `a: int?`) had the same gap.
//
// spec §5.1 "Implicit conversions" (int → long, float → double, uint → ulong)
// and "Nullable conversions" (the rules compose through at most one `?` on
// each side), §22.5 (a default may reference an earlier parameter), §22.6
// stage 3, v0.307.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

void expectPrints(const std::string& src, const std::string& expected) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// per §5.1 (v0.307), fix #4967 — the issue's repro (`int?` at `long?`),
// present and null.
BEZEL_TEST(nullable_arg_lift_widen_4967, int_nullable_at_long_nullable_present_and_null) {
    expectPrints(
        "module M; "
        "class P "
        "  public static method F(a: long?): long return a ?? -1; end F; "
        "  public static method Main(): int "
        "    var x: int? := 5; var n: int? := null; "
        "    Console.WriteLine(F(x)); Console.WriteLine(F(n)); return 0; "
        "  end Main; "
        "end P;",
        "5\n-1\n");
}

// per §5.1 (v0.307), fix #4967 — `float?` → `double?` (FPExt of the payload).
BEZEL_TEST(nullable_arg_lift_widen_4967, float_nullable_at_double_nullable) {
    expectPrints(
        "module M; "
        "class P "
        "  public static method D(x: double?): double return x ?? 0.5; end D; "
        "  public static method Main(): int "
        "    var f: float? := 2; var fn: float? := null; "
        "    Console.WriteLine($\"{D(f)}/{D(fn)}\"); return 0; "
        "  end Main; "
        "end P;",
        "2/0.5\n");
}

// per §5.1 (v0.307), fix #4967 — `uint?` → `ulong?` must ZERO-extend: a
// sign-extension would print 18446744069414584320.
BEZEL_TEST(nullable_arg_lift_widen_4967, uint_nullable_at_ulong_nullable_zero_extends) {
    expectPrints(
        "module M; "
        "class P "
        "  public static method U(y: ulong?): ulong return y ?? 9; end U; "
        "  public static method Main(): int "
        "    var u: uint? := 4000000000; var un: uint? := null; "
        "    Console.WriteLine($\"{U(u)}/{U(un)}\"); return 0; "
        "  end Main; "
        "end P;",
        "4000000000/9\n");
}

// per §5.1 / §22.5 (v0.307), fix #4967 — the ctor form `new J(x)`, whose
// omitted `b: long? := a` default lifts the earlier `long?` formal (and the
// static-method default `b: long? := a` from an `int?` formal: the
// fillOmittedParamDefaults twin).
BEZEL_TEST(nullable_arg_lift_widen_4967, ctor_new_and_omitted_default_lift) {
    expectPrints(
        "module M; "
        "class J "
        "  public readonly property Tag: string; "
        "  public constructor(a: long?, b: long? := a) "
        "    Tag := $\"{a ?? -1}/{b ?? -2}\"; end constructor; "
        "end J; "
        "class P "
        "  public static method Dflt(a: int?, b: long? := a): long "
        "    return b ?? -7; "
        "  end Dflt; "
        "  public static method Main(): int "
        "    var x: int? := 5; var n: int? := null; "
        "    using var j := new J(x); Console.WriteLine(j.Tag); "
        "    using var j2 := new J(n); Console.WriteLine(j2.Tag); "
        "    Console.WriteLine($\"{Dflt(x)}/{Dflt(n)}\"); return 0; "
        "  end Main; "
        "end P;",
        "5/5\n-1/-2\n5/-7\n");
}

// per §5.1 / §28.3 (v0.307), fix #4967 — 50 constructions in a loop with an
// `int?` at the `long?` formal, alternating present and null; the sum and the
// heap `Tag` lengths pin every lifted value.
BEZEL_TEST(nullable_arg_lift_widen_4967, ctor_loop_50_iterations) {
    expectPrints(
        "module M; "
        "class J "
        "  public readonly property Tag: string; "
        "  public readonly property V: long; "
        "  public constructor(a: long?) "
        "    Tag := $\"N{a ?? -1}\"; V := a ?? 0; end constructor; "
        "end J; "
        "class P "
        "  public static method Main(): int "
        "    var sum: long := 0; var len: int := 0; "
        "    for var i in 0..<50 do "
        "      var x: int? := null; "
        "      if i mod 2 = 0 then x := i; end; "
        "      using var k := new J(x); sum := sum + k.V; "
        "      len := len + k.Tag.Length; "
        "    end; "
        "    Console.WriteLine($\"{sum}/{len}\"); return 0; "
        "  end Main; "
        "end P;",
        "600/145\n");
}
