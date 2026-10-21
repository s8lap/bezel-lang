// Codegen / float and double keys of GroupBy, Distinct and ToSet — issue #6657.
// Spec: language spec §4.3.1 (v0.339): `NaN = NaN` is true and `-0.0 = +0.0`
// is true, so floating-point `=` is reflexive; stdlib §5 GroupBy / Distinct /
// ToSet group, dedup and collect by the key type's value equality
// (Bezel_Standard_Library.md, v0.339).
//
// THE BUG. bz_groupby_to_array kept a byte compare for float/double keys
// (#6532 carved them out), so 0.0 and -0.0, and NaNs of different payload,
// split into separate groups (5 groups for the 5 values below, expected 3).
// Distinct and ToSet over double/float did not compile at all: the key-equality
// trampoline (buildLinqTrampoline kind 3) fell through to an integer ICmp on a
// floating-point operand, a codegen IR-verification abort.
//
// Values (built with BitConverter, stdlib §2.12): 0.0, -0.0, NaN, a NaN with a
// different payload, 1.5. Expected: 3 groups, in first-occurrence order
// (0.0 x2, NaN x2, 1.5 x1).
//
// Oracle: runAot, stdout pinned byte-for-byte.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

std::string prog(const std::string& T, const std::string& mk0,
                 const std::string& mkNegZero, const std::string& mkNan,
                 const std::string& mkNanP, const std::string& mkOne,
                 const std::string& body) {
    return "module M; import System.Collections.Linq; "
           "record R(x: double, n: int); "
           "class C "
           "  public static method Main(): int "
           "    using var a := new Array<" + T + ">(); "
           "    a.Add(" + mk0 + "); a.Add(" + mkNegZero + "); "
           "    a.Add(" + mkNan + "); a.Add(" + mkNanP + "); a.Add(" + mkOne + "); " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

std::string d(const std::string& bits) {
    return "BitConverter.Int64BitsToDouble(" + bits + ")";
}
std::string f(const std::string& bits) {
    return "BitConverter.Int32BitsToFloat(" + bits + ")";
}

const char* kGroups =
    "    for var g in a.GroupBy(lambda(v) => v) do "
    "      Console.WriteLine(\"g \" + g.Count().ToString()); "
    "    end; ";
const char* kDistinctSet =
    "    var dc := 0; for var v in a.Distinct() do dc := dc + 1; end; "
    "    using var ts := a.ToSet(); "
    "    Console.WriteLine(\"distinct \" + dc.ToString() + \" toset \" + ts.Count.ToString()); ";

std::string dbl(const char* body, const std::string& T = "double") {
    return prog(T, d("0L"), d("-9223372036854775808L"),
                d("9221120237041090560L"), d("9221120237041090851L"),
                d("4609434218613702656L"), body);
}

}  // namespace

BEZEL_TEST(float_key_reflexive_equality_6657, groupby_double_merges_zeros_and_nans_spec_4_3_1) {
    JitResult r = runAot(dbl(kGroups));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "g 2\ng 2\ng 1\n");
}

BEZEL_TEST(float_key_reflexive_equality_6657, groupby_float_merges_zeros_and_nans_spec_4_3_1) {
    JitResult r = runAot(prog("float", f("0"), f("-2147483648"), f("2143289344"),
                              f("2143289635"), f("1069547520"), kGroups));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "g 2\ng 2\ng 1\n");
}

BEZEL_TEST(float_key_reflexive_equality_6657, groupby_nullable_double_and_record_field_stay_merged) {
    JitResult r = runAot(dbl(kGroups, "double?"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "g 2\ng 2\ng 1\n");
    JitResult q = runAot(prog("R", "new R(" + d("0L") + ", 7)",
                              "new R(" + d("-9223372036854775808L") + ", 7)",
                              "new R(" + d("9221120237041090560L") + ", 7)",
                              "new R(" + d("9221120237041090851L") + ", 7)",
                              "new R(" + d("4609434218613702656L") + ", 7)", kGroups));
    BEZEL_EXPECT_TRUE(q.ran);
    BEZEL_EXPECT_EQ(q.stdoutText, "g 2\ng 2\ng 1\n");
}

BEZEL_TEST(float_key_reflexive_equality_6657, distinct_and_toset_double_compile_and_dedup_spec_4_3_1) {
    JitResult r = runAot(dbl(kDistinctSet));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "distinct 3 toset 3\n");
}

BEZEL_TEST(float_key_reflexive_equality_6657, distinct_and_toset_float_compile_and_dedup_spec_4_3_1) {
    JitResult r = runAot(prog("float", f("0"), f("-2147483648"), f("2143289344"),
                              f("2143289635"), f("1069547520"), kDistinctSet));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "distinct 3 toset 3\n");
}
