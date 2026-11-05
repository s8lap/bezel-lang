// Pin test for issue #7663 -- a user extension on `Array<int>` / `Stack<int>`
// named after a stdlib §5 LINQ operator OUTRANKS the library's extension on
// `IEnumerable<T>`. spec §26.5.1 "Specificity ordering" (v0.355): "An
// extension on E1 is more specific than one on E2 when E1 is a subtype of E2
// and E2 is not a subtype of E1" -- a class head beats the interface it
// implements; §26.5 step 3 "Most specific type wins".
//
// ALREADY FIXED on staging b52409a7 (verified by lane r39-verify-vB); this
// file only PINS it (no fail-without is available). The conformance corpus
// (s26_5_1_0038/0039) covers only Take, Skip, Where, Select on Array; this
// covers all sixteen names of the issue on BOTH receivers.
//
// ONE program per receiver defines every extension as returning a distinct
// int (101..116) and prints each call, so a name that still reached the
// library would print a different value (or fail to compile).
//
// Oracle: the Stack row is runAotChecked (leak-strict); the Array row is
// runAotCheckedLenient because the verify lane's 144 B leak hypothesis
// REPRODUCES (see the comment at that row).
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

struct Op {
    const char* name;
    const char* param;   // declared parameter list, with %T for the receiver type
    const char* arg;     // call arguments, with %A for the receiver variable
};

const Op kOps[] = {
    {"Where", "p: (int) -> bool", "lambda(x) => x > 1"},
    {"Select", "f: (int) -> int", "lambda(x) => x"},
    {"SelectMany", "f: (int) -> %T", "lambda(x) => %A"},
    {"OrderBy", "k: (int) -> int", "lambda(x) => x"},
    {"OrderByDescending", "k: (int) -> int", "lambda(x) => x"},
    {"Distinct", "", ""},
    {"Union", "o: %T", "%A"},
    {"Intersect", "o: %T", "%A"},
    {"Except", "o: %T", "%A"},
    {"Take", "n: int", "1"},
    {"TakeWhile", "p: (int) -> bool", "lambda(x) => x > 1"},
    {"Skip", "n: int", "1"},
    {"SkipWhile", "p: (int) -> bool", "lambda(x) => x > 1"},
    {"Concat", "o: %T", "%A"},
    {"ToSet", "", ""},
    {"ToDictionary", "k: (int) -> int", "lambda(x) => x"},
};

std::string subst(std::string s, const std::string& T) {
    for (std::string::size_type p; (p = s.find("%T")) != std::string::npos;)
        s.replace(p, 2, T);
    for (std::string::size_type p; (p = s.find("%A")) != std::string::npos;)
        s.replace(p, 2, "a");
    return s;
}

// Returns {program, expected stdout}.
std::pair<std::string, std::string> build(const std::string& recv,
                                          const std::string& init) {
    const std::string T = recv + "<int>";
    std::string src = "module R;\nextension " + T + "\n";
    std::string body, expect;
    int v = 101;
    for (const Op& op : kOps) {
        src += std::string("  public method ") + op.name + "(" +
               subst(op.param, T) + "): int => " + std::to_string(v) + ";\n";
        body += std::string("  Console.WriteLine(a.") + op.name + "(" +
                subst(op.arg, T) + "));\n";
        expect += std::to_string(v) + "\n";
        ++v;
    }
    src += "end;\n"
           "class Program public static method Main(): int\n"
           "  var a: " + T + " := " + init + ";\n" + body +
           "  return 0;\n"
           "end Main; end Program;\n";
    return {src, expect};
}

}  // namespace

BEZEL_TEST(extension_linq_named_user_wins_7663,
           array_receiver_all_sixteen_names_spec_26_5_1_v0_355) {
    auto [src, expect] = build("Array", "[1,2,3]");
    // Lenient: the 144 B leak of the Array literal (80 B header + 64 B
    // payload, LSan "bz_array_alloc") REPRODUCES under runAotChecked on this
    // program (measured at b52409a7): a SEPARATE defect from #7663, not
    // pinned here. The generated-code ASan stays on; only LSan is off.
    auto r = runAotCheckedLenient(src);

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expect);
}

BEZEL_TEST(extension_linq_named_user_wins_7663,
           stack_receiver_all_sixteen_names_spec_26_5_1_v0_355) {
    auto [src, expect] = build("Stack", "new Stack<int>()");
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expect);
}
