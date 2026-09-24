// fix #4779 (§22.5 "Optional parameters and named arguments" / §22.6 stage 1,
// v0.307): codegen's constructor selector `ctorForArgs`
// (src/codegen/LlvmBackend_impl.hpp) treated an EQUAL-ARITY constructor as the
// candidate even when a named argument names none of its parameters. §22.5:
// each supplied argument binds exactly one parameter, and binding precedes
// type applicability (§22.6's resolution pipeline, stage 1) — so
// `new C(x := 5)` against `constructor(z: int)` is inapplicable and the
// defaulted `constructor(x: int := 1, y: int := 0)` is the only survivor.
//
// Pre-fix, with exactly ONE equal-arity sibling, the arm returned null (the
// #3760 type-class rejection) instead of continuing to the defaulted
// candidates, and the `new` silently ZERO-FILLED the object: `Tag` printed as
// an empty line, exit 0, no diagnostic. With TWO equal-arity siblings the
// "first by arity" fallback bound the wrong constructor outright.
//
// Every row is a VALUE assertion on stdout — the pre-fix failure compiles,
// runs and exits 0, so `ran`/exit code alone would prove nothing.
#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// The issue's repro (p2c_z_string.bzl): `x :=` only and `y :=` only against
// the defaulted ctor beside an arity-1 `(z: int)` sibling. Pre-fix: `Z5`
// then two EMPTY lines.
BEZEL_TEST(codegen_ctor_named_arg_defaulted_sibling_arity_4779,
           x_only_and_y_only_beside_arity_one_sibling_spec_22_5) {
    auto r = runAot(
        "module M; "
        "public class C "
        "  public readonly property Tag: string; "
        "  public constructor(x: int := 1, y: int := 0) "
        "    Tag := $\"XY{x},{y}\"; "
        "  end constructor; "
        "  public constructor(z: int) "
        "    Tag := $\"Z{z}\"; "
        "  end constructor; "
        "end C; "
        "public class Program "
        "  public static method Main(): int "
        "    var c := new C(z := 5); "
        "    Console.WriteLine(c.Tag); "
        "    var d := new C(x := 5); "
        "    Console.WriteLine(d.Tag); "
        "    var e := new C(y := 6); "
        "    Console.WriteLine(e.Tag); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "Z5\nXY5,0\nXY1,6\n");
}

// The skip-the-middle shape (p3_skip_middle.bzl): `new D(1, c := 9)` binds
// `a` positionally and `c` by name, defaulting `b`, beside an arity-2
// `(p, q)` sibling that `c :=` cannot bind. `C` (no sibling) is the control
// that already worked. Pre-fix: `ABC1,2,9` then an EMPTY line.
BEZEL_TEST(codegen_ctor_named_arg_defaulted_sibling_arity_4779,
           skip_middle_named_beside_arity_two_sibling_spec_22_5) {
    auto r = runAot(
        "module M; "
        "public class C "
        "  public readonly property Tag: string; "
        "  public constructor(a: int, b: int := 2, c: int := 3) "
        "    Tag := $\"ABC{a},{b},{c}\"; "
        "  end constructor; "
        "end C; "
        "public class D "
        "  public readonly property Tag: string; "
        "  public constructor(a: int, b: int := 2, c: int := 3) "
        "    Tag := $\"ABC{a},{b},{c}\"; "
        "  end constructor; "
        "  public constructor(p: int, q: int) "
        "    Tag := $\"PQ{p},{q}\"; "
        "  end constructor; "
        "end D; "
        "public class Program "
        "  public static method Main(): int "
        "    var c := new C(1, c := 9); "
        "    Console.WriteLine(c.Tag); "
        "    var d := new D(1, c := 9); "
        "    Console.WriteLine(d.Tag); "
        "    var e := new D(1, 4); "
        "    Console.WriteLine(e.Tag); "
        "    var f := new D(q := 8, p := 7); "
        "    Console.WriteLine(f.Tag); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ABC1,2,9\nABC1,2,9\nPQ1,4\nPQ7,8\n");
}

// TWO equal-arity siblings (`(z: int)`, `(w: string)`): the multi-candidate
// arm's "first by arity" fallback must not adopt a candidate the named
// argument cannot bind. Pre-fix `new C(x := 5)` / `new C(y := 6)` bound
// `(z: int)` and printed `Z5` / `Z6`.
BEZEL_TEST(codegen_ctor_named_arg_defaulted_sibling_arity_4779,
           two_equal_arity_siblings_spec_22_6) {
    auto r = runAot(
        "module M; "
        "public class C "
        "  public readonly property Tag: string; "
        "  public constructor(x: int := 1, y: int := 0) "
        "    Tag := $\"XY{x},{y}\"; "
        "  end constructor; "
        "  public constructor(z: int) "
        "    Tag := $\"Z{z}\"; "
        "  end constructor; "
        "  public constructor(w: string) "
        "    Tag := $\"W{w}\"; "
        "  end constructor; "
        "end C; "
        "public class Program "
        "  public static method Main(): int "
        "    var a := new C(x := 5); "
        "    Console.WriteLine(a.Tag); "
        "    var b := new C(y := 6); "
        "    Console.WriteLine(b.Tag); "
        "    var c := new C(z := 7); "
        "    Console.WriteLine(c.Tag); "
        "    var d := new C(w := \"s\"); "
        "    Console.WriteLine(d.Tag); "
        "    var e := new C(); "
        "    Console.WriteLine(e.Tag); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "XY5,0\nXY1,6\nZ7\nWs\nXY1,0\n");
}

// Sibling-arity control: calls the equal-arity sibling CAN bind (positional,
// and by its own name) still select it; the no-sibling class still fills its
// defaults. Passes on both sides of the fix — pins that the fix did not
// re-route the equal-arity match.
BEZEL_TEST(codegen_ctor_named_arg_defaulted_sibling_arity_4779,
           control_equal_arity_sibling_still_selected_spec_22_6) {
    auto r = runAot(
        "module M; "
        "public class C "
        "  public readonly property Tag: string; "
        "  public constructor(x: int := 1, y: int := 0) "
        "    Tag := $\"XY{x},{y}\"; "
        "  end constructor; "
        "  public constructor(z: int) "
        "    Tag := $\"Z{z}\"; "
        "  end constructor; "
        "end C; "
        "public class N "
        "  public readonly property Tag: string; "
        "  public constructor(x: int := 1, y: int := 0) "
        "    Tag := $\"XY{x},{y}\"; "
        "  end constructor; "
        "end N; "
        "public class Program "
        "  public static method Main(): int "
        "    var a := new C(7); "
        "    Console.WriteLine(a.Tag); "
        "    var b := new C(z := 8); "
        "    Console.WriteLine(b.Tag); "
        "    var c := new C(2, 3); "
        "    Console.WriteLine(c.Tag); "
        "    var d := new C(); "
        "    Console.WriteLine(d.Tag); "
        "    var e := new N(y := 5); "
        "    Console.WriteLine(e.Tag); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "Z7\nZ8\nXY2,3\nXY1,0\nXY1,5\n");
}

// Loop-amplified (50 iterations): the selection is a compile-time choice, so
// every iteration takes the same path; the sum makes a zero-filled
// construction visible as a wrong total (pre-fix every sum reads 0).
BEZEL_TEST(codegen_ctor_named_arg_defaulted_sibling_arity_4779,
           loop_50_iterations_spec_22_5) {
    auto r = runAot(
        "module M; "
        "public class C "
        "  public readonly property V: int; "
        "  public constructor(x: int := 1, y: int := 0) "
        "    V := x * 100 + y; "
        "  end constructor; "
        "  public constructor(z: int) "
        "    V := -z; "
        "  end constructor; "
        "end C; "
        "public class D "
        "  public readonly property V: int; "
        "  public constructor(a: int, b: int := 2, c: int := 3) "
        "    V := a * 100 + b * 10 + c; "
        "  end constructor; "
        "  public constructor(p: int, q: int) "
        "    V := -(p + q); "
        "  end constructor; "
        "end D; "
        "public class Program "
        "  public static method Main(): int "
        "    var sx := 0; "
        "    var sy := 0; "
        "    var sd := 0; "
        "    for var i in 0..<50 do "
        "      var c := new C(x := i); "
        "      sx := sx + c.V; "
        "      var e := new C(y := i); "
        "      sy := sy + e.V; "
        "      var d := new D(i, c := 9); "
        "      sd := sd + d.V; "
        "    end; "
        "    Console.WriteLine($\"{sx} {sy} {sd}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // sx = 100 * (0+..+49) = 122500
    // sy = 50*100 + (0+..+49) = 5000 + 1225 = 6225
    // sd = 100 * 1225 + 50 * (2*10 + 9) = 122500 + 1450 = 123950
    BEZEL_EXPECT_EQ(r.stdoutText, "122500 6225 123950\n");
}
