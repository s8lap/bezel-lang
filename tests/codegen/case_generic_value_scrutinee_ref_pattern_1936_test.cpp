// fix #1936 / §19.3 / §19.5: a typed binding pattern `var x: T` whose type T is a
// REFERENCE type (class, interface, or built-in exception) against a VALUE-typed
// scrutinee can never match. The concrete-scrutinee case is rejected up front by
// sema (#1203/#1936, see the sema regression test), but a MONOMORPHIZED
// type-parameter scrutinee — `case t of var x: IOException` in a generic body,
// instantiated at `T = int` — is only decidable in codegen: sema sees the abstract
// type parameter, so the arm passes admission and reaches the backend.
//
// Without the codegen guards, that monomorphization CRASHED the compiler:
//   * a built-in exception pattern left `case.end` unterminated → malformed IR
//     (the typed-binding built-in-exception branch was guarded on a POINTER
//     scrutinee; a non-pointer value fell through to `return nullptr`);
//   * a class/interface pattern reached emitIsA, which ICmp'd the non-pointer
//     scrutinee against a null pointer → an ICmpInst assertion abort.
// Both are fixed by a `!scrut->getType()->isPointerTy() → getInt1(false)` gate in
// patternMatches (LlvmBackend_Exprs2.cpp), mirroring the bare-name NamedPattern
// path. A value-typed scrutinee cleanly NO-MATCHES (takes `_`); a value pattern of
// the same type still matches. Under `runAot` a compile abort surfaces as
// r.ran == false → run() returns -999, so these FAIL WITHOUT the fix.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §19.3/§19.5 (#1936): generic scrutinee `t: T` at T=int against a BUILT-IN
// exception typed binding. Previously malformed IR (unterminated case.end); now
// cleanly no-matches → the `_` arm → 9.
BEZEL_TEST(case_generic_value_scrutinee_ref_pattern_1936,
           generic_int_vs_builtin_exception_no_match) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Program "
        "  public static method Pick<T>(t: T): int "
        "    return case t of var x: IOException => 1, _ => 9 end; "
        "  end Pick; "
        "  public static method Main(): int "
        "    return Program.Pick<int>(5); "
        "  end Main; end Program;"), 9);
}

// §19.3/§19.5 (#1936): generic scrutinee at T=int against a CLASS typed binding.
// Previously an ICmpInst assertion abort in emitIsA; now cleanly no-matches → 9.
BEZEL_TEST(case_generic_value_scrutinee_ref_pattern_1936,
           generic_int_vs_class_no_match) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public method M(): int return 0; end M; end C; "
        "class Program "
        "  public static method Pick<T>(t: T): int "
        "    return case t of var x: C => 1, _ => 9 end; "
        "  end Pick; "
        "  public static method Main(): int "
        "    return Program.Pick<int>(5); "
        "  end Main; end Program;"), 9);
}

// §19.3/§19.5 (#1936): generic scrutinee at T=int against an INTERFACE typed
// binding. Same emitIsA crash class; now cleanly no-matches → 9.
BEZEL_TEST(case_generic_value_scrutinee_ref_pattern_1936,
           generic_int_vs_interface_no_match) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface I method M(): int; end I; "
        "class Program "
        "  public static method Pick<T>(t: T): int "
        "    return case t of var x: I => 1, _ => 9 end; "
        "  end Pick; "
        "  public static method Main(): int "
        "    return Program.Pick<int>(5); "
        "  end Main; end Program;"), 9);
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
        "    return case t of var x: int => x, _ => 9 end; "
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
        "    return case t of var x: Animal => 1, _ => 9 end; "
        "  end Pick; "
        "  public static method Main(): int "
        "    return Program.Pick<Dog>(new Dog()); "
        "  end Main; end Program;"), 1);
}
