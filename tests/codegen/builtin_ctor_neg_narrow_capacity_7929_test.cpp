// Spec-derived regression test for issue #7929 (campaign
// builtin-call-admission, lane C2-51): a negative narrow-signed capacity.
//
// ─── THE NORMATIVE SENTENCES (CHECKED in docs/, v0.350) ─────────────────────
//
// stdlib §4.4 / §4.8 / §3.1: `Set<T>(capacity: int)`, `Dictionary<K,V>(capacity:
//   int)` and `StringBuilder(capacity: int)` raise ArgumentOutOfRangeException
//   on a negative capacity.
// spec §5.1: `sbyte` / `short` implicitly widen to `int`, preserving the value,
//   so a `sbyte` -1 IS the capacity -1.
//
// ─── THE DEFECT (MEASURED on r30-c1 26216e9f) ───────────────────────────────
//
// The guards in `lowerNew` fired only for an argument whose type is exactly
// `int`; `new Set<int>(b)` over `b: sbyte := -1` constructed silently.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return "module M; import System; import System.Collections; "
           "import System.Text; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// 2 = raised ArgumentOutOfRangeException, 1 = constructed.
std::string probe(const std::string& decl, const std::string& ctor) {
    return prog("    " + decl + " var ok := 0; "
                "    try var c := " + ctor + "; ok := 1; "
                "    catch ArgumentOutOfRangeException => do ok := 2; end end; "
                "    Console.WriteLine(ok);");
}

}  // namespace

BEZEL_TEST(builtin_ctor_neg_narrow_capacity_7929, set_sbyte) {
    BEZEL_EXPECT_EQ(runAotOut(probe("var b: sbyte := -1;", "new Set<int>(b)")), "2");
}
BEZEL_TEST(builtin_ctor_neg_narrow_capacity_7929, dictionary_sbyte) {
    BEZEL_EXPECT_EQ(runAotOut(probe("var b: sbyte := -1;",
                                    "new Dictionary<int,int>(b)")), "2");
}
BEZEL_TEST(builtin_ctor_neg_narrow_capacity_7929, string_builder_sbyte) {
    BEZEL_EXPECT_EQ(runAotOut(probe("var b: sbyte := -1;", "new StringBuilder(b)")), "2");
}
BEZEL_TEST(builtin_ctor_neg_narrow_capacity_7929, short_forms) {
    BEZEL_EXPECT_EQ(runAotOut(probe("var b: short := -300;", "new Set<int>(b)")), "2");
    BEZEL_EXPECT_EQ(runAotOut(probe("var b: short := -300;",
                                    "new Dictionary<int,int>(b)")), "2");
    BEZEL_EXPECT_EQ(runAotOut(probe("var b: short := -300;", "new StringBuilder(b)")), "2");
}
// Controls: the int -1 form still raises; a non-negative narrow value (and a
// byte 200, which zero-extends) constructs.
BEZEL_TEST(builtin_ctor_neg_narrow_capacity_7929, controls) {
    BEZEL_EXPECT_EQ(runAotOut(probe("var b: int := -1;", "new Set<int>(b)")), "2");
    BEZEL_EXPECT_EQ(runAotOut(probe("var b: sbyte := 5;", "new Set<int>(b)")), "1");
    BEZEL_EXPECT_EQ(runAotOut(probe("var b: byte := 200;",
                                    "new Dictionary<int,int>(b)")), "1");
    BEZEL_EXPECT_EQ(runAotOut(probe("var b: byte := 200;", "new StringBuilder(b)")), "1");
}
