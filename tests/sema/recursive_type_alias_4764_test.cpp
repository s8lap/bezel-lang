// fix #4764 (§27.8, v0.307): SIGSEGV on a self-referential generic type
// alias.
//
// Spec §27.8 "Generic type aliases": "A generic alias may not reference
// itself (no recursive aliases) and must resolve to a concrete type after
// substitution." The compiler had no cycle guard on the alias-transparency
// arm of `resolveNamed` (TypeResolution_Names.cpp): resolving an alias's
// body recurses into `resolveType(aliasedType, ...)`, and for
// `type Loop<T> := Loop<T>;` that body IS a reference back to the same
// alias, so the recursion has no base case — a stack overflow (SIGSEGV,
// `rc=139`), not a diagnostic.
//
// MEASURED before this fix, all three shapes below crash the same way
// (confirmed by direct `bezelc -c` invocation, not merely inferred):
//   - the issue's own repro, `type Loop<T> := Loop<T>;`      -> SIGSEGV
//   - the non-generic self-cycle, `type Loop := Loop;`        -> SIGSEGV
//   - the non-generic mutual cycle, `type A := B; type B := A;` -> SIGSEGV
// So the fix is NOT scoped to the generic case the issue titles: the
// alias-transparency arm is the ONE code path every alias reference goes
// through regardless of arity, and the cycle guard added there closes all
// three shapes (and the generic mutual-cycle shape below) at once. Spec
// §27.8's ban is stated in the "Generic type aliases" subsection, but the
// base definition a few lines above — "A type alias introduces an alias for
// any EXISTING type" / "a pure synonym" — already excludes a self- or
// mutually-referential body (there is no existing type to alias), so citing
// §27.8 for the non-generic shapes is the section's own general rule, not a
// new interpretation; flagged here per CLAUDE.md's decline-with-reasons
// discipline since the section does not use the word "recursive" outside
// the generic subsection.
//
// THE FIX (src/sema/TypeResolution_Names.cpp, src/sema/TypeResolution_impl.hpp):
// `Resolver::aliasResolutionStack_` tracks the `ast::TypeAlias*` declarations
// currently being expanded on THIS call stack — pushed on entry to the
// alias-transparency arm, popped via RAII on every exit (including the
// arity-mismatch early return). This is a true ON-STACK guard (like the
// existing `checkCycleFrom`'s onStack/done split for inheritance cycles),
// not a permanent "visited-once" set: it must catch a cycle of ANY length,
// so the mutual-generic-cycle case below (`Ping<T> := Pong<T>; Pong<T> :=
// Ping<T>`) is included specifically to prove a same-alias-only guard (which
// would never revisit `Ping` at the top of a 2-step cycle) is not what was
// shipped.
//
// Test oracle: every reject row uses `failedAtCompileTimeWithRealLocation`
// (excludes a crash/signal/timeout outcome) AND a diagnostic-text check for
// the specific §27.8 wording this fix introduces ("is recursive") — NOT
// `failedAtCompileTimeWithRealLocation` alone, which the pre-fix SIGSEGV's
// own backstop diagnostic could otherwise satisfy vacuously if the crash
// happened to carry a real location.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// ---------------------------------------------------------------------------
// REJECT: every cyclic shape must be diagnosed, never crash.
// ---------------------------------------------------------------------------

// The issue's own repro, verbatim (§27.8's own example shape): a generic
// alias referencing itself with the same type argument.
BEZEL_TEST(recursive_type_alias_4764, generic_self_cycle_rejected) {
    auto r = runAot(
        "module M; "
        "type Loop4764a<T> := Loop4764a<T>; "
        "public static class Program4764a "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "type alias `Loop4764a` is recursive"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "27.8"));
}

// §27.8's general "pure synonym for an existing type" rule extended to the
// non-generic shape: a plain alias whose body is a reference to itself.
BEZEL_TEST(recursive_type_alias_4764, nongeneric_self_cycle_rejected) {
    auto r = runAot(
        "module M; "
        "type Loop4764b := Loop4764b; "
        "public static class Program4764b "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "type alias `Loop4764b` is recursive"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "27.8"));
}

// The non-generic MUTUAL cycle named explicitly in the issue
// ("Worth checking the non-generic form and mutual recursion ... in the
// same fix"): `A := B; B := A`. Neither alias directly references itself,
// so a guard keyed only on "does this alias reference itself" would miss
// this shape entirely — it requires an on-stack (not same-node) check.
BEZEL_TEST(recursive_type_alias_4764, nongeneric_mutual_cycle_rejected) {
    auto r = runAot(
        "module M; "
        "type MutA4764c := MutB4764c; "
        "type MutB4764c := MutA4764c; "
        "public static class Program4764c "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "is recursive"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "27.8"));
}

// The GENERIC mutual cycle: `Ping<T> := Pong<T>; Pong<T> := Ping<T>`. This
// is the case that specifically distinguishes a real on-stack cycle guard
// from a same-declaration-only self-reference check (which this fix's first
// draft could plausibly have shipped instead, since the issue's own title
// names only the direct self-cycle) — a guard that only ever compared
// `aliasDecl == thisAlias` would recurse forever here (Ping -> Pong -> Ping
// -> Pong -> ...) because neither alias, taken alone, references itself.
BEZEL_TEST(recursive_type_alias_4764, generic_mutual_cycle_rejected) {
    auto r = runAot(
        "module M; "
        "type Ping4764d<T> := Pong4764d<T>; "
        "type Pong4764d<T> := Ping4764d<T>; "
        "public static class Program4764d "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "is recursive"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "27.8"));
}

// ---------------------------------------------------------------------------
// ACCEPT: a legitimate, non-cyclic generic alias-of-alias chain must keep
// compiling and running — the cycle guard pushes/pops per reference, so it
// must not misfire on the ordinary "aliases of aliases are permitted"
// (§27.8) shape it shares code with.
// ---------------------------------------------------------------------------

BEZEL_TEST(recursive_type_alias_4764,
           noncyclic_generic_alias_chain_still_accepted) {
    auto r = runAot(
        "module M; "
        "type Outer4764e<T> := Inner4764e<T>; "
        "type Inner4764e<T> := Array<T>; "
        "public static class Program4764e "
        "  public static method Main(): int "
        "    var a: Outer4764e<int> := new Array<int>(3, 0); "
        "    return a.Count; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}
