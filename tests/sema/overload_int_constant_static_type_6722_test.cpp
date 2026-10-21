// fix #6722 (§22.6 *The argument's static type*, §5.1 *Constant narrowing*,
// §4.2, v0.340).
//
// §22.6 (v0.340), verbatim: "A **compile-time integer constant**
// ([§17.14]) is read at its own static type too — `int` for an unsuffixed
// literal that fits `int` ([§4.2]), a `const`'s declared type — and
// [§5.1]'s constant narrowing enters resolution as an *applicability* rule
// only: a candidate the narrowing reaches *converts* at that position, never
// exact, and ranks co-equal with every other conversion. So `Log(42)` against
// `Log(int)` beside `Log(long)` selects `Log(int)`, the identity (Example 1);
// `M(3)` against `M(int)` beside `M(byte)` selects `M(int)`; `M(3)` against
// `M(byte)` beside `M(short)` is ambiguous — both candidates convert —
// remedied as every ambiguity is, by a cast (`M(byte(3))`); and a `ushort`
// constant `3` is exact at `M(ushort)` alone, so against `M(int)` beside
// `M(byte)` it is ambiguous too. A target-typed integer constant ([§4.2]) has
// no static type of its own and, like the `null` literal, is graded alike —
// converting, as above — at every integer candidate whose range holds it:
// `f(3_000_000_000)` against `f(long)` beside `f(ulong)` is ambiguous,
// remedied by a suffix."
//
// The rows are the issue's four recorded coverage gaps, plus the two shapes
// the fix's mechanism reaches: a per-candidate re-type that the NEXT
// candidate's grade inherited (so `Pick(300)` against `Pick(byte)` /
// `Pick(short)` found no applicable candidate in that order), and a call that
// leaves the grid undecided (an `Array<int>` second argument), whose fallback
// grade also read a re-typed cell as exact. Every pair is in BOTH declaration
// orders: the defect was order-dependent, so one order alone proves nothing.
//
// Probes: /var/tmp/bezel-scratch/r34-ovl3/n/ and /u/.

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using namespace bezel::test;

