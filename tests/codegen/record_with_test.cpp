// Codegen slice CG29c — record `with`-expressions (§9.5, non-destructive
// update). `r with name := value, ... end` evaluates to a copy of the record
// `r` with the named fields replaced; `r` itself is unchanged (records are
// value types). Observable via the int exit code.
//
// Regression: before this slice the `with` path hung the compiler at codegen
// (#254) — the source record value was used directly as a GEP base instead of
// being spilled to a stack slot first.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

BEZEL_TEST(codegen_cg29c, with_updates_one_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a := new P(1, 2); var b := a with x := 9 end; "
        "  return b.x * 10 + b.y; "
        "end Main; end C;"), 92);
}

BEZEL_TEST(codegen_cg29c, with_does_not_mutate_source) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a := new P(1, 2); var b := a with x := 9 end; "
        "  return a.x * 10 + a.y; "
        "end Main; end C;"), 12);
}
