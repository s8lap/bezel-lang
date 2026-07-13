// Codegen fix #715 — a non-constant static-field initializer that references
// another static field of the SAME generic class via a qualified same-class
// access (e.g. `Registry<T>.seed + 10`) must read the already-initialized
// value, not a stale zero.
//
// §22.12: "each distinct instantiation has its own static state."
// §22.13: "Within a single type, static fields are initialized in textual
//         declaration order."
//
// Root cause before the fix:
//   Inside `emitStaticConstructorBody`, `lowerExpr(Registry<T>.seed + 10)` calls
//   `staticFieldGlobal` for `seed`.  Path (B) of `staticFieldGlobal` extracts
//   the generic args from the MemberAccessExpr target `Registry<T>`.  Because
//   the sfinit body runs with no active `genericSubst_` (it is a single template-
//   level function), `T` is an unresolved generic parameter whose TypeRef has
//   `kind==Named, declaration==nullptr`.  `typeRefIdentityToken` returns nullptr
//   for this TypeRef, and the mangle falls to the catch-all "t".  A spurious
//   per-instantiation global `bz$Registry$t$seed` was created with zero-init
//   and never stored into.  The derived-field initializer loaded 0 from that
//   global and computed 0 + 10 = 10 instead of 7 + 10 = 17.
//
// The fix (in `staticFieldGlobal`, src/codegen/LlvmBackend_Setup.cpp):
//   After collecting substIds via Path (B), check that ALL entries are non-null
//   (i.e. all type arguments are concrete resolved types).  If any substId is
//   nullptr (unresolved generic parameter), skip the per-instantiation redirect
//   and fall through to the template-level global, which is correctly initialized
//   first in the sfinit sequence.  Per §22.12 / #715.
//
// Contrast with unqualified access (`seed` without `Registry<T>.`): Path (B)
// is not taken at all (no MemberAccessExpr target), so the unqualified case
// was never broken.  Confirmed probe: unqualified derived = 17 even before fix.
//
// Spec references:
//   §22.12 — per-instantiation static state.
//   §22.13 — static-field initialization order.
//
// Related:
//   #709 — non-const static init propagated to per-inst globals.
//   #536 — per-instantiation static state isolation.
//   #697 — per-instantiation globals for direct external access.
//   #701 — cross-named access from a different generic method.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// Exact repro from the bug report: derived := Registry<T>.seed + 10.
// seed=7 (from Helper.Compute()), so derived must be 17, not 10.
const char* kRepro =
    "module M; "
    "class Helper "
    "  public static method Compute(): int return 7; end Compute; "
    "end Helper; "
    "class Registry<T> "
    "  public static seed: int := Helper.Compute(); "
    "  public static derived: int := Registry<T>.seed + 10; "
    "end Registry; ";

// Variant: the referenced field is itself a literal (constant-init).
const char* kLiteralSeed =
    "module M; "
    "class Box<T> "
    "  public static origin: int := 5; "
    "  public static doubled: int := Box<T>.origin * 2; "
    "end Box; ";

// Variant: three fields form a chain: a -> b -> c (each qualified with <T>).
const char* kChain =
    "module M; "
    "class Chain<T> "
    "  public static a: int := 3; "
    "  public static b: int := Chain<T>.a + 1; "
    "  public static c: int := Chain<T>.b * 2; "
    "end Chain; ";

}  // namespace

// ── Core repro: two instantiations, Registry<T>.seed cross-reference ─────────

// §22.12 / §22.13 / #715: Registry<int>.derived must equal 17 (= 7 + 10).
BEZEL_TEST(codegen_generic_static_sameclass_715,
           int_derived_equals_17_spec_22_12_22_13) {
    BEZEL_EXPECT_EQ(run(
        std::string(kRepro) +
        "class Program "
        "  public static method Main(): int "
        "    return Registry<int>.derived; "
        "  end Main; "
        "end Program;"), 17);
}

// §22.12 / §22.13 / #715: Registry<string>.derived must equal 17 (= 7 + 10).
BEZEL_TEST(codegen_generic_static_sameclass_715,
           string_derived_equals_17_spec_22_12_22_13) {
    BEZEL_EXPECT_EQ(run(
        std::string(kRepro) +
        "class Program "
        "  public static method Main(): int "
        "    return Registry<string>.derived; "
        "  end Main; "
        "end Program;"), 17);
}

