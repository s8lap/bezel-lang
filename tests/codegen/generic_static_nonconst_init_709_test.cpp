// Codegen fix #709 — non-constant static-field initializer on a generic class
// must reach every per-instantiation global, not just the template-level one.
//
// §22.12: "each distinct instantiation has its own static state."
// §22.13: "Within a single type, static fields are initialized in textual
//         declaration order."
//
// Root cause before the fix: `emitStaticConstructorBody` evaluated the
// non-constant initializer (e.g. `Helper.Compute()`) and stored the result
// only into the template-level global (`bz$Registry$count`).  Per-
// instantiation globals (`bz$Registry$int$count`, `bz$Registry$string$count`)
// are created lazily by `staticFieldGlobal` during method-body emission (which
// precedes static-ctor emission).  Those globals were initialized from the
// template's compile-time constant initializer (zero for a non-literal) and
// the runtime store never reached them.  Any read of `Registry<int>.count`
// therefore returned 0 instead of the computed value.
//
// The fix iterates `genericInstStaticGlobals_` at the tail of the field-init
// loop in `emitStaticConstructorBody`, emitting a `store val, instGV` for
// every per-instantiation global whose parent field matches the current field
// symbol (`key.first == fs`).  This covers the common T-independent case (the
// initializer does not reference T) and is a no-op for non-generic classes.
//
// Spec references:
//   §22.12 — per-instantiation static state.
//   §22.13 — static-field initialization order.
//
// Related:
//   #536 — per-instantiation static STATE isolation (method-body path).
//   #582 — non-constant static init for NON-GENERIC classes.
//   #697 — per-instantiation globals for direct external access (non-generic ctx).
//   #701 — cross-named access from a different generic method.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}

bezel::test::JitResult jit(const std::string& prog) {
    return runJit(prog);
}

// The exact repro from the bug report: Helper.Compute() returns 7,
// Registry<int>.count and Registry<string>.count both initialize from it.
// Main returns a * 10 + b = 77.
const char* kRepro =
    "module M; "
    "class Helper "
    "  public static method Compute(): int return 7; end Compute; "
    "end Helper; "
    "class Registry<T> "
    "  public static count: int := Helper.Compute(); "
    "end Registry; ";

// A generic class whose static field initializer is a simple arithmetic
// expression involving another non-generic static field — T-independent.
const char* kComputed =
    "module M; "
    "class Base "
    "  public static seed: int := 3; "
    "end Base; "
    "class Cache<T> "
    "  public static value: int := Base.seed * 4; "
    "end Cache; ";

}  // namespace

// ── Core repro: two instantiations, T-independent init ───────────────────────

// §22.12 / §22.13 / #709: the exact repro — Registry<int>.count and
// Registry<string>.count must both equal 7 (from Helper.Compute()).
// Main: a * 10 + b = 77.
BEZEL_TEST(codegen_generic_static_nonconst_init_709,
           two_instantiations_compute_returns_77_spec_22_12_22_13) {
    BEZEL_EXPECT_EQ(run(
        std::string(kRepro) +
        "class Program "
        "  public static method Main(): int "
        "    var a := Registry<int>.count; "
        "    var b := Registry<string>.count; "
        "    return a * 10 + b; "
        "  end Main; "
        "end Program;"), 77);
}

// §22.12 / #709: Registry<int>.count must equal 7 individually.
BEZEL_TEST(codegen_generic_static_nonconst_init_709,
           int_instantiation_count_equals_7_spec_22_12) {
    BEZEL_EXPECT_EQ(run(
        std::string(kRepro) +
        "class Program "
        "  public static method Main(): int "
        "    return Registry<int>.count; "
        "  end Main; "
        "end Program;"), 7);
}

// §22.12 / #709: Registry<string>.count must equal 7 individually.
BEZEL_TEST(codegen_generic_static_nonconst_init_709,
           string_instantiation_count_equals_7_spec_22_12) {
    BEZEL_EXPECT_EQ(run(
        std::string(kRepro) +
        "class Program "
        "  public static method Main(): int "
        "    return Registry<string>.count; "
        "  end Main; "
        "end Program;"), 7);
}

// ── Three instantiations: int, string, bool ───────────────────────────────────

