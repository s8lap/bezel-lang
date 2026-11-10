// Codegen/sema — spec §5.1 (reference conversion to an implemented interface)
// / §14.2 / stdlib §2.6 (v0.357) / #8288: `Guid` is declared
// `IComparable<Guid>, IEquatable<Guid>`, so a Guid value converts to those
// interfaces at its own type, and the boxed Guid DISPATCHES (the 16-byte
// canonical order, `bz_guid_compare`).
//
// THE DEFECT (MEASURED on the parent): `var g: IComparable<Guid> :=
// Guid.Empty;` was refused "not implicitly convertible to the target type
// (§5.1)" (control: `IComparable<int> := 5` is accepted).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

// The issue's literal repro: prints 0.
BEZEL_TEST(codegen_guid_boxed_icomparable_8288,
           issue_repro_guid_empty_converts_to_icomparable_guid) {
    JitResult r = runAotChecked(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var g: IComparable<Guid> := Guid.Empty;\n"
        "    Console.WriteLine(g.CompareTo(Guid.Empty));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "0\n");
}

// Order (stdlib §2.6: lexicographic over the 16 canonical bytes) and equality
// through both interfaces.
BEZEL_TEST(codegen_guid_boxed_icomparable_8288,
           boxed_guid_dispatches_compare_to_and_equals) {
    JitResult r = runAotChecked(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var lo := Guid.Parse(\"00000001-0000-0000-0000-000000000000\");\n"
        "    var hi := Guid.Parse(\"22222222-2222-2222-2222-222222222222\");\n"
        "    var lo2 := Guid.Parse(\"00000001-0000-0000-0000-000000000000\");\n"
        "    var a: IComparable<Guid> := lo;\n"
        "    var b: IComparable<Guid> := hi;\n"
        "    Console.WriteLine(a.CompareTo(hi));\n"
        "    Console.WriteLine(b.CompareTo(lo));\n"
        "    Console.WriteLine(a.CompareTo(lo2));\n"
        "    var e: IEquatable<Guid> := lo;\n"
        "    Console.WriteLine(e.Equals(lo2));\n"
        "    Console.WriteLine(e.Equals(hi));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "-1\n1\n0\nTrue\nFalse\n");
}

// A Guid never converts to the interface at another type argument.
BEZEL_TEST(codegen_guid_boxed_icomparable_8288,
           guid_does_not_convert_to_icomparable_of_another_type) {
    JitResult r = runAotChecked(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var g: IComparable<int> := Guid.Empty;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
}
