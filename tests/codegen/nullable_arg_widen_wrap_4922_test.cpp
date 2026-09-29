// Regression tests for issue #4922 — §5.1 "Nullable conversions" (`S → T → T?`)
// at a CALL ARGUMENT: an `int` argument bound to a `long?` (or `double?`,
// `ulong?`) formal.
//
// `buildOrderedArgs` wrapped a plain argument into the tagged `{i8, T}`
// nullable struct ONLY when its LLVM type was exactly the payload type, so an
// `int` at a `long?` formal escaped unwrapped. At a `new` the ctor call then
// mismatched its signature, the ctor body was inlined with the formal bound in
// a scalar slot, and `a ?? -1` ran `extractvalue` on an i64 — `bezelc` died
// with SIGSEGV (rc 139, no diagnostic). At a method call the per-argument
// width coercion widened to i64 but never wrapped: IR verification failed
// ("Call parameter type does not match function signature!"). The omitted-
// default fill (`c: long? := 7`) had the same gap.
//
// spec §5.1 "Implicit conversions" (int → long / double, uint → ulong) and
// "Nullable conversions" (`S` converts to `T?` via `S → T → T?`), §22.5
// (default-value arguments), §22.6 stage 3 (an applicable argument's implicit
// conversion is applied), v0.307.

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

// per §5.1 / §22.6 (v0.307), fix #4922 — the issue's repro verbatim:
// `new J(5, 2)` at `(a: long?, b: int? := null)`, `{a ?? -1}`.
BEZEL_TEST(nullable_arg_widen_wrap_4922, issue_repro_ctor_int_at_long_nullable) {
    expectPrints(
        "module M; "
        "class J "
        "  public readonly property Tag: string; "
        "  public constructor(a: long?, b: int? := null) Tag := $\"N{a ?? -1}\"; "
        "  end constructor; "
        "end J; "
        "class P "
        "  public static method Main(): int "
        "    using var k := new J(5, 2); Console.WriteLine(k.Tag); return 0; "
        "  end Main; "
        "end P;",
        "N5\n");
}

// per §5.1 (v0.307), fix #4922 — the issue's `{b ?? -1}` variant (the
// exact-payload `int` → `int?` wrap, which compiled pre-fix), with `a` still
// an `int` at `long?`.
BEZEL_TEST(nullable_arg_widen_wrap_4922, issue_variant_coalesce_b) {
    expectPrints(
        "module M; "
        "class J "
        "  public readonly property Tag: string; "
        "  public constructor(a: long?, b: int? := null) "
        "    Tag := $\"N{b ?? -1}/{a ?? -1}\"; end constructor; "
        "end J; "
        "class P "
        "  public static method Main(): int "
        "    using var k := new J(5, 2); Console.WriteLine(k.Tag); "
        "    using var k2 := new J(6); Console.WriteLine(k2.Tag); return 0; "
        "  end Main; "
        "end P;",
        "N2/5\nN-1/6\n");
}

// per §5.1 / §22.5 (v0.307), fix #4922 — an `int` LOCAL at a `long?` formal
// with a `null` default; an explicit `null`; and an omitted `long? := 7`
// default (the fillOmittedParamDefaults twin).
BEZEL_TEST(nullable_arg_widen_wrap_4922, ctor_int_local_at_long_nullable_null_default) {
    expectPrints(
        "module M; "
        "class J "
        "  public readonly property Tag: string; "
        "  public constructor(a: long?, b: long? := null, c: long? := 7) "
        "    Tag := $\"{a ?? -1}/{b ?? -1}/{c ?? -1}\"; end constructor; "
        "end J; "
        "class P "
        "  public static method Main(): int "
        "    var x: int := 5; "
        "    using var k := new J(x); Console.WriteLine(k.Tag); "
        "    using var k2 := new J(null, x); Console.WriteLine(k2.Tag); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "5/-1/7\n-1/5/7\n");
}

// per §5.1 / §22.5 (v0.307), fix #4922 — the method-call sibling (pre-fix an
// IR verification failure): `F(5)` at `long?`, an omitted `long? := 7`
// default, a `short` local, `uint` → `ulong?` (zero-extended) and
// `int`/`float` → `double?`.
BEZEL_TEST(nullable_arg_widen_wrap_4922, method_int_at_long_nullable_and_default) {
    expectPrints(
        "module M; "
        "class P "
        "  public static method F(a: long?): long return a ?? -1; end F; "
        "  public static method H(c: long? := 7): long return c ?? -1; end H; "
        "  public static method G(x: double?, y: ulong?): string "
        "    return $\"{x ?? 0.5}/{y ?? 9}\"; "
        "  end G; "
        "  public static method Main(): int "
        "    var s: short := -3; var u: uint := 4000000000; var f: float := 2; "
        "    Console.WriteLine(F(5)); Console.WriteLine(H()); "
        "    Console.WriteLine(H(s)); Console.WriteLine(G(3, u)); "
        "    Console.WriteLine(G(f, 7)); Console.WriteLine(G(null, null)); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "5\n7\n-3\n3/4000000000\n2/7\n0.5/9\n");
}

// per §5.1 / §28.3 (v0.307), fix #4922 — 50 constructions in a loop, each
// with an `int` at the `long?` formal and an interpolated (heap) `Tag`; the
// running sum pins every wrapped value.
BEZEL_TEST(nullable_arg_widen_wrap_4922, ctor_loop_50_iterations) {
    expectPrints(
        "module M; "
        "class J "
        "  public readonly property Tag: string; "
        "  public readonly property V: long; "
        "  public constructor(a: long?, b: int? := null) "
        "    Tag := $\"N{a ?? -1}\"; V := a ?? -1; end constructor; "
        "end J; "
        "class P "
        "  public static method Main(): int "
        "    var sum: long := 0; var len: int := 0; "
        "    for var i in 0..<50 do "
        "      using var k := new J(i, 2); sum := sum + k.V; "
        "      len := len + k.Tag.Length; "
        "    end; "
        "    Console.WriteLine($\"{sum}/{len}\"); return 0; "
        "  end Main; "
        "end P;",
        "1225/140\n");
}
