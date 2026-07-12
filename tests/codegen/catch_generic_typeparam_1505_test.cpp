// Codegen tests for §31.2.1 (v0.183) — a generic type parameter used as a
// catch-arm type. Issue #1505: `catch var ex: TException` silently failed to
// match and dispatch fell through to a later `Exception` arm (wrong-arm
// selection, no diagnostic).
//
// §31.2.1 (verbatim): "A generic type parameter is admissible as an arm's type
// [...] in a generic method or type, `var ex: TEx` (or bare `TEx`) matches
// exactly as the substituted type argument would [...] so an
// `Assert.Throws<TEx>`-style helper matches the concrete exception type,
// subtypes included."
//
// Root cause: catch dispatch resolved the arm's type via
// descriptorForTypeName() on the *unsubstituted* placeholder ref. A type
// parameter carries no `declaration` and no builtin-name entry, so the lookup
// returned null, the arm's type test was skipped, and `matched` stayed false —
// dispatch fell through to the bare `Exception` arm. Fix: resolve the arm type
// through resolveTypeParamRef() (the active monomorphization binding) before
// the descriptor lookup, exactly as every other body-level type-param site does.
//
// Tests (a `Throws<TEx>` helper: arm 1 binds `TEx` → returns 1, arm 2 binds the
// bare `Exception` fallback → returns 2):
//   1. Exact match — thrown type == the substituted TEx → arm 1 (returns 1).
//   2. Subtype match — thrown type derives from the substituted TEx → arm 1.
//   3. Non-match — thrown type unrelated to TEx → falls to Exception arm (2).
//   4. TEx = base Exception — every Exception-derived throwable matches arm 1.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// A generic `Throws<TEx>` helper plus a small exception hierarchy:
//   MyFault : Exception,  SubFault : MyFault,  OtherFault : Exception.
// The helper's first arm binds the type parameter, the second the bare
// Exception fallback; `Main` calls it with an explicit type argument and a
// throwing lambda so the return value reveals which arm dispatch selected.
std::string program(const char* typeArg, const char* thrownType) {
    return std::string(
        "module M; "
        "class MyFault: Exception "
        "  public constructor(msg: string) base(msg); end constructor; "
        "end MyFault; "
        "class SubFault: MyFault "
        "  public constructor(msg: string) base(msg); end constructor; "
        "end SubFault; "
        "class OtherFault: Exception "
        "  public constructor(msg: string) base(msg); end constructor; "
        "end OtherFault; "
        "static class H "
        "  public static method Throws<TEx>(action: () -> ()): int "
        "      where TEx: Exception "
        "    try "
        "      action(); "
        "    catch "
        "      var ex: TEx => do return 1; end, "
        "      var ex: Exception => do return 2; end "
        "    end; "
        "    return 0; "
        "  end Throws; "
        "end H; "
        "class C public static method Main(): int "
        "  return H.Throws<") + typeArg +
        ">(() => do throw new " + thrownType +
        "(\"boom\"); end); "
        "end Main; end C;";
}

} // namespace

// The exact issue #1505 repro: TEx = MyFault, a MyFault is thrown. Before the
// fix this returned 2 (the `Exception` arm); the substituted arm must match.
// per §31.2.1: the arm matches "exactly as the substituted type argument would".
BEZEL_TEST(codegen_1505, generic_typeparam_exact_match_spec_31_2_1) {
    BEZEL_EXPECT_EQ(run(program("MyFault", "MyFault")), 1);
}

// Subtypes of the substituted type argument match too (SubFault : MyFault).
// per §31.2.1: "the concrete exception type, subtypes included".
BEZEL_TEST(codegen_1505, generic_typeparam_subtype_match_spec_31_2_1) {
    BEZEL_EXPECT_EQ(run(program("MyFault", "SubFault")), 1);
}

// A thrown type unrelated to the substituted TEx does NOT match arm 1 and
// correctly falls through to the bare `Exception` arm (returns 2). Guards
// against an over-broad fix that would match anything.
// per §31.2.3: the first matching arm wins; non-matches propagate to the next.
BEZEL_TEST(codegen_1505, generic_typeparam_non_match_falls_through_spec_31_2_1) {
    BEZEL_EXPECT_EQ(run(program("MyFault", "OtherFault")), 2);
}

// TEx substituted to the base `Exception` itself: every Exception-derived
// throwable matches arm 1 (here an OtherFault).
BEZEL_TEST(codegen_1505, generic_typeparam_base_exception_matches_spec_31_2_1) {
    BEZEL_EXPECT_EQ(run(program("Exception", "OtherFault")), 1);
}