namespace {

// One static `Pick` per comma-separated parameter type, in declaration order,
// each returning its 1-based position, so the PRINTED value names the pick.
std::string pickProg(const std::string& sigs, const std::string& call,
                     const std::string& decl = "var z := 0;") {
    std::string s = "module M;\npublic class C\n";
    int n = 1;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t comma = sigs.find(',', pos);
        const std::string t = sigs.substr(
            pos, comma == std::string::npos ? std::string::npos : comma - pos);
        s += "  public static method Pick(x: " + t + "): int => " +
             std::to_string(n++) + ";\n";
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    s += "  public static method Main(): int\n    " + decl +
         "\n    Console.WriteLine($\"{" + call +
         "}\");\n    return 0;\n  end Main;\nend C;\n";
    return s;
}

std::string prints(const std::string& src) {
    auto r = runAot(src);
    if (!r.ran) return "<did not run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

bool rejectedAmbiguous(const std::string& src, const char* callee = "Pick") {
    auto r = runAot(src);
    const std::string needle =
        std::string("ambiguous call to `") + callee + "`";
    return failedAtCompileTimeWithRealLocation(r) && !r.ran &&
           diagnosticMessageContains(r, needle.c_str());
}

}  // namespace

// "`M(3)` against `M(int)` beside `M(byte)` selects `M(int)`". Before the fix
// `Pick(byte)` declared first was selected (its re-typed cell was Exact).
BEZEL_TEST(overload_int_constant_static_type_6722,
           literal_int_beside_byte_selects_int_both_orders_spec_22_6) {
    BEZEL_EXPECT_EQ(prints(pickProg("byte,int", "C.Pick(3)")), "2\n");
    BEZEL_EXPECT_EQ(prints(pickProg("int,byte", "C.Pick(3)")), "1\n");
    // The `const` spelling: "a `const`'s declared type".
    BEZEL_EXPECT_EQ(
        prints(pickProg("byte,int", "C.Pick(N)", "const N: int := 3;")), "2\n");
}

// "`M(3)` against `M(byte)` beside `M(short)` is ambiguous — both candidates
// convert". Before the fix the first-declared was selected in either order.
BEZEL_TEST(overload_int_constant_static_type_6722,
           literal_byte_beside_short_is_ambiguous_both_orders_spec_22_6) {
    BEZEL_EXPECT_TRUE(rejectedAmbiguous(pickProg("byte,short", "C.Pick(3)")));
    BEZEL_EXPECT_TRUE(rejectedAmbiguous(pickProg("short,byte", "C.Pick(3)")));
    // "remedied ... by a cast (`M(byte(3))`)".
    BEZEL_EXPECT_EQ(prints(pickProg("short,byte", "C.Pick(byte(3))")), "2\n");
}

// "a `ushort` constant `3` is exact at `M(ushort)` alone, so against `M(int)`
// beside `M(byte)` it is ambiguous too". KEPT: already rejected before the
// fix; recorded here as the tracker's coverage gap. The `ushort` candidate
// control is the exact one.
BEZEL_TEST(overload_int_constant_static_type_6722,
           ushort_constant_int_beside_byte_is_ambiguous_kept_spec_22_6) {
    BEZEL_EXPECT_TRUE(rejectedAmbiguous(
        pickProg("int,byte", "C.Pick(N)", "const N: ushort := 3;")));
    BEZEL_EXPECT_TRUE(rejectedAmbiguous(
        pickProg("byte,int", "C.Pick(N)", "const N: ushort := 3;")));
    BEZEL_EXPECT_EQ(prints(pickProg("int,ushort,byte", "C.Pick(N)",
                                    "const N: ushort := 3;")),
                    "2\n");
}

// "`f(3_000_000_000)` against `f(long)` beside `f(ulong)` is ambiguous,
// remedied by a suffix" — and `f(long)` / `f(long?)` likewise (the issue's
// own pair). Before the fix the literal took the first candidate's type and
// that candidate was selected.
BEZEL_TEST(overload_int_constant_static_type_6722,
           target_typed_constant_is_ambiguous_both_orders_spec_22_6_4_2) {
    BEZEL_EXPECT_TRUE(
        rejectedAmbiguous(pickProg("long,ulong", "C.Pick(3_000_000_000)")));
    BEZEL_EXPECT_TRUE(
        rejectedAmbiguous(pickProg("ulong,long", "C.Pick(3_000_000_000)")));
    BEZEL_EXPECT_TRUE(
        rejectedAmbiguous(pickProg("long,long?", "C.Pick(3_000_000_000)")));
    // The suffix remedy: `3_000_000_000L` is a `long`, exact at `Pick(long)`.
    BEZEL_EXPECT_EQ(prints(pickProg("ulong,long", "C.Pick(3_000_000_000L)")),
                    "2\n");
}

// The order defect behind the grade: candidate `c` re-typed the constant from
// candidate `c-1`'s re-type. `300` fits `short` but not `byte`, so only
// `Pick(short)` applies; with `Pick(byte)` declared first the call reported
// "no overload of `Pick` is applicable".
BEZEL_TEST(overload_int_constant_static_type_6722,
           constant_fitting_only_the_second_candidate_applies_spec_5_1_22_6) {
    BEZEL_EXPECT_EQ(prints(pickProg("byte,short", "C.Pick(300)")), "2\n");
    BEZEL_EXPECT_EQ(prints(pickProg("short,byte", "C.Pick(300)")), "1\n");
}

// A second argument the §22.6 grid cannot judge (`Array<int>`) sends the call
// to the undecided fallback, whose grade also read the re-typed `byte` cell
// as exact: `F(byte, …)` beside `F(int, …)` on `F(3, a)` selected `F(byte)`
// when it was declared first.
BEZEL_TEST(overload_int_constant_static_type_6722,
           undecided_grid_int_beside_byte_selects_int_both_orders_spec_22_6) {
    const std::string byteFirst = R"bzl(module M;
static class S
  public static method F(x: byte, a: Array<int>): int => int(x) + 100;
  public static method F(x: int, a: Array<int>): int => 7;
end S;
class P public static method Main(): int
  using var a := new Array<int>();
  Console.WriteLine(S.F(3, a));
  return 0; end Main; end P;
)bzl";
    const std::string intFirst = R"bzl(module M;
static class S
  public static method F(x: int, a: Array<int>): int => 7;
  public static method F(x: byte, a: Array<int>): int => int(x) + 100;
end S;
class P public static method Main(): int
  using var a := new Array<int>();
  Console.WriteLine(S.F(3, a));
  return 0; end Main; end P;
)bzl";
    BEZEL_EXPECT_EQ(prints(byteFirst), "7\n");
    BEZEL_EXPECT_EQ(prints(intFirst), "7\n");
}

// Constructors resolve by the same rule (§22.6: "Methods and constructors
// may be overloaded"). Before the fix `new T(3)` selected `constructor(byte)`
// in BOTH orders (the last candidate's re-type was the shared argument type).
BEZEL_TEST(overload_int_constant_static_type_6722,
           constructor_int_beside_byte_selects_int_both_orders_spec_22_6) {
    auto ctorProg = [](const char* first, const char* second) {
        return std::string("module M;\nclass T\n  public v: int;\n"
                           "  public constructor(x: ") +
               first +
               ") v := 1; end constructor;\n  public constructor(x: " +
               second +
               ") v := 2; end constructor;\nend T;\n"
               "class P public static method Main(): int\n"
               "  var t := new T(3);\n  Console.WriteLine(t.v);\n"
               "  return 0; end Main; end P;\n";
    };
    BEZEL_EXPECT_EQ(prints(ctorProg("byte", "int")), "2\n");
    BEZEL_EXPECT_EQ(prints(ctorProg("int", "byte")), "1\n");
    BEZEL_EXPECT_TRUE(rejectedAmbiguous(ctorProg("byte", "short"), "new T"));
}
