// §12.8 / Appendix C.5 (issue #5151, v0.313; rule added v0.309, #5036): a
// `this(...)` chain must end. Following each constructor's chain to the
// constructor it resolves to (§22.6, at the declaration) must reach one that
// does not chain; a constructor that reaches itself, directly or through
// others, is a compile error at its declaration naming the participating
// constructors, as for an inheritance cycle (§12.10).
//
// MEASURED before the fix: the record and same-arity rows below compiled and
// the binary stack-overflowed. The #3793 self-chain heuristic judges only a
// DIRECT self-chain by arity and exempts every type with a primary list.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasError(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

constexpr const char* kCycle = "constructor chain cycle in";

const char* kMain =
    "class Program "
    "  public static method Main(): int "
    "    Console.WriteLine(\"ok\"); "
    "    return 0; "
    "  end Main; "
    "end Program;";

}  // namespace

// p3: defaulted-sibling cycle on a record with a primary list.
BEZEL_TEST(this_chain_cycle_5151, rejects_record_defaulted_sibling_cycle_12_8) {
    auto r = runAot(
        std::string("module M; "
        "record R(X: int) "
        "  public constructor(a: bool, b: bool) this(true, false); "
        "  end constructor; "
        "  public constructor(a: bool, b: bool, c: int := 0) this(a, b); "
        "  end constructor; "
        "end R; ") + kMain);
    BEZEL_EXPECT_TRUE(hasError(r, kCycle));
    BEZEL_EXPECT_TRUE(hasError(r, "`R(bool, bool)`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// p4: same-arity two-constructor cycle, distinguished by parameter type.
BEZEL_TEST(this_chain_cycle_5151, rejects_same_arity_two_ctor_cycle_12_8) {
    auto r = runAot(
        std::string("module M; "
        "class C "
        "  public constructor(a: int) this(\"s\"); end constructor; "
        "  public constructor(s: string) this(1); end constructor; "
        "end C; ") + kMain);
    BEZEL_EXPECT_TRUE(hasError(r, kCycle));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Three-constructor cycle on a class with a primary list.
BEZEL_TEST(this_chain_cycle_5151, rejects_three_ctor_cycle_with_primary_12_8) {
    auto r = runAot(
        std::string("module M; "
        "class C(N: int) "
        "  public constructor(a: bool) this(\"s\"); end constructor; "
        "  public constructor(s: string) this(1.5); end constructor; "
        "  public constructor(d: double) this(true); end constructor; "
        "end C; ") + kMain);
    BEZEL_EXPECT_TRUE(hasError(r, kCycle));
    BEZEL_EXPECT_FALSE(r.ran);
}

// A three-constructor acyclic chain ending at the primary is accepted and
// builds the value.
BEZEL_TEST(this_chain_cycle_5151, accepts_three_ctor_acyclic_chain_12_8) {
    auto r = runAot(
        "module M; "
        "record R(X: int) "
        "  public constructor(a: bool) this(\"s\"); end constructor; "
        "  public constructor(s: string) this(7); end constructor; "
        "end R; "
        "class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new R(true).ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("R(X: 7)\n"));
}

// A defaulted sibling that is NOT part of a cycle stays accepted.
BEZEL_TEST(this_chain_cycle_5151, accepts_defaulted_sibling_chain_12_8) {
    auto r = runAot(
        "module M; "
        "record R(X: int) "
        "  public constructor(a: bool, b: bool) this(true, false, 3); "
        "  end constructor; "
        "  public constructor(a: bool, b: bool, c: int := 0) this(c); "
        "  end constructor; "
        "end R; "
        "class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new R(true, true).ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("R(X: 3)\n"));
}