// §22.12 / §22.13 / #715: exact repro scenario — both instantiations,
// seed and derived, all four values must be correct (7/17/7/17).
// Uses Console-style encoding: a*1000 + b*100 + c*10 + d.
// seed<int>=7, derived<int>=17, seed<string>=7, derived<string>=17.
// 7*1000 + 17*100 + 7*10 + 17 = 7000 + 1700 + 70 + 17 = 8787.
BEZEL_TEST(codegen_generic_static_sameclass_715,
           all_four_values_correct_spec_22_12_22_13) {
    // 8787 does not fit an 8-bit exit code — observe on stdout.
    BEZEL_EXPECT_EQ(runAotOut(
        std::string(kRepro) +
        "class Program "
        "  public static method Main(): int "
        "    var si := Registry<int>.seed; "
        "    var di := Registry<int>.derived; "
        "    var ss := Registry<string>.seed; "
        "    var ds := Registry<string>.derived; "
        "    Console.WriteLine(si * 1000 + di * 100 + ss * 10 + ds); return 0; "
        "  end Main; "
        "end Program;"), "8787\n");
}

// §22.12 / §22.13 / #715: seed itself is still correct (sanity check —
// the fix must not break the #709 path that propagates seed to per-inst globals).
BEZEL_TEST(codegen_generic_static_sameclass_715,
           seed_still_correct_after_fix_regression_709) {
    BEZEL_EXPECT_EQ(run(
        std::string(kRepro) +
        "class Program "
        "  public static method Main(): int "
        "    var a := Registry<int>.seed; "
        "    var b := Registry<string>.seed; "
        "    if a = 7 and b = 7 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end Program;"), 1);
}

// ── Variant: same-class qualified access on a constant-literal field ──────────

// §22.12 / §22.13 / #715: Box<T>.origin is a constant literal (5).
// Box<T>.doubled := Box<T>.origin * 2 must equal 10 for both instantiations.
// After the fix, Box<T>.origin resolves to the template-level global (with
// constant 5) rather than a spurious T-param global with zero-init.
BEZEL_TEST(codegen_generic_static_sameclass_715,
           literal_seed_qualified_access_int_spec_22_13) {
    BEZEL_EXPECT_EQ(run(
        std::string(kLiteralSeed) +
        "class Program "
        "  public static method Main(): int "
        "    return Box<int>.doubled; "
        "  end Main; "
        "end Program;"), 10);
}

BEZEL_TEST(codegen_generic_static_sameclass_715,
           literal_seed_qualified_access_string_spec_22_13) {
    BEZEL_EXPECT_EQ(run(
        std::string(kLiteralSeed) +
        "class Program "
        "  public static method Main(): int "
        "    return Box<string>.doubled; "
        "  end Main; "
        "end Program;"), 10);
}

// ── Variant: chain a->b->c (all qualified with <T>) ──────────────────────────

// §22.12 / §22.13 / #715: Chain<int>.c must equal (3+1)*2 = 8.
BEZEL_TEST(codegen_generic_static_sameclass_715,
           chain_three_fields_int_c_equals_8_spec_22_13) {
    BEZEL_EXPECT_EQ(run(
        std::string(kChain) +
        "class Program "
        "  public static method Main(): int "
        "    return Chain<int>.c; "
        "  end Main; "
        "end Program;"), 8);
}

// §22.12 / §22.13 / #715: Chain<string>.c must equal (3+1)*2 = 8.
BEZEL_TEST(codegen_generic_static_sameclass_715,
           chain_three_fields_string_c_equals_8_spec_22_13) {
    BEZEL_EXPECT_EQ(run(
        std::string(kChain) +
        "class Program "
        "  public static method Main(): int "
        "    return Chain<string>.c; "
        "  end Main; "
        "end Program;"), 8);
}

// ── Regression: cross-class non-const static init (#709) still works ─────────

// §22.12 / #709 regression guard: a T-independent cross-class init
// (Helper.Compute()) must still propagate to per-inst globals after the #715
// fix.  The fix only skips creating a per-inst global when all substIds are
// null (unresolved params), so the cross-class path (which doesn't go through
// staticFieldGlobal for the SAME class) is completely unaffected.
BEZEL_TEST(codegen_generic_static_sameclass_715,
           cross_class_nonconst_init_regression_709) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Helper2 "
        "  public static method Val(): int return 42; end Val; "
        "end Helper2; "
        "class Store<T> "
        "  public static value: int := Helper2.Val(); "
        "end Store; "
        "class Program "
        "  public static method Main(): int "
        "    var a := Store<int>.value; "
        "    var b := Store<string>.value; "
        "    if a = 42 and b = 42 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end Program;"), 1);
}
