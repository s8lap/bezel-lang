// fix #6656 (§25.2 "Resolution", §6.6 "Compound assignments", v0.339).
//
// §6.6: "`x op= e` is `x := x op e`" with the target evaluated once; §25.1: "the
// compound forms desugar mechanically to read-modify-write using the underlying
// binary operator's overload". §25.2: "A derived-typed operand satisfies a
// base-typed parameter by the ordinary argument rules" (parameters are matched
// "by the ordinary argument rules", §5.1).
//
// THE DEFECT (MEASURED on 15d9937e): `v += r` over `operator +(v: V, p: P)` with
// P a stdlib type, an interface or `Object` passed sema and then died in IR
// verification (a record handed to llvm.sadd.with.overflow), while
// `v := v + r` printed 1001. Codegen's structural re-search matched only
// user-declared Named, primitive and built-in-exception parameters.
//
// Every row prints v.X after the compound; the binary form is the control.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
const char* kDecls =
    "public interface IA end IA; "
    "public class Animal: IA end Animal; "
    "public class Dog: Animal end Dog; ";
std::string local(const std::string& param, const std::string& setup,
                  const std::string& stmt) {
    return std::string("module M; ") + kDecls +
           "record V(X: int) "
           "  public static method operator +(v: V, p: " + param +
           "): V => new V(v.X + 1000); "
           "end V; "
           "public class Program "
           "  public static method Main(): int "
           "    var v := new V(1); " + setup + " " + stmt +
           "    Console.WriteLine(v.X); return 0; "
           "  end Main; "
           "end Program;";
}
}  // namespace

// The issue's literal repro.
BEZEL_TEST(compound_operator_stdlib_operand_6656,
           r1_stringbuilder_into_stringbuilder_spec_6_6) {
    auto r = runAotChecked(local("StringBuilder", "var r := new StringBuilder();",
                                 "v += r; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1001\n");
}

BEZEL_TEST(compound_operator_stdlib_operand_6656,
           r1b_stringbuilder_binary_twin_spec_6_6) {
    auto r = runAot(local("StringBuilder", "var r := new StringBuilder();",
                          "v := v + r; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1001\n");
}

BEZEL_TEST(compound_operator_stdlib_operand_6656,
           r2_array_int_into_idisposable_spec_6_6) {
    auto r = runAotChecked(local("IDisposable", "var r := new Array<int>();",
                                 "v += r; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1001\n");
}

BEZEL_TEST(compound_operator_stdlib_operand_6656,
           r3_dog_into_user_interface_spec_6_6) {
    auto r = runAotChecked(local("IA", "var r := new Dog();", "v += r; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1001\n");
}

BEZEL_TEST(compound_operator_stdlib_operand_6656,
           r4_class_into_object_spec_6_6) {
    auto r = runAotChecked(local("Object", "var r := new Dog();", "v += r; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1001\n");
}

// Two compound statements: the operator runs once per statement, the target is
// read each time (a constant-bodied operator would not see this).
BEZEL_TEST(compound_operator_stdlib_operand_6656,
           r5_array_into_ienumerable_twice_reads_target_spec_6_6) {
    auto r = runAotChecked(local("IEnumerable<int>", "var r := new Array<int>();",
                                 "v += r; v += r; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "2001\n");
}

// A field of a class and an element of an Array<V> as the target.
BEZEL_TEST(compound_operator_stdlib_operand_6656,
           r6_field_target_spec_6_6) {
    auto r = runAotChecked(std::string("module M; ") + kDecls +
        "record V(X: int) "
        "  public static method operator +(v: V, p: StringBuilder): V => "
        "new V(v.X + 1000); "
        "end V; "
        "public class Holder public F: V := new V(1); end Holder; "
        "public class Program "
        "  public static method Main(): int "
        "    var h := new Holder(); var r := new StringBuilder(); "
        "    h.F += r; "
        "    Console.WriteLine(h.F.X); return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1001\n");
}

BEZEL_TEST(compound_operator_stdlib_operand_6656,
           r7_array_element_target_spec_6_6) {
    auto r = runAotChecked(std::string("module M; ") + kDecls +
        "record V(X: int) "
        "  public static method operator +(v: V, p: StringBuilder): V => "
        "new V(v.X + 1000); "
        "end V; "
        "public class Program "
        "  public static method Main(): int "
        "    var a := new Array<V>(1); a.Add(new V(1)); "
        "    var r := new StringBuilder(); "
        "    a[0] += r; "
        "    Console.WriteLine(a[0].X); return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1001\n");
}

// Accessor-mediated targets (block-form property, user indexer) go through
// emitCompoundOpTyped, which has no sema-recorded winner to read.
BEZEL_TEST(compound_operator_stdlib_operand_6656,
           r8_block_property_and_indexer_target_spec_6_6) {
    auto r = runAotChecked(std::string("module M; ") + kDecls +
        "record V(X: int) "
        "  public static method operator +(v: V, p: StringBuilder): V => "
        "new V(v.X + 1000); "
        "end V; "
        "public class Box "
        "  public _p: V := new V(1); "
        "  public property PP: V "
        "    get => do return _p; end; "
        "    set => do _p := value; end; "
        "  end; "
        "  public property [i: int]: V "
        "    get => do return _p; end; "
        "    set => do _p := value; end; "
        "  end; "
        "end Box; "
        "public class Program "
        "  public static method Main(): int "
        "    var b := new Box(); var r := new StringBuilder(); "
        "    b.PP += r; "
        "    Console.WriteLine(b._p.X); "
        "    b[0] += r; "
        "    Console.WriteLine(b._p.X); return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1001\n2001\n");
}

// Property through an interface (the virtual/interface compound arm).
BEZEL_TEST(compound_operator_stdlib_operand_6656,
           r9_interface_property_target_spec_6_6) {
    auto r = runAotChecked(std::string("module M; ") + kDecls +
        "record V(X: int) "
        "  public static method operator +(v: V, p: StringBuilder): V => "
        "new V(v.X + 1000); "
        "end V; "
        "public interface IHold property P: V; end IHold; "
        "public class Hold: IHold "
        "  public _p: V := new V(1); "
        "  public property P: V "
        "    get => do return _p; end; "
        "    set => do _p := value; end; "
        "  end; "
        "end Hold; "
        "public class Program "
        "  public static method Main(): int "
        "    var h: IHold := new Hold(); var r := new StringBuilder(); "
        "    h.P += r; "
        "    Console.WriteLine(h.P.X); return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1001\n");
}
