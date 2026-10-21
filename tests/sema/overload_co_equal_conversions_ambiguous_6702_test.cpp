// fix #6702 (§22.6 *Implicit conversions and tie-breaking*, §12.9, §5.1,
// v0.340).
//
// §22.6 (v0.340), verbatim: "An exact match is the only relation that
// distinguishes one applicable candidate from another at an argument
// position; two candidates that both require an implicit conversion at the
// same position are **equally ranked** there, regardless of which conversion
// each uses." And: "If no candidate is unambiguously better — because each of
// two candidates is exact on a different argument, or because the remaining
// candidates all rely on implicit conversions that this rule treats as
// equally ranked — the call is **ambiguous** and is a compile error. The
// compiler reports the error and cites the candidate overloads; it never
// silently selects one. The caller resolves the ambiguity by adding an
// explicit cast ([§5.2]) to an argument so that exactly one candidate becomes
// an exact match".
//
// Every row's argument is a built-in collection instantiation, which leaves
// the §22.6 grid undecided; the undecided arm then selected a candidate
// (first-declared, or the one the by-name ancestor walk reached) instead of
// reporting. `Array<Dog>` converts to `IReadOnlyList<Dog>` (§26.5) and, by
// §12.9 covariance, to `IReadOnlyList<Animal>`; neither is exact. `Object` is
// reached from every class reference (§5.1). Each pair is declared in BOTH
// orders. The controls keep their selection: an argument whose static type
// is exactly one candidate's parameter, and a pair only one candidate of
// which is reachable.
//
// Probes: /var/tmp/bezel-scratch/r34-ovl2rv/p/ (A_*, X_*) and
// /var/tmp/bezel-scratch/r34-ovl3/{p,q,u}/.

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using namespace bezel::test;

