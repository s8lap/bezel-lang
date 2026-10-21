// fix #6804 (§5.1 *Constant narrowing*, §4.2 *Target-typed integer constants*,
// §22.6, v0.340).
//
// §5.1 (v0.340): "A constant whose value does not fit the target's range is a
// **compile error** at the conversion site — never a silent truncation."
// §4.2: the target-typed integer constant is "range-checked against" its
// written target. Before the fix `var u: uint := -3_000_000_000;` typed the
// magnitude at `uint` and then negated it, storing 1294967296.
//
// Probes: /var/tmp/bezel-scratch/r34-negconst/ and /m/.

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using namespace bezel::test;

namespace {

std::string mainProg(const std::string& body, const std::string& pre = "") {
    return "module M;\n" + pre +
           "class P public static method Main(): int\n  " + body +
           "\n  return 0; end Main; end P;\n";
}

bool rejectedDoesNotFit(const std::string& src) {
    auto r = runAot(src);
    return failedAtCompileTimeWithRealLocation(r) && !r.ran &&
           diagnosticMessageContains(r, "does not fit");
}

std::string prints(const std::string& src) {
    auto r = runAot(src);
    if (!r.ran) return "<did not run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

// ctor(sigs...) each setting v := 1-based position; `new C(arg)` prints v.
std::string ctorProg(const std::string& sigs, const std::string& arg) {
    std::string s = "class C\n  public v: int;\n";
    int n = 1;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t comma = sigs.find(',', pos);
        const std::string t = sigs.substr(
            pos, comma == std::string::npos ? std::string::npos : comma - pos);
        s += "  public constructor(x0: " + t + ") v := " + std::to_string(n++) +
             "; end constructor;\n";
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    s += "end C;\n";
    return mainProg("var c := new C(" + arg +
                        ");\n  Console.WriteLine(c.v);", s);
}

}  // namespace

// The issue's repro: a negative constant cannot be held by `uint`/`ulong`.
BEZEL_TEST(negative_target_typed_constant_unsigned_6804,
           local_initializer_rejected_at_unsigned_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedDoesNotFit(
        mainProg("var u: uint := -3_000_000_000; Console.WriteLine(u);")));
    BEZEL_EXPECT_TRUE(rejectedDoesNotFit(
        mainProg("var u: ulong := -3_000_000_000; Console.WriteLine(u);")));
    BEZEL_EXPECT_TRUE(rejectedDoesNotFit(
        mainProg("var u: ulong := -5_000_000_000; Console.WriteLine(u);")));
    // Parentheses group and introduce no expression (§4.2 boundary carve-out).
    BEZEL_EXPECT_TRUE(rejectedDoesNotFit(
        mainProg("var u: uint := -(3_000_000_000); Console.WriteLine(u);")));
    BEZEL_EXPECT_TRUE(rejectedDoesNotFit(
        mainProg("var u: ulong := -(3_000_000_000); Console.WriteLine(u);")));
}

BEZEL_TEST(negative_target_typed_constant_unsigned_6804,
           assignment_and_argument_rejected_at_unsigned_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedDoesNotFit(mainProg(
        "var u: uint := 1; u := -3_000_000_000; Console.WriteLine(u);")));
    BEZEL_EXPECT_TRUE(rejectedDoesNotFit(mainProg(
        "var u: ulong := 1; u := -3_000_000_000; Console.WriteLine(u);")));
    const std::string f =
        "class Q public static method F(x: uint): int => 1; end Q;\n";
    BEZEL_EXPECT_TRUE(rejectedDoesNotFit(
        mainProg("_ := Q.F(-3_000_000_000);", f)));
    const std::string g =
        "class Q public static method F(x: ulong): int => 1; end Q;\n";
    BEZEL_EXPECT_TRUE(rejectedDoesNotFit(
        mainProg("_ := Q.F(-3_000_000_000);", g)));
}

