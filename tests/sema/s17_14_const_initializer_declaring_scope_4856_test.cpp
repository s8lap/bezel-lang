// fix #4856 / §17.14 / §2.8.3 / §22.12 / §5.2 (v0.307): a reference to a
// `const` denotes THAT const's value, and the names in its initializer are
// bound where the const is DECLARED. The §17.14 compile-time folds (the
// integer fold behind §5.1/§5.2 narrowing and case-range gates, and #4801's
// float/double/decimal fold) used to follow a class const's initializer and
// re-resolve its bare names in the USE site's scope, so a same-named member
// of the caller's class, or a local const, substituted a different value —
// `int(A.K)` with `A.K := M1`, `A.M1 = 1.0` was rejected as the value 1e30
// when the caller's class declared its own `M1 := 1.0e30`.
//
// ACCEPT rows are runAot VALUE rows (compile, run, exact stdout), so a
// spurious reject (no run) and a wrong fold both fail. The REJECT controls
// keep the genuine case (the declaring scope's own value overflows) a compile
// error with its specific diagnostic.
//
// Authority: spec §17.14 / §2.8.3 / §22.12 / §5.2, v0.307.

#include <string>
#include <string_view>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bool hasDiag(const bezel::test::JitResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// `A` declares K := M1 with its OWN M1; `C` (the use site) declares a
// different M1. `aM1` / `cM1` are the two initializers, `ty` the const type.
std::string prog(const std::string& ty, const std::string& aM1,
                 const std::string& cM1, const std::string& mainBody) {
    return "module M; "
           "class A "
           "public static const M1: " + ty + " := " + aM1 + "; "
           "public static const K: " + ty + " := M1; "
           "end A; "
           "class C "
           "public static const M1: " + ty + " := " + cM1 + "; "
           "public static const J: " + ty + " := A.K; "
           "public static method Main(): int " + mainBody +
           " return 0; end Main; end C;";
}

}  // namespace

// The issue's repro: float fold, shadowing CLASS member at the use site.
BEZEL_TEST(s17_14_const_initializer_declaring_scope_4856,
           double_qualified_const_not_shadowed_by_caller_member) {
    auto r = runAot(prog("double", "1.0", "1.0e30",
                         "Console.WriteLine(int(A.K));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// Same, shadowing LOCAL const at the use site.
BEZEL_TEST(s17_14_const_initializer_declaring_scope_4856,
           double_qualified_const_not_shadowed_by_local_const) {
    auto r = runAot(prog("double", "1.0", "1.0",
                         "const M1: double := 1.0e30; "
                         "Console.WriteLine(int(A.K) + int(M1 / 1.0e30));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

// The BARE-identifier path: C.J := A.K is reached through `J`, whose own
// initializer is folded in C's scope, then A.K's in A's.
BEZEL_TEST(s17_14_const_initializer_declaring_scope_4856,
           double_bare_const_chain_crosses_declaring_scopes) {
    auto r = runAot(prog("double", "1.0", "1.0e30",
                         "Console.WriteLine(int(J));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// The integer twin (foldConstIdentTC): `short(A.K)` was rejected as 100000.
BEZEL_TEST(s17_14_const_initializer_declaring_scope_4856,
           int_qualified_const_not_shadowed_by_caller_member) {
    auto r = runAot(prog("int", "1", "100000",
                         "Console.WriteLine(short(A.K));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

BEZEL_TEST(s17_14_const_initializer_declaring_scope_4856,
           int_qualified_const_not_shadowed_by_local_const) {
    auto r = runAot(prog("int", "1", "1",
                         "const M1: int := 100000; "
                         "Console.WriteLine(short(A.K));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// The integer BARE-identifier path (foldConstIdentTC following `K` itself):
// C's own `K := M1` must read C.M1, not Main's shadowing local `M1`. (The
// integer fold does not follow a QUALIFIED name inside an initializer, so the
// J := A.K chain above has no integer twin.)
BEZEL_TEST(s17_14_const_initializer_declaring_scope_4856,
           int_bare_const_not_shadowed_by_local_const) {
    auto r = runAot(
        "module M; class C "
        "public static const M1: int := 1; "
        "public static const K: int := M1; "
        "public static method Main(): int "
        "const M1: int := 100000; "
        "Console.WriteLine(short(K)); "
        "return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// CONTROLS: when the DECLARING scope's own value overflows, the conversion is
// still a §5.2 compile error — the fix moves the lookup, it does not stop it.
BEZEL_TEST(s17_14_const_initializer_declaring_scope_4856,
           double_declaring_scope_overflow_still_rejected) {
    auto r = runAot(prog("double", "1.0e30", "1.0",
                         "Console.WriteLine(int(A.K));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasDiag(
        r, "a `double` constant expression whose value is `1e+30`) of the "
           "explicit conversion to `int`"));
}

BEZEL_TEST(s17_14_const_initializer_declaring_scope_4856,
           int_declaring_scope_overflow_still_rejected) {
    auto r = runAot(prog("int", "100000", "1",
                         "Console.WriteLine(short(A.K));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasDiag(
        r, "`100000` of the explicit conversion to `short`"));
}
