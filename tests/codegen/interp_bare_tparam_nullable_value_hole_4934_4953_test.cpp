// Regression tests for issues #4934 (render half) and #4953 — §18.5
// "Nullable holes" at a hole whose sema type is a BARE type parameter `T`
// inside a generic body instantiated at a nullable VALUE type (`T := int?`).
//
// The hole lowers to the tagged `{i8, T}` carrier, but the interpolation
// lowering read the hole's sema type (`T`, unsubstituted) to pick a render
// arm. No arm matched a bare `T`, so the carrier was dropped and the hole
// rendered the empty string:
//   * `Util.Show<int?>(null)` / `Util.Show<int?>(5)` with
//     `Show<T>(v: T): string => $"<{v}>"` printed "<>" twice (#4934);
//   * `new B<int?>(41).Show()` with `Show(): string => $"{_v}"` over a
//     `_v: T` field printed an empty line (#4953).
//
// spec §18.5 "Nullable holes" ("When the value is `null` at render time, the
// hole renders as the four-character text `null`"; "The alignment applies to
// the rendered text `null` exactly as to any other rendered text"), §8.5,
// §14.1 (monomorphization substitutes the supplied type argument), v0.307.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

void expectPrints4934(const std::string& src, const std::string& expected) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// per §18.5 / §14.1 (v0.307), fix #4934 — the issue's no-lambda repro: a
// generic `Show<T>` rendering a `T` hole at `T := int?`, null and present.
BEZEL_TEST(interp_bare_tparam_nullable_4934, issue_show_int_nullable_null_and_value_18_5) {
    expectPrints4934(
        "module Conf; "
        "public class Util "
        "  public static method Show<T>(v: T): string "
        "    return $\"<{v}>\"; "
        "  end Show; "
        "end Util; "
        "public class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(Util.Show<int?>(null)); "
        "    Console.WriteLine(Util.Show<int?>(5)); "
        "    return 0; "
        "  end Main; "
        "end Program;",
        "<null>\n<5>\n");
}

// per §18.5 / §14.1 (v0.307), fix #4934 — other nullable value payloads
// (`long?` negative, `bool?`, `double?`), the expression-bodied form, the
// alignment-only hole (the alignment pads the text `null` exactly as a present
// value), and a non-nullable `T := int` control.
BEZEL_TEST(interp_bare_tparam_nullable_4934, payloads_expr_body_alignment_and_control_18_5) {
    expectPrints4934(
        "module Conf; "
        "public class Util "
        "  public static method Show<T>(v: T): string => $\"<{v}>\"; "
        "  public static method Pad<T>(v: T): string => $\"[{v,6}]\"; "
        "end Util; "
        "public class Program "
        "  public static method Main(): int "
        "    var n: long? := -9; "
        "    Console.WriteLine(Util.Show<long?>(n)); "
        "    Console.WriteLine(Util.Show<bool?>(true)); "
        "    Console.WriteLine(Util.Show<bool?>(null)); "
        "    Console.WriteLine(Util.Show<double?>(null)); "
        "    Console.WriteLine(Util.Pad<int?>(5)); "
        "    Console.WriteLine(Util.Pad<int?>(null)); "
        "    Console.WriteLine(Util.Show<int>(7)); "
        "    return 0; "
        "  end Main; "
        "end Program;",
        "<-9>\n<True>\n<null>\n<null>\n[     5]\n[  null]\n<7>\n");
}

// per §18.5 / §14.1 (v0.307), fix #4953 — the issue's repro: a `T` field of a
// generic class at `T := int?`, rendered from a method and from a property;
// a null field; and the `B<bool>` control.
BEZEL_TEST(interp_bare_tparam_nullable_4953, issue_generic_class_field_hole_18_5) {
    expectPrints4934(
        "module M; "
        "public class B<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public method Show(): string => $\"{_v}\"; "
        "  public property S: string => $\"({_v})\"; "
        "end B; "
        "public class Program "
        "  public static method Main(): int "
        "    using var a := new B<int?>(41); "
        "    Console.WriteLine(a.Show()); "
        "    Console.WriteLine(a.S); "
        "    using var z := new B<int?>(null); "
        "    Console.WriteLine(z.Show()); "
        "    using var b := new B<bool>(true); "
        "    Console.WriteLine(b.S); "
        "    return 0; "
        "  end Main; "
        "end Program;",
        "41\n(41)\nnull\n(True)\n");
}

// per §18.5 / §14.1 (v0.307), fix #4934 / #4953 — 50 renders in a loop,
// alternating null and present; the concatenated length pins every hole
// (each present render is the decimal of i, each null render is "null").
BEZEL_TEST(interp_bare_tparam_nullable_4934, loop_50_null_and_present_18_5) {
    expectPrints4934(
        "module Conf; "
        "public class Util "
        "  public static method Show<T>(v: T): string => $\"{v}\"; "
        "end Util; "
        "public class Program "
        "  public static method Main(): int "
        "    var total: int := 0; var nulls: int := 0; "
        "    for var i in 0..<50 do "
        "      var x: int? := null; "
        "      if i mod 2 = 0 then x := i; end; "
        "      var s := Util.Show<int?>(x); "
        "      if s = \"null\" then nulls := nulls + 1; end; "
        "      total := total + s.Length; "
        "    end; "
        "    Console.WriteLine(nulls); Console.WriteLine(total); "
        "    return 0; "
        "  end Main; "
        "end Program;",
        "25\n145\n");
}
