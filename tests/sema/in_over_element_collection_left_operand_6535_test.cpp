// #6535 (spec §17.5, §5.1, v0.339): `x in c` IS the call `c.Contains(x)`, so
// over an ELEMENT collection the left operand is admitted exactly when
// `Contains(item: T)` admits it. Before the fix `1.5 in Array<int>` died in LLVM
// IR verification ("Both operands to ICmp instruction are not of the same
// type") and `1.5 in Set<int>` printed `False` at exit 0.
//
// Control rows (compile and run on both sides of the fix) pin that the legal
// spellings stay admitted: same-type, widening, constant narrowing, a nullable
// element type, and the Range receiver.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {
bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}
const char* kSite = "has a type that is not implicitly convertible to the "
                    "target type (\xC2\xA7""5.1)";

std::string prog(const std::string& decl, const std::string& expr) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    " + decl + "\n"
           "    Console.WriteLine((" + expr + ").ToString());\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}
}  // namespace

BEZEL_TEST(in_over_element_collection_6535,
           r1_array_int_double_left_rejected_spec_5_1) {
    auto r = runAot(prog("var a: Array<int> := [1, 2];", "1.5 in a"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, std::string("left operand of `in` over `Array`") + " " + kSite));
}

BEZEL_TEST(in_over_element_collection_6535,
           r2_set_int_double_left_rejected_spec_5_1) {
    auto r = runAot(prog("var s: Set<int> := [1, 2];", "1.5 in s"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, std::string("left operand of `in` over `Set`") + " " + kSite));
}

BEZEL_TEST(in_over_element_collection_6535,
           r3_array_string_int_left_rejected_spec_5_1) {
    auto r = runAot(prog("var a: Array<string> := [\"a\"];", "1 in a"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "left operand of `in` over `Array`"));
}

BEZEL_TEST(in_over_element_collection_6535,
           c1_array_int_int_left_admitted) {
    auto r = runAot(prog("var a: Array<int> := [1, 2];", "2 in a"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}

BEZEL_TEST(in_over_element_collection_6535,
           c2_array_long_int_left_widening_admitted) {
    auto r = runAot(prog("var a: Array<long> := [1, 2]; var i: int := 2;", "i in a"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}

BEZEL_TEST(in_over_element_collection_6535,
           c3_set_byte_constant_narrowing_admitted) {
    auto r = runAot(prog("var s: Set<byte> := [1, 2];", "3 in s"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "False\n");
}

BEZEL_TEST(in_over_element_collection_6535,
           c4_array_nullable_element_admits_int_left) {
    auto r = runAot(prog("var a: Array<int?> := [1, null];", "1 in a"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}

BEZEL_TEST(in_over_element_collection_6535,
           c5_range_receiver_unchanged) {
    auto r = runAot(prog("var i: int := 3;", "i in 1 .. 5"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}