// Controls: the signed targets that DO hold the value, and the minimum edges.
BEZEL_TEST(negative_target_typed_constant_unsigned_6804,
           signed_targets_and_minimum_edges_accept_spec_5_1) {
    BEZEL_EXPECT_EQ(
        prints(mainProg("var l: long := -3_000_000_000; Console.WriteLine(l);")),
        "-3000000000\n");
    BEZEL_EXPECT_EQ(
        prints(mainProg("var l: long := -(3_000_000_000); Console.WriteLine(l);")),
        "-3000000000\n");
    BEZEL_EXPECT_EQ(
        prints(mainProg("var i: int := -2147483648; Console.WriteLine(i);")),
        "-2147483648\n");
    BEZEL_EXPECT_EQ(
        prints(mainProg("var i: int := int.MinValue; Console.WriteLine(i);")),
        "-2147483648\n");
    BEZEL_EXPECT_EQ(
        prints(mainProg("var i: long := -9223372036854775808L; "
                        "Console.WriteLine(i);")),
        "-9223372036854775808\n");
    BEZEL_EXPECT_EQ(
        prints(mainProg("var s: short := -32768; Console.WriteLine(s);")),
        "-32768\n");
    BEZEL_EXPECT_EQ(
        prints(mainProg("var s: sbyte := -128; Console.WriteLine(s);")),
        "-128\n");
    // A non-negative constant of the same magnitude still fits the unsigned.
    BEZEL_EXPECT_EQ(
        prints(mainProg("var u: uint := 3_000_000_000; Console.WriteLine(u);")),
        "3000000000\n");
    BEZEL_EXPECT_EQ(
        prints(mainProg("var u: ulong := 3_000_000_000; Console.WriteLine(u);")),
        "3000000000\n");
}

// Just below each signed minimum, and -1 at every unsigned type, still reject.
BEZEL_TEST(negative_target_typed_constant_unsigned_6804,
           below_minimum_and_minus_one_unsigned_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedDoesNotFit(
        mainProg("var s: short := -32769; Console.WriteLine(s);")));
    BEZEL_EXPECT_TRUE(rejectedDoesNotFit(
        mainProg("var s: sbyte := -129; Console.WriteLine(s);")));
    BEZEL_EXPECT_TRUE(rejectedDoesNotFit(
        mainProg("var i: int := -2147483649; Console.WriteLine(i);")));
    for (const char* t : {"byte", "ushort", "uint", "ulong"})
        BEZEL_EXPECT_TRUE(rejectedDoesNotFit(mainProg(
            std::string("var u: ") + t + " := -1; Console.WriteLine(u);")));
    BEZEL_EXPECT_TRUE(rejectedDoesNotFit(
        mainProg("var u: uint := 0 - 5; Console.WriteLine(u);")));
    BEZEL_EXPECT_TRUE(rejectedDoesNotFit(
        mainProg("const N := -5; var u: uint := N; Console.WriteLine(u);")));
}

// §22.6: only `long` holds -3e9, so the pick is `long` in BOTH orders (the
// `ulong`/`uint` candidate is inapplicable, not co-equal).
BEZEL_TEST(negative_target_typed_constant_unsigned_6804,
           ctor_overload_selects_long_both_orders_spec_22_6) {
    BEZEL_EXPECT_EQ(prints(ctorProg("ulong,long", "-3_000_000_000")), "2\n");
    BEZEL_EXPECT_EQ(prints(ctorProg("long,ulong", "-3_000_000_000")), "1\n");
    BEZEL_EXPECT_EQ(prints(ctorProg("uint,long", "-3_000_000_000")), "2\n");
    BEZEL_EXPECT_EQ(prints(ctorProg("long,uint", "-3_000_000_000")), "1\n");
    // int-range negative: both signed candidates hold it, `int` is exact-ish
    // by the existing ranking; unsigned candidate is inapplicable.
    BEZEL_EXPECT_EQ(prints(ctorProg("ulong,long", "-5")), "2\n");
    BEZEL_EXPECT_EQ(prints(ctorProg("ushort,int", "-3")), "2\n");
    BEZEL_EXPECT_EQ(prints(ctorProg("int,ushort", "-3")), "1\n");
    // Control, unchanged by the fix: the POSITIVE constant stays ambiguous
    // (§22.6 "f(3_000_000_000) against f(long) beside f(ulong) is ambiguous").
    auto r = runAot(ctorProg("ulong,long", "3_000_000_000"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r) && !r.ran &&
                      diagnosticMessageContains(r, "ambiguous call to"));
    // Single unsigned ctor: rejected at the conversion site, not ambiguous.
    BEZEL_EXPECT_TRUE(rejectedDoesNotFit(ctorProg("ulong", "-3_000_000_000")));
}
