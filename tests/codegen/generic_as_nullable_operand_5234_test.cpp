// Issue #5234 (§12.7 "Value-typed operands", v0.313): in a generic body,
// `v as T` over `v: T?` instantiated at a value type (`T = int`) is the
// own-type case over the nullable form: the result is the operand itself (no
// second wrap as `T?`), null staying null. Pre-fix the concrete-value arm of
// lowerTypeTest wrapped the already-nullable `{i8,i32}` operand into a
// `{i8,i32}` again ("Invalid InsertValueInst operands", IR verification
// failure) because resolving the bare parameter `T` dropped the operand's `?`.
// A mismatched substitution answers null; stdout / exit code are the oracles.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// G.Show<T,..> prints the `as` result; each row supplies the call sites.
std::string prog(const std::string& generics, const std::string& body) {
    return
        "module M;\n"
        "record R(X: int, Y: int);\n"
        "enum Col Red, Green, Blue; end Col;\n"
        "class G\n" + generics +
        "end G;\n"
        "class C\n"
        "  public static method Main(): int\n" + body +
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

void expectOut(const std::string& generics, const std::string& body,
               const char* want) {
    JitResult r = runAot(prog(generics, body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(want));
}

}  // namespace

// The issue's repro verbatim: exit 1.
BEZEL_TEST(generic_as_nullable_operand_5234, issue_repro_exit_1_spec_12_7_v0_313) {
    JitResult r = runAot(
        "module M;\n"
        "class G\n"
        "  public static method Mk<T>(v: T?): bool\n"
        "    var q := v as T;\n"
        "    return q <> null;\n"
        "  end Mk;\n"
        "end G;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var p: int? := 5;\n"
        "    if G.Mk<int>(p) then return 1; end if;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// Null operand: stays null (the issue repro's null counterpart).
BEZEL_TEST(generic_as_nullable_operand_5234, null_operand_stays_null_spec_12_7_v0_313) {
    JitResult r = runAot(
        "module M;\n"
        "class G\n"
        "  public static method Mk<T>(v: T?): bool\n"
        "    var q := v as T;\n"
        "    return q <> null;\n"
        "  end Mk;\n"
        "end G;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var p: int? := null;\n"
        "    if G.Mk<int>(p) then return 1; end if;\n"
        "    return 2;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}

// The value is carried through unchanged (not just non-null). The generic
// returns the `as` result as `T?`; the caller prints it.
BEZEL_TEST(generic_as_nullable_operand_5234, int_value_preserved_and_null_spec_12_7_v0_313) {
    expectOut(
        "  public static method Id<T>(v: T?): T?\n"
        "    var q := v as T;\n"
        "    return q;\n"
        "  end Id;\n",
        "    var p: int? := 5;\n"
        "    var a := G.Id<int>(p);\n"
        "    if a <> null then Console.WriteLine(a); else Console.WriteLine(\"null\"); end;\n"
        "    var n: int? := null;\n"
        "    var b := G.Id<int>(n);\n"
        "    if b <> null then Console.WriteLine(b); else Console.WriteLine(\"null\"); end;\n",
        "5\nnull\n");
}

// Record and enum instantiations (value types, own type).
BEZEL_TEST(generic_as_nullable_operand_5234, record_and_enum_spec_12_7_v0_313) {
    expectOut(
        "  public static method Id<T>(v: T?): T?\n"
        "    var q := v as T;\n"
        "    return q;\n"
        "  end Id;\n",
        "    var r: R? := new R(3, 4);\n"
        "    var a := G.Id<R>(r);\n"
        "    if a <> null then Console.WriteLine(\"has\"); else Console.WriteLine(\"null\"); end;\n"
        "    var rn: R? := null;\n"
        "    var b := G.Id<R>(rn);\n"
        "    if b <> null then Console.WriteLine(\"has\"); else Console.WriteLine(\"null\"); end;\n"
        "    var e: Col? := Col.Green;\n"
        "    var c := G.Id<Col>(e);\n"
        "    if c <> null then Console.WriteLine(\"has\"); else Console.WriteLine(\"null\"); end;\n"
        "    var en: Col? := null;\n"
        "    var d := G.Id<Col>(en);\n"
        "    if d <> null then Console.WriteLine(\"has\"); else Console.WriteLine(\"null\"); end;\n",
        "has\nnull\nhas\nnull\n");
}

// Concrete operand `int?` with a type-parameter target: own type at T=int,
// a different type at T=long answers null.
BEZEL_TEST(generic_as_nullable_operand_5234, control_concrete_nullable_operand_param_target_spec_12_7_v0_313) {
    expectOut(
        "  public static method Has<T>(v: int?): bool\n"
        "    var q := v as T;\n"
        "    return q <> null;\n"
        "  end Has;\n",
        "    var p: int? := 5;\n"
        "    Console.WriteLine(G.Has<int>(p));\n"
        "    Console.WriteLine(G.Has<long>(p));\n",
        "True\nFalse\n");
}

// Concrete non-nullable operand with a type-parameter target: match wraps,
// mismatch is null.
BEZEL_TEST(generic_as_nullable_operand_5234, control_concrete_plain_operand_param_target_spec_12_7_v0_313) {
    expectOut(
        "  public static method Has<T>(v: int): bool\n"
        "    var q := v as T;\n"
        "    return q <> null;\n"
        "  end Has;\n",
        "    Console.WriteLine(G.Has<int>(7));\n"
        "    Console.WriteLine(G.Has<long>(7));\n",
        "True\nFalse\n");
}

// Same root cause for `is`: the dropped `?` made `v is T` a static `true` even
// for a null operand (pre-fix printed yes/yes); the null test must answer no.
BEZEL_TEST(generic_as_nullable_operand_5234, is_over_nullable_param_null_test_spec_12_7_v0_313) {
    expectOut(
        "  public static method Show<T>(v: T?)\n"
        "    if v is T then Console.WriteLine(\"yes\"); else Console.WriteLine(\"no\"); end;\n"
        "  end Show;\n",
        "    var p: int? := 5;\n"
        "    G.Show<int>(p);\n"
        "    var n: int? := null;\n"
        "    G.Show<int>(n);\n",
        "yes\nno\n");
}
