// Codegen slice CG45 — default parameter values (§22.7). A call that omits
// a trailing parameter with a default fills it with the default-value
// expression, evaluated at the call site. Observable via the int exit code.
// (Named arguments, which reorder/skip, are a separate slice.)

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

BEZEL_TEST(codegen_cg45, default_used_when_omitted) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Add(a: int, b: int := 10): int => a + b; "
        "  public static method Main(): int => Add(5); "
        "end C;"), 15);
}

BEZEL_TEST(codegen_cg45, explicit_overrides_default) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Add(a: int, b: int := 10): int => a + b; "
        "  public static method Main(): int => Add(5, 20); "
        "end C;"), 25);
}

BEZEL_TEST(codegen_cg45, multiple_defaults_all_omitted) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method F(a: int, b: int := 2, c: int := 3): int "
        "    => a + b + c; "
        "  public static method Main(): int => F(10); "
        "end C;"), 15);
}

BEZEL_TEST(codegen_cg45, multiple_defaults_partial) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method F(a: int, b: int := 2, c: int := 3): int "
        "    => a + b + c; "
        "  public static method Main(): int => F(10, 20); "
        "end C;"), 33);
}

BEZEL_TEST(codegen_cg45, instance_method_default) {
    BEZEL_EXPECT_EQ(run(
        "module M; class N "
        "  public V: int := 0; "
        "  public method Add(d: int := 5) V := V + d; end Add; "
        "end N; "
        "class C public static method Main(): int "
        "  var n := new N(); n.Add(); n.Add(); return n.V; "
        "end Main; end C;"), 10);
}

// §22.5: "a default-value expression may ... read other parameters appearing
// earlier in the same parameter list (e.g. M(a: int, b: int = a + 1) is
// permitted)." The default must evaluate against THIS call's earlier argument
// values, not a zero/garbage slot. (Regression: the default `b := a` produced
// 0 because `a` inside the default was lowered in a scope where the earlier
// parameter wasn't bound to its argument value.)
BEZEL_TEST(codegen_cg45, default_reads_earlier_param_spec_22_5) {
    // F(5): b defaults to a (= 5). Without the fix this returned 0.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method F(a: int, b: int := a): int => b; "
        "  public static method Main(): int => F(5); "
        "end C;"), 5);
}

BEZEL_TEST(codegen_cg45, explicit_overrides_earlier_param_default_spec_22_5) {
    // F(5, 9): b is supplied (= 9), the default is not taken.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method F(a: int, b: int := a): int => b; "
        "  public static method Main(): int => F(5, 9); "
        "end C;"), 9);
}

BEZEL_TEST(codegen_cg45, default_earlier_param_in_expression_spec_22_5) {
    // §22.5 example: M(a: int, b: int = a + 1). F(5) → b = 6.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method F(a: int, b: int := a + 1): int => b; "
        "  public static method Main(): int => F(5); "
        "end C;"), 6);
}

BEZEL_TEST(codegen_cg45, default_literal_still_works_control_spec_22_5) {
    // Control: a literal default must keep working alongside the earlier-param
    // path (it ignores the temporary earlier-param bindings).
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method F(a: int, b: int := 99): int => b; "
        "  public static method Main(): int => F(5); "
        "end C;"), 99);
}

BEZEL_TEST(codegen_cg45, default_chained_earlier_params_spec_22_5) {
    // §22.5: each default reads parameters strictly EARLIER in the list. Here
    // b := a and c := b chain; F(5) → a=5, b=5, c=5, sum 15.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method F(a: int, b: int := a, c: int := b): int "
        "    => a + b + c; "
        "  public static method Main(): int => F(5); "
        "end C;"), 15);
}

BEZEL_TEST(codegen_cg45, ctor_default_reads_earlier_param_spec_22_5) {
    // §22.5 applies to constructors too: the default for `b` reads the
    // earlier ctor parameter `a`. new C(7) → V = a + b = 7 + 7 = 14.
    BEZEL_EXPECT_EQ(run(
        "module M; class N "
        "  public V: int := 0; "
        "  public constructor(a: int, b: int := a) V := a + b; end constructor; "
        "end N; "
        "class C public static method Main(): int "
        "  var n := new N(7); return n.V; "
        "end Main; end C;"), 14);
}