namespace {

// `F(x: p1)` returns 1, `F(x: p2)` returns 2; `decl` declares `v`.
std::string prog(const std::string& p1, const std::string& p2,
                 const std::string& decl) {
    return "module M;\n"
           "class Animal public constructor() end; end Animal;\n"
           "class Dog: Animal public constructor() end; end Dog;\n"
           "class MyArg: ArgumentException public constructor(m: string) "
           "base(m); end; end MyArg;\n"
           "static class S\n"
           "  public static method F(x: " + p1 + "): int => 1;\n"
           "  public static method F(x: " + p2 + "): int => 2;\n"
           "end S;\n"
           "class P public static method Main(): int\n"
           "  " + decl + "\n"
           "  Console.WriteLine(S.F(v));\n"
           "  return 0; end Main; end P;\n";
}

std::string prints(const std::string& src) {
    auto r = runAot(src);
    if (!r.ran) return "<did not run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

bool rejectedAmbiguous(const std::string& src) {
    auto r = runAot(src);
    return failedAtCompileTimeWithRealLocation(r) && !r.ran &&
           diagnosticMessageContains(r, "ambiguous call to `F`") &&
           diagnosticMessageContains(r, "no overload is unambiguously better");
}

// Both declaration orders of one pair are ambiguous on `decl`.
bool ambiguousBothOrders(const std::string& p1, const std::string& p2,
                         const std::string& decl) {
    return rejectedAmbiguous(prog(p1, p2, decl)) &&
           rejectedAmbiguous(prog(p2, p1, decl));
}

}  // namespace

// The issue's first row: `F(IReadOnlyList<Animal>)` beside
// `F(IReadOnlyList<Dog>)` on an `Array<Dog>` — both convert.
BEZEL_TEST(overload_co_equal_conversions_ambiguous_6702,
           covariant_and_identity_element_are_co_equal_spec_22_6_12_9) {
    const std::string arr = "using var v := new Array<Dog>();";
    BEZEL_EXPECT_TRUE(ambiguousBothOrders("IReadOnlyList<Animal>",
                                          "IReadOnlyList<Dog>", arr));
    BEZEL_EXPECT_TRUE(ambiguousBothOrders("IEnumerable<Animal>",
                                          "IEnumerable<Dog>", arr));
    BEZEL_EXPECT_TRUE(ambiguousBothOrders("IReadOnlyCollection<Animal>",
                                          "IReadOnlyCollection<Dog>", arr));
    BEZEL_EXPECT_TRUE(ambiguousBothOrders("IEnumerable<Object>",
                                          "IEnumerable<Dog>", arr));
}

// Built-in exception element types (§31.4) and a user subclass of one.
BEZEL_TEST(overload_co_equal_conversions_ambiguous_6702,
           exception_element_types_are_co_equal_spec_22_6_12_9) {
    BEZEL_EXPECT_TRUE(ambiguousBothOrders(
        "IEnumerable<Exception>", "IEnumerable<ArgumentException>",
        "using var v := new Array<ArgumentException>();"));
    BEZEL_EXPECT_TRUE(ambiguousBothOrders(
        "IReadOnlyList<Exception>", "IReadOnlyList<ArgumentException>",
        "using var v := new Array<ArgumentException>();"));
    BEZEL_EXPECT_TRUE(ambiguousBothOrders(
        "IEnumerable<ArgumentException>", "IEnumerable<MyArg>",
        "using var v := new Array<MyArg>();"));
}

// The issue's third row: `F(Object)` beside `F(IEnumerable<int>)` on an
// `Array<int>` selected the second (the `Object` cell read as unreachable).
BEZEL_TEST(overload_co_equal_conversions_ambiguous_6702,
           object_beside_interface_is_ambiguous_spec_22_6_5_1) {
    BEZEL_EXPECT_TRUE(ambiguousBothOrders(
        "Object", "IEnumerable<int>", "using var v := new Array<int>();"));
}

// Two primitive conversions at one position, with a second argument that
// leaves the grid undecided: `F(short, …)` beside `F(byte, …)` on `F(3, a)`
// (§22.6 v0.340: "`M(3)` against `M(byte)` beside `M(short)` is ambiguous").
// Remedy: the cast makes one candidate exact.
BEZEL_TEST(overload_co_equal_conversions_ambiguous_6702,
           two_narrowings_beside_an_undecided_argument_spec_22_6_5_1) {
    auto two = [](const char* first, const char* second, const char* arg) {
        return std::string("module M;\nstatic class S\n"
                           "  public static method F(x: ") +
               first + ", a: Array<int>): int => 1;\n"
                       "  public static method F(x: " +
               second +
               ", a: Array<int>): int => 2;\nend S;\n"
               "class P public static method Main(): int\n"
               "  using var a := new Array<int>();\n"
               "  Console.WriteLine(S.F(" +
               arg + ", a));\n  return 0; end Main; end P;\n";
    };
    BEZEL_EXPECT_TRUE(rejectedAmbiguous(two("short", "byte", "3")));
    BEZEL_EXPECT_TRUE(rejectedAmbiguous(two("byte", "short", "3")));
    BEZEL_EXPECT_EQ(prints(two("short", "byte", "byte(3)")), "2\n");
}

// CONTROLS. "an exact match is the only relation that distinguishes": an
// argument whose static type IS one candidate's parameter selects it in both
// orders, and a pair only one of which the argument reaches selects that one.
// The remedy: a binding of the intended interface type makes it exact.
BEZEL_TEST(overload_co_equal_conversions_ambiguous_6702,
           exact_candidate_and_single_reachable_keep_selection_spec_22_6) {
    const std::string exactArg =
        "using var arr := new Array<ArgumentException>(); "
        "var v: IEnumerable<ArgumentException> := arr;";
    BEZEL_EXPECT_EQ(prints(prog("IEnumerable<Exception>",
                                "IEnumerable<ArgumentException>", exactArg)),
                    "2\n");
    BEZEL_EXPECT_EQ(prints(prog("IEnumerable<ArgumentException>",
                                "IEnumerable<Exception>", exactArg)),
                    "1\n");
    // `IEnumerable<Exception>` does not convert to
    // `IEnumerable<ArgumentException>`: only the first candidate applies.
    BEZEL_EXPECT_EQ(
        prints(prog("IEnumerable<Exception>", "IEnumerable<ArgumentException>",
                    "using var arr := new Array<Exception>(); "
                    "var v: IEnumerable<Exception> := arr;")),
        "1\n");
    const std::string dogList =
        "using var arr := new Array<Dog>(); var v: IReadOnlyList<Dog> := arr;";
    BEZEL_EXPECT_EQ(
        prints(prog("IReadOnlyList<Animal>", "IReadOnlyList<Dog>", dogList)),
        "2\n");
    BEZEL_EXPECT_EQ(
        prints(prog("IReadOnlyList<Dog>", "IReadOnlyList<Animal>", dogList)),
        "1\n");
}
