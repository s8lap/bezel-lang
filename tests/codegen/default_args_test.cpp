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
