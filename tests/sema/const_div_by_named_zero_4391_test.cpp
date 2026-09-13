// Regression test for #4391: const division by a named zero const is not
// rejected at compile time.
//
// Spec §17.14 (v0.184, #1688): "an evaluation FAULT — integer/decimal overflow,
// or division/mod by zero — in a constant expression is a COMPILE ERROR at the
// constant expression, never a deferred runtime fault."
//
// The existing lit test `const_div_by_zero_1688.bzl` pins that literal zero
// (`const D := 5 / 0;`) is correctly rejected. This test pins the gap:
// a named const whose value is zero (`const Z := 0; const C := 5 / Z;`)
// was silently ACCEPTED (compiled, raised DivideByZeroException at runtime).
// The `containsConstDivByZero` walk must resolve named const identifiers to
// their values when checking if a divisor is zero.
//
// Three shapes are tested:
//   (a) class-level const divisor (same class, explicit type)
//   (b) class-level const divisor (same class, inferred type)
//   (c) local const divisor (inside method body)

#include "support/aot_support.hpp"   // runAot — the real bezelc AOT oracle
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// True iff the compile was rejected AND some diagnostic contains `needle`.
bool hasMsg(const std::string& prog, const std::string& needle) {
    auto r = runAot(prog);
    if (r.ran) return false;              // compiled clean — no diagnostic
    if (!r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

constexpr const char* kDivZeroDiag =
    "initializer contains a division or modulo by zero";

}  // namespace

// Shape (a): class-level, explicit type on divisor
BEZEL_TEST(const_div_by_named_zero_4391,
           class_level_explicit_type_rejected) {
    BEZEL_EXPECT_TRUE(hasMsg(
        "module M; "
        "public class K "
        "  public const Z: int := 0; "
        "  public const C := 5 / Z; "
        "  public static method Main(): int return 0; end Main; "
        "end K;",
        kDivZeroDiag));
}

// Shape (b): class-level, inferred type on divisor
BEZEL_TEST(const_div_by_named_zero_4391,
           class_level_inferred_type_rejected) {
    BEZEL_EXPECT_TRUE(hasMsg(
        "module M; "
        "public class K "
        "  public const Z := 0; "
        "  public const C := 5 / Z; "
        "  public static method Main(): int return 0; end Main; "
        "end K;",
        kDivZeroDiag));
}

// Shape (c): local const divisor (method body)
BEZEL_TEST(const_div_by_named_zero_4391,
           local_const_divisor_rejected) {
    BEZEL_EXPECT_TRUE(hasMsg(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    const Z: int := 0; "
        "    const C := 5 / Z; "
        "    return 0; "
        "  end Main; "
        "end C;",
        kDivZeroDiag));
}
