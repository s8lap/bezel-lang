// fix #5205 (§22.6 / §25.2 / §6.6, v0.313): compound assignment on a Named
// target ignored the §22.6 overload ranking (#5110 fixed only the binary
// form). `v += 100` against `+(V, short)` and `+(V, long)` silently picked
// `long`; §22.6 Example 2 makes it an ambiguity error, exactly as `v + 100`.
// Sema now ranks the compound path's candidates through the same Exact /
// Converting relation and codegen adopts sema's recorded resolution.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {
const char* kTwoOps =
    "module M; "
    "record V(X: int) "
    "  public static method operator +(v: V, b: short): V => new V(v.X + int(b)); "
    "  public static method operator +(v: V, n: long): V => new V(v.X + 1000); "
    "end V; ";
std::string prog(const std::string& body) {
    return std::string(kTwoOps) +
           "public class Program "
           "  public static method Main(): int " + body +
           "    Console.WriteLine(v.X); return 0; "
           "  end Main; "
           "end Program;";
}
}  // namespace

BEZEL_TEST(sema_compound_operator_overload_tie_5205,
           t1_constant_tie_rejected_spec_22_6) {
    auto r = runAot(prog("var v := new V(1); v += 100; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "ambiguous operator `+`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`operator +(V, short)`, `operator +(V, long)`"));
}

BEZEL_TEST(sema_compound_operator_overload_tie_5205,
           t2_exact_short_wins_spec_22_6) {
    auto r = runAot(prog("var v := new V(1); var s: short := 7; v += s; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "8\n");
}

BEZEL_TEST(sema_compound_operator_overload_tie_5205,
           t3_exact_long_wins_spec_22_6) {
    auto r = runAot(prog("var v := new V(1); var l: long := 7; v += l; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1001\n");
}

BEZEL_TEST(sema_compound_operator_overload_tie_5205,
           t4_explicit_short_cast_resolves_spec_22_6) {
    auto r = runAot(prog("var v := new V(1); v += short(100); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "101\n");
}

BEZEL_TEST(sema_compound_operator_overload_tie_5205,
           t5_derived_rhs_converts_to_base_param_spec_25_2) {
    auto r = runAot(
        "module M; "
        "class Base "
        "  public constructor() end constructor; "
        "  public method N(): int => 41; "
        "end Base; "
        "class Derived : Base "
        "  public constructor() base(); end constructor; "
        "end Derived; "
        "record V(X: int) "
        "  public static method operator +(v: V, b: Base): V => new V(v.X + b.N()); "
        "end V; "
        "public class Program "
        "  public static method Main(): int "
        "    var v := new V(1); v += new Derived(); "
        "    Console.WriteLine(v.X); return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

BEZEL_TEST(sema_compound_operator_overload_tie_5205,
           t6_nullable_target_rejected_spec_7_11) {
    auto r = runAot(
        std::string(kTwoOps) +
        "public class Program "
        "  public static method Main(): int "
        "    var v: V? := new V(1); var s: short := 7; v += s; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "compound assignment `+=` has no valid result"));
}
