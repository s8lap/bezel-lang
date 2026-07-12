// Codegen slice CG36 — const members (§A.4). A `const X := …` member
// lowers to its initializer's compile-time constant value, referenced
// bare (`X`), qualified (`C.X`), or cross-class. Observable via the int
// exit code.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg36, bare_const_reference) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public const MAX: int := 100; "
        "  public static method Main(): int => MAX; "
        "end C;"), 100);
}

BEZEL_TEST(codegen_cg36, const_in_expression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public const A: int := 40; "
        "  public const B: int := 2; "
        "  public static method Main(): int => A + B; "
        "end C;"), 42);
}

BEZEL_TEST(codegen_cg36, qualified_const_reference) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class K public const V: int := 7; end K; "
        "class C public static method Main(): int "
        "  return K.V; "
        "end Main; end C;"), 7);
}

BEZEL_TEST(codegen_cg36, const_bool_in_condition) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public const ON: bool := true; "
        "  public static method Main(): int "
        "    if ON then return 5; end; return 0; "
        "  end Main; "
        "end C;"), 5);
}

BEZEL_TEST(codegen_cg36, const_used_as_loop_bound) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public const N: int := 5; "
        "  public static method Main(): int "
        "    var i := 0; var s := 0; "
        "    while i < N do s += 10; i += 1; end; return s; "
        "  end Main; "
        "end C;"), 50);
}

// issue #609 / §22.12 / §8.5: a `var` local inferred from a qualified const
// member access (`var x := Class.Const`) must carry the const's type so that
// subsequent `.ToString()` resolves to the primitive ToString intrinsic. Bug:
// `resolveMemberAccess` did not set exprType for a local ConstMemberSymbol,
// leaving the inferred local untyped → codegen emitted no output.

// §22.12 / §8.5: inferred local from qualified int const + ToString (#609).
BEZEL_TEST(codegen_cg36_609, inferred_local_from_const_tostring) {
    // var fromConst := Config.Limit infers int; fromConst.ToString() = "42",
    // Length = 2.  Before the fix this returned 0 (no output / wrong type).
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Config public const Limit: int := 42; end Config; "
        "class C public static method Main(): int "
        "  var fromConst := Config.Limit; "
        "  return fromConst.ToString().Length; "
        "end Main; end C;"), 2);
}

// §22.12 / §8.5: inferred local from qualified const + equality test (#609).
BEZEL_TEST(codegen_cg36_609, inferred_local_from_const_value_correct) {
    // Confirms the inferred local carries the VALUE 42, not some garbage.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Config public const Limit: int := 42; end Config; "
        "class C public static method Main(): int "
        "  var fromConst := Config.Limit; "
        "  return (1 if fromConst.ToString() = \"42\" else 0); "
        "end Main; end C;"), 1);
}

// §22.12 / §8.5: inferred local in arithmetic with const then ToString (#609).
// Bug affected binary expressions too because typeBinary used the untyped
// left operand → result was also untyped.
BEZEL_TEST(codegen_cg36_609, const_arithmetic_inferred_tostring) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Config public const Limit: int := 42; end Config; "
        "class C public static method Main(): int "
        "  var result := Config.Limit + 1; "
        "  return (1 if result.ToString() = \"43\" else 0); "
        "end Main; end C;"), 1);
}

// §22.12 / §8.5: direct qualified const.ToString() (no intermediate var) (#609).
BEZEL_TEST(codegen_cg36_609, direct_const_tostring) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Config public const Limit: int := 42; end Config; "
        "class C public static method Main(): int "
        "  return Config.Limit.ToString().Length; "
        "end Main; end C;"), 2);
}

// §22.12 / §8.5: const with no explicit ': Type' annotation inferred correctly
// (falls back to value expr's exprType) (#609).
BEZEL_TEST(codegen_cg36_609, unannotated_const_inferred_tostring) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Config public const Limit := 42; end Config; "
        "class C public static method Main(): int "
        "  var fromConst := Config.Limit; "
        "  return (1 if fromConst.ToString() = \"42\" else 0); "
        "end Main; end C;"), 1);
}
