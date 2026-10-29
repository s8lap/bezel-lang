// Regression for #7715 -- `Console.WriteLine(a)` of a NON-OWNING inline array
// (`var a: [3]int := [1,2,3];`) was an IR verify failure
// ("Call parameter type does not match function signature":
// bz_string_to_utf8z([3 x i32])).
//
// per spec §8.6 *Boxing of inline arrays* (v0.350): "Non-owning inline arrays
// (`[N]T` and `[N]T?`) are ordinary value types and box per the general rule",
// and stdlib §2.1 declares `Console.WriteLine(value: Object)` /
// `Write(value: Object)`, so the argument boxes. Spec §8.5 *ToString* (v0.350):
// "For inline arrays (all forms ...): the type's name -- `"[3]int"`-style for an
// inline array (element-listing output is deliberately not synthesized)". The
// console now renders through the one renderer `a.ToString()` and the §18.5
// hole already used. Owning inline arrays stay rejected at the conversion site
// (§8.6 move-only rule, #7694).

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
const char* kPre =
    "module M; "
    "class Res public constructor() end constructor; end Res; "
    "record Rec(x: int); "
    "class H public F: [2]int := [7, 8]; "
    "  public static method Mk(): [2]int => [4, 5]; end H; "
    "class P public static method Main() ";

std::string out(const std::string& body, bool& ran) {
    auto r = runAot(std::string(kPre) + body + " end Main; end P;");
    ran = r.ran;
    return r.stdoutText;
}
}  // namespace

// The issue's repro, WriteLine and Write.
BEZEL_TEST(console_writeline_inline_array_7715, int_array_writeline_and_write) {
    bool ran = false;
    auto s = out("var a: [3]int := [1, 2, 3]; Console.WriteLine(a); "
                 "Console.Write(a); Console.WriteLine(\"\");", ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_EQ(s, std::string("[3]int\n[3]int\n"));
}

// Element types: string, double, bool, char, a record, a nullable element.
BEZEL_TEST(console_writeline_inline_array_7715, element_type_matrix) {
    bool ran = false;
    auto s = out(
        "var a: [2]string := [\"a\", \"b\"]; Console.WriteLine(a); "
        "var d: [2]double := [1.5, 2.5]; Console.WriteLine(d); "
        "var b: [2]bool := [true, false]; Console.WriteLine(b); "
        "var c: [2]char := ['x', 'y']; Console.WriteLine(c); "
        "var r: [2]Rec := [new Rec(1), new Rec(2)]; Console.WriteLine(r); "
        "var n: [3]int? := [1, null, 3]; Console.WriteLine(n);", ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_EQ(s, std::string(
        "[2]string\n[2]double\n[2]bool\n[2]char\n[2]Rec\n[3]int?\n"));
}

// Console.Error.WriteLine/Write and Console.Out compile and run (stderr is not
// captured by the harness; the pre-fix failure was the compile).
BEZEL_TEST(console_writeline_inline_array_7715, error_and_out_writers) {
    bool ran = false;
    auto s = out("var a: [3]int := [1, 2, 3]; Console.Error.WriteLine(a); "
                 "Console.Error.Write(a); Console.Out.WriteLine(a); "
                 "Console.Out.Write(a);", ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_EQ(s, std::string("[3]int\n[3]int"));
}

// The sibling spellings agree with the console: `$"{a}"` and `a.ToString()`,
// and the expression shapes (field read, call result, Object-typed local).
BEZEL_TEST(console_writeline_inline_array_7715, agrees_with_interpolation_and_tostring) {
    bool ran = false;
    auto s = out("var h := new H(); var a: [3]int := [1, 2, 3]; "
                 "Console.WriteLine($\"{a}\"); Console.WriteLine(a.ToString()); "
                 "Console.WriteLine(h.F); Console.WriteLine(H.Mk()); "
                 "var o: Object := h.F; Console.WriteLine(o);", ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_EQ(s, std::string("[3]int\n[3]int\n[2]int\n[2]int\n[2]int\n"));
}

// Loop the write: no per-call leak (the render is a literal sentinel).
BEZEL_TEST(console_writeline_inline_array_7715, loop_no_leak) {
    bool ran = false;
    auto s = out("var a: [2]int := [1, 2]; "
                 "for var i in 0..<200 do Console.Write(a); end for; "
                 "Console.WriteLine(\"\");", ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_EQ(s.size(), std::size_t(200 * 6 + 1));
}

// Owning inline array stays REJECTED as a boxing (§8.6, #7694).
BEZEL_TEST(console_writeline_inline_array_7715, owning_inline_array_still_rejected) {
    auto r = runAot(std::string(kPre) +
        "var a: [2 owned]Res? := [null, null]; Console.WriteLine(a); end Main; end P;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, "cannot be boxed"));
}
