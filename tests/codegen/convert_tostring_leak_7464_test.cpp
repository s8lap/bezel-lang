// Fix #7464 -- `Convert.ToString` leaked its result (stdlib §2.3, spec §28.9,
// v0.350).
//
// THE DEFECT. stdlib §2.3 declares `Convert.ToString(value): string` for
// `int` / `long` / `float` / `double` / `decimal` / `bool` and
// `Convert.ToString(value, toBase): string` for `int` / `long`. Every form
// lowers to a FRESH rc := 1 `bz.string` the caller owns (§28.9: a value the
// compiler owns is disposed exactly once). `isPlusOneStringProducer` did not
// recognize the `Convert.ToString` call, so every consumer (a console write, a
// local bind, a concat operand, a call argument) treated the result as a
// BORROW and never discharged the producer's +1: `Console.WriteLine(
// Convert.ToString(7))` leaked 34 bytes in 2 allocations per call.
//
// THE FIX. The predicate recognizes a `Convert.ToString(...)` call (one or two
// arguments) on the BUILTIN `Convert`. (A user type cannot shadow it with a
// `ToString` of its own: a static member named `ToString` is blocked at any
// parameter list, §12.2, so the receiver test needs no user-type control.)
//
// ORACLE. `runAotChecked` (generated-code ASan + LSan); every program loops
// 100x so a one-shot LSan blind spot cannot hide the leak, and every row
// asserts the program ran and its exact stdout.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

std::string prog(const std::string& body) {
    return std::string(
               "module M; import System;\nclass C\n"
               "  public static method IdI(x: int?): int? return x; end IdI;\n"
               "  public static method IdL(x: long?): long? return x; end IdL;\n"
               "  public static method Len(s: string): int => s.Length;\n"
               "  public static method Main(): int\n"
               "    var i := 0;\n"
               "    var n := 0;\n") +
           body + "\n    Console.WriteLine(n);\n    return 0;\n  end Main;\nend C;\n";
}

std::string stdoutOf(const JitResult& r) {
    std::string t = r.stdoutText;
    if (!t.empty() && t.back() == '\n') t.pop_back();
    return t;
}

// The loop wrapper: 100 iterations of `inner`.
std::string loop(const std::string& inner) {
    return "    while i < 100 do\n" + inner + "\n      i := i + 1;\n    end while;";
}

}  // namespace

// stdlib §2.3 -- the issue's repro: a plain `int` through the console.
BEZEL_TEST(convert_tostring_leak_7464, console_int_no_leak_spec_28_9) {
    JitResult r = runAotChecked(prog(loop(
        "      var v := 7;\n"
        "      Console.WriteLine(Convert.ToString(v));\n"
        "      n := n + 1;")));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::string expect;
    for (int k = 0; k < 100; ++k) expect += "7\n";
    expect += "100";
    BEZEL_EXPECT_EQ(stdoutOf(r), expect);
}

// §7.4 / §22.6 -- the narrowed `int?` form (the issue's 32-byte residual).
BEZEL_TEST(convert_tostring_leak_7464, narrowed_int_bound_local_no_leak_spec_28_9) {
    JitResult r = runAotChecked(prog(loop(
        "      var x: int? := C.IdI(42);\n"
        "      if x <> null then\n"
        "        var s := Convert.ToString(x);\n"
        "        n := n + s.Length;\n"
        "      end;")));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r), "200");
}

// §2.3 -- every one-argument overload: long, float, double, decimal, bool.
BEZEL_TEST(convert_tostring_leak_7464, every_one_arg_overload_no_leak_spec_28_9) {
    JitResult r = runAotChecked(prog(loop(
        "      var a := Convert.ToString(123456789012L);\n"
        "      var b := Convert.ToString(1.5f);\n"
        "      var c := Convert.ToString(2.25);\n"
        "      var d := Convert.ToString(3.5m);\n"
        "      var e := Convert.ToString(true);\n"
        "      n := n + a.Length + b.Length + c.Length + d.Length + e.Length;")));
    BEZEL_EXPECT_TRUE(r.ran);
    // 12 + 3 + 4 + 3 + 4 = 26 per iteration.
    BEZEL_EXPECT_EQ(stdoutOf(r), "2600");
}

// §2.3 -- the radix form on `int` and `long` (the 32-byte residual).
BEZEL_TEST(convert_tostring_leak_7464, radix_form_no_leak_spec_28_9) {
    JitResult r = runAotChecked(prog(loop(
        "      Console.WriteLine(Convert.ToString(255, 16));\n"
        "      var s := Convert.ToString(-255L, 2);\n"
        "      n := n + s.Length;")));
    BEZEL_EXPECT_TRUE(r.ran);
    std::string expect;
    for (int k = 0; k < 100; ++k) expect += "ff\n";
    expect += "900";
    BEZEL_EXPECT_EQ(stdoutOf(r), expect);
}

// §7.4 -- the narrowed `long?` value at the radix form.
BEZEL_TEST(convert_tostring_leak_7464, radix_form_narrowed_long_no_leak_spec_28_9) {
    JitResult r = runAotChecked(prog(loop(
        "      var l: long? := C.IdL(-255);\n"
        "      var b: int? := C.IdI(16);\n"
        "      if l <> null and b <> null then\n"
        "        Console.WriteLine(Convert.ToString(l, b));\n"
        "      end;")));
    BEZEL_EXPECT_TRUE(r.ran);
    std::string expect;
    for (int k = 0; k < 100; ++k) expect += "-ff\n";
    expect += "0";
    BEZEL_EXPECT_EQ(stdoutOf(r), expect);
}

// §28.3 -- the result as a concat operand and as a borrowed call argument
// (each consumer discharges the unbound temporary exactly once: no double
// free, no leak).
BEZEL_TEST(convert_tostring_leak_7464, concat_operand_and_call_argument_no_leak_spec_28_3) {
    JitResult r = runAotChecked(prog(loop(
        "      var t := \"v=\" + Convert.ToString(i);\n"
        "      n := n + t.Length + C.Len(Convert.ToString(i));")));
    BEZEL_EXPECT_TRUE(r.ran);
    // i in 0..99: "v=" + digits (2 + d) and Len(digits) (d): sum over i of
    // 2 + 2d, d = 1 for 0..9 (10 values), 2 for 10..99 (90 values).
    // 100*2 + 2*(10*1 + 90*2) = 200 + 380 = 580.
    BEZEL_EXPECT_EQ(stdoutOf(r), "580");
}