// §22.12 / #709: three distinct instantiations all receive the same
// T-independent computed initializer value.
BEZEL_TEST(codegen_generic_static_nonconst_init_709,
           three_instantiations_all_receive_init_spec_22_12) {
    BEZEL_EXPECT_EQ(run(
        std::string(kRepro) +
        "class Program "
        "  public static method Main(): int "
        "    var a := Registry<int>.count; "
        "    var b := Registry<string>.count; "
        "    var c := Registry<bool>.count; "
        "    if a = 7 and b = 7 and c = 7 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end Program;"), 1);
}

// ── Arithmetic init expression ────────────────────────────────────────────────

// §22.12 / §22.13 / #709: Cache<T>.value := Base.seed * 4 = 12.
// Both Cache<int>.value and Cache<string>.value must equal 12.
BEZEL_TEST(codegen_generic_static_nonconst_init_709,
           arithmetic_init_both_instantiations_correct_spec_22_13) {
    BEZEL_EXPECT_EQ(run(
        std::string(kComputed) +
        "class Program "
        "  public static method Main(): int "
        "    var a := Cache<int>.value; "
        "    var b := Cache<string>.value; "
        "    if a = 12 and b = 12 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end Program;"), 1);
}

// ── Post-init writes are independent ─────────────────────────────────────────

// §22.12 / #709: after init, each instantiation's global is still isolated.
// Write to Registry<int>.count must not affect Registry<string>.count.
BEZEL_TEST(codegen_generic_static_nonconst_init_709,
           post_init_writes_remain_isolated_spec_22_12) {
    BEZEL_EXPECT_EQ(run(
        std::string(kRepro) +
        "class Program "
        "  public static method Main(): int "
        "    Registry<int>.count := 99; "
        "    if Registry<int>.count = 99 and Registry<string>.count = 7 "
        "    then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end Program;"), 1);
}

// ── Regression: constant-literal static init on a generic class still works ──

// §22.12 / §22.13 / #697 / #536 regression guard: a constant-literal
// initializer on a generic class must still work after the #709 fix.
// The fix only runs the extra store loop for non-constant initializers; the
// constant-literal path (constInitFor) is unchanged.
BEZEL_TEST(codegen_generic_static_nonconst_init_709,
           constant_literal_init_regression_spec_22_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Tagged<T> "
        "  public static version: int := 42; "
        "end Tagged; "
        "class Program "
        "  public static method Main(): int "
        "    if Tagged<int>.version = 42 and Tagged<string>.version = 42 "
        "    then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end Program;"), 1);
}

// ── Regression: non-generic class non-constant static init still works ────────

// §22.13 / #582 regression guard: a non-generic class with a non-constant
// static field initializer must still be computed correctly. The fix adds
// a per-inst store loop that iterates over an empty range for non-generic
// fields, so the CG12 / #582 path is unaffected.
BEZEL_TEST(codegen_generic_static_nonconst_init_709,
           nongeneric_nonconst_init_regression_spec_22_13) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Helper2 "
        "  public static method Compute(): int return 5; end Compute; "
        "end Helper2; "
        "class NonGen "
        "  public static count: int := Helper2.Compute(); "
        "end NonGen; "
        "class Program "
        "  public static method Main(): int "
        "    return NonGen.count; "
        "  end Main; "
        "end Program;"), 5);
}

// ── Mix: one constant field + one non-constant field on same generic class ────

// §22.12 / §22.13 / #709: a generic class with BOTH a constant-literal and a
// non-constant static field. Both fields must be correct on each instantiation.
BEZEL_TEST(codegen_generic_static_nonconst_init_709,
           mixed_constant_and_nonconstant_fields_spec_22_12_22_13) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Helper3 "
        "  public static method Base(): int return 3; end Base; "
        "end Helper3; "
        "class Mixed<T> "
        "  public static literal: int := 10; "
        "  public static computed: int := Helper3.Base() * 2; "
        "end Mixed; "
        "class Program "
        "  public static method Main(): int "
        "    var a := Mixed<int>.literal + Mixed<int>.computed; "
        "    var b := Mixed<string>.literal + Mixed<string>.computed; "
        "    if a = 16 and b = 16 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end Program;"), 1);
}
