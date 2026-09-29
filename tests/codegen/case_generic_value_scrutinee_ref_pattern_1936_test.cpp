// fix #1936 / §19.3 / §19.5: a typed binding pattern `var x: T` whose type T is a
// REFERENCE type (class, interface, or built-in exception) against a VALUE-typed
// scrutinee can never match. The concrete-scrutinee case is rejected up front by
// sema (#1203/#1936, see the sema regression test).
//
// A MONOMORPHIZED type-parameter scrutinee — `case t of var x: IOException` in a
// generic body, instantiated at `T = int` — is decided at the INSTANTIATION SITE
// by the §3.6 rule 2 per-instantiation join (fix #5176 / §19.3 v0.310, ruling
// #5156). Before that ruling the arm was admitted by sema and only the codegen
// path was guarded: a `!scrut->getType()->isPointerTy() → getInt1(false)` gate
// in patternMatches (LlvmBackend_Exprs2.cpp) defended against
//   * a built-in exception pattern that left `case.end` unterminated (the
//     typed-binding built-in-exception branch was guarded on a POINTER
//     scrutinee; a non-pointer value fell through to `return nullptr`); and
//   * a class/interface pattern that reached emitIsA, which ICmp'd the
//     non-pointer scrutinee against a null pointer (ICmpInst assertion abort).
// Under the v0.311 ruling those rows are REJECTED at the instantiation site —
// pattern position never performs an implicit box of the scrutinee, so a value
// instantiation can never produce a reference-typed binding target. The 1936
// test is updated to assert the rejection; the codegen guards remain for any
// case the §3.6 join does not yet cover (interface/class patterns admitted at
// reference substitutions, and the pre-#5176 source). Under `runAot` a compile
// reject surfaces as r.ran == false + r.diagnostics.hasErrors().

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §19.3/§19.5 (#1936, v0.311 ruling, #5176): generic scrutinee `t: T` at T=int
// against a BUILT-IN exception typed binding. The §3.6 per-instantiation join
// REJECTS at the instantiation site — pattern position never boxes the
// scrutinee, so the binding target is unproducible from a value instantiation.
BEZEL_TEST(case_generic_value_scrutinee_ref_pattern_1936,
           generic_int_vs_builtin_exception_rejected_at_site) {
    auto r = runAot(
        "module M; class Program "
        "  public static method Pick<T>(t: T): int "
        "    return (case t of var x: IOException => 1, _ => 9); "
        "  end Pick; "
        "  public static method Main(): int "
        "    return Program.Pick<int>(5); "
        "  end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "reference-typed binding"));
}

// §19.3/§19.5 (#1936, v0.311 ruling, #5176): generic scrutinee at T=int against
// a CLASS typed binding. Per-instantiation rejection.
BEZEL_TEST(case_generic_value_scrutinee_ref_pattern_1936,
           generic_int_vs_class_rejected_at_site) {
    auto r = runAot(
        "module M; "
        "class C public method M(): int return 0; end M; end C; "
        "class Program "
        "  public static method Pick<T>(t: T): int "
        "    return (case t of var x: C => 1, _ => 9); "
        "  end Pick; "
        "  public static method Main(): int "
        "    return Program.Pick<int>(5); "
        "  end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "reference-typed binding"));
}

// §19.3/§19.5 (#1936, v0.311 ruling, #5176): generic scrutinee at T=int against
// an INTERFACE typed binding. Per-instantiation rejection.
BEZEL_TEST(case_generic_value_scrutinee_ref_pattern_1936,
           generic_int_vs_interface_rejected_at_site) {
    auto r = runAot(
        "module M; "
        "interface I method M(): int; end I; "
        "class Program "
        "  public static method Pick<T>(t: T): int "
        "    return (case t of var x: I => 1, _ => 9); "
        "  end Pick; "
        "  public static method Main(): int "
        "    return Program.Pick<int>(5); "
        "  end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "reference-typed binding"));
}

// Control (#1936, no over-rejection): the SAME generic form with a VALUE pattern
// of the instantiated type matches — `case t of var x: int` at T=int binds x=5
// and returns it. Confirms the guard is specific to REFERENCE patterns and does
// not suppress a legitimate value-type match.
BEZEL_TEST(case_generic_value_scrutinee_ref_pattern_1936,
           generic_int_vs_int_matches) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Program "
        "  public static method Pick<T>(t: T): int "
        "    return (case t of var x: int => x, _ => 9); "
        "  end Pick; "
        "  public static method Main(): int "
        "    return Program.Pick<int>(7); "
        "  end Main; end Program;"), 7);
}

// Control (#1936, pointer path intact): a generic instantiated at a REFERENCE
// type — the scrutinee IS a pointer, so the guard is skipped and the real
// runtime is-a runs. At T=Dog, `case t of var x: Animal` matches (a Dog is-an
// Animal) → 1. Confirms the non-pointer guard did not disturb the reference path.
BEZEL_TEST(case_generic_value_scrutinee_ref_pattern_1936,
           generic_reference_vs_base_matches) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Animal public method S(): int return 0; end S; end Animal; "
        "class Dog : Animal public method B(): int return 1; end B; end Dog; "
        "class Program "
        "  public static method Pick<T>(t: T): int "
        "    return (case t of var x: Animal => 1, _ => 9); "
        "  end Pick; "
        "  public static method Main(): int "
        "    return Program.Pick<Dog>(new Dog()); "
        "  end Main; end Program;"), 1);
}
