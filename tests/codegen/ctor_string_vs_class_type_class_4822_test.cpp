// fix #4822 (§5.1 / §22.6 stage 3, v0.307): codegen's constructor selector
// `ctorForArgs` (src/codegen/LlvmBackend_impl.hpp) filed `string` under the
// same type class as every Named type, so a class-typed argument "matched" a
// `string` parameter. `new H(sq)` picked `(s: string)` over the applicable
// `(s: IShape, k: int := 2)` — `Sq` does not convert to `string` (§5.1) — and
// read the `Sq` object pointer as a string: silent wrong code (`Hs0`, exit 0).
// The repro is the issue's own (/var/tmp/bezel-scratch/r20/rv-4790/p/i_h.bzl);
// the assertion is on the value.
#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

BEZEL_TEST(codegen_ctor_string_vs_class_type_class_4822,
           class_arg_does_not_select_string_param_spec_5_1_22_6) {
    auto r = runAot(
        "module M; "
        "public interface IShape "
        "  method Name(): string; "
        "end IShape; "
        "public class Sq : IShape "
        "  public method Name(): string return \"sq\"; end Name; "
        "end Sq; "
        "public class H "
        "  public readonly property Tag: string; "
        "  public constructor(s: IShape, k: int := 2) Tag := $\"H{s.Name()},{k}\"; end constructor; "
        "  public constructor(s: string) Tag := $\"Hs{s.Length}\"; end constructor; "
        "end H; "
        "public class Program "
        "  public static method Main(): int "
        "    var sq := new Sq(); "
        "    var h := new H(sq); "
        "    Console.WriteLine(h.Tag); "
        "    var t := new H(\"abc\"); "
        "    Console.WriteLine(t.Tag); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "Hsq,2\nHs3\n");
}

// Control (§5.1 / §14.2 clause 5 / §22.6 stage 5, v0.307): moving `string`
// out of the Named class must not make codegen stricter than sema — a
// `string` argument still reaches an `IComparable<string>` /
// `IEquatable<string>` parameter (now via ctorForArgs' relaxed §5.1 pass),
// and an exact `string` sibling still outranks that conversion.
BEZEL_TEST(codegen_ctor_string_vs_class_type_class_4822,
           string_arg_still_reaches_builtin_iface_param_spec_5_1_22_6) {
    auto r = runAot(
        "module M; "
        "public class G "
        "  public readonly property Tag: string; "
        "  public constructor(v: IComparable<string>, k: int := 3) "
        "    Tag := $\"G{v.CompareTo(\"a\")},{k}\"; "
        "  end constructor; "
        "end G; "
        "public class E "
        "  public readonly property Tag: string; "
        "  public constructor(v: IEquatable<string>) "
        "    Tag := \"EQ\"; "
        "  end constructor; "
        "  public constructor(s: string, k: int := 1) "
        "    Tag := $\"S{s},{k}\"; "
        "  end constructor; "
        "end E; "
        "public class Program "
        "  public static method Main(): int "
        "    var g := new G(\"b\"); "
        "    Console.WriteLine(g.Tag); "
        "    var e := new E(\"x\"); "
        "    Console.WriteLine(e.Tag); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "G1,3\nSx,1\n");
}
