// Codegen — #5217 (§6.6, v0.313 / §5.1 / §17.14): the #5211 predicate
// `isLitDerived` was purely syntactic literal-derived; issue #5217 narrows
// it to a real §17.14 constant predicate and adds the §5.1/§5.2 range
// check for compile-time constants. Sema rule pinned in
// tests/lit/sema/char_compound_*_5217.bzl; the rows here pin the runtime
// behavior of the admitted cases (named/local const RHS, in-range) and
// the silent-wrap regression for out-of-range uint RHS.
//
// Pre-fix: `c += K` (K := 5) failed to compile (false reject — K is a
// class-level const, not a literal). Post-fix: admitted, codegen computes
// char(100) + 5 = char(105).
//
// Pre-fix: `c += 4294967295U` compiled and stored char 99 (silent wrap).
// Post-fix: compile error at the conversion site per §5.1.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
std::string run(const std::string& body) {
    auto r = runAot(body);
    if (!r.ran) return "NORUN";
    std::string s = r.stdoutText;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}
}  // namespace

// Named compile-time constant RHS admitted under §5.1's constant narrowing
// (§17.14 / §22.12). Pre-fix: rejected (false reject). Post-fix: compiles
// and runs to char(105).
BEZEL_TEST(codegen_5217_char_compound_uint_const, named_class_const_rhs) {
    BEZEL_EXPECT_EQ(run("module M; class C public const K: uint := 5; "
                        "public static method Main(): int "
                        "var c: char := char(100); c += K; "
                        "Console.WriteLine(int(c)); return 0; "
                        "end Main; end C;"),
                    std::string("105"));
}

// Local compile-time constant RHS admitted under §5.1's constant narrowing
// (§17.14 / §21.1). Pre-fix: rejected (false reject). Post-fix: compiles
// and runs to char(103).
BEZEL_TEST(codegen_5217_char_compound_uint_const, local_const_rhs) {
    BEZEL_EXPECT_EQ(run("module M; class C public static method Main(): int "
                        "const L: uint := 3; "
                        "var c: char := char(100); c += L; "
                        "Console.WriteLine(int(c)); return 0; "
                        "end Main; end C;"),
                    std::string("103"));
}

// Out-of-range uint literal RHS rejected at the conversion site per §5.1.
// Pre-fix: admitted, codegen wrapped to char(99) (silent wrong value).
// Post-fix: compile error; the test asserts that the build no longer
// runs and that the program's run would not produce "99".
BEZEL_TEST(codegen_5217_char_compound_uint_const,
           uint_literal_out_of_range_rejected) {
    auto r = runAot("module M; class C public static method Main(): int "
                    "var c: char := char(100); "
                    "try c += 4294967295U; Console.WriteLine(int(c)); "
                    "catch OverflowException => Console.WriteLine(\"OVF\"); end; "
                    "return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("99") == std::string::npos);
}

// CONTROL — `c += 5` (an `int` literal that fits char's range) was already
// admitted by the #5211 rule and remains admitted by #5217's constant
// predicate + §5.1 narrowing. Pin that the in-range literal case is
// unaffected by the #5217 rewrite.
BEZEL_TEST(codegen_5217_char_compound_uint_const,
           int_literal_in_range_unchanged_control) {
    BEZEL_EXPECT_EQ(run("module M; class C public static method Main(): int "
                        "var c: char := char(100); c += 5; c -= 2; c++; "
                        "Console.WriteLine(int(c)); return 0; "
                        "end Main; end C;"),
                    std::string("104"));
}