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

// ---------------------------------------------------------------------------
// fix #6329 (§27.8, v0.338): the on-stack cycle guard must not fire on a
// NESTED INSTANTIATION of a legitimate alias at a USE site.
//
// §27.8 scopes the self-reference ban to the DECLARATION: "An alias may not
// reference itself, directly or through other aliases, in its aliased type or
// in its `where` clause" (and, for generics, "A generic alias is subject to
// the self-reference rule above"). The rule constrains what a `type`
// declaration may name — it is NOT a use-site rule, and it is not a
// same-name-mention rule. `type Id<T> := T;` mentions `Id` nowhere in its
// aliased type, so `Id` is a well-formed alias; at a use site, `Id<Id<int>>`
// names `Id` only in an ARGUMENT position, and expands to `int` with no
// cycle — §27.8's "each instantiation of the alias is interchangeable with
// the corresponding instantiation of the aliased type" makes it a synonym
// for `int`.
//
// The guard added by #4764 pushed `aliasDecl` on entry to the
// alias-transparency arm and popped it on exit, but the arm ALSO resolves
// this reference site's own generic arguments while `aliasDecl` is still
// pushed — so the inner `Id<int>` of `Id<Id<int>>` re-entered the guarded
// arm, found `Id` already on the stack, and was rejected as
// "type alias `Id` is recursive (§27.8)" (issue #6329's repro, verbatim).
// Argument resolution is a SIBLING subtree, not an expansion of the alias
// body, so the fix pops the alias for its duration and restores it on exit.
// A cycle is only reachable through an alias BODY, so this cannot hide one:
// every alias reached inside the (finite) argument subtree is pushed again on
// entry, and a cycle closing back on this alias is caught at the next push.
//
// Deliberately NOT changed: the `where`-clause self-bound, which §27.8 bans
// explicitly ("or in its `where` clause") and which
// `recursive_type_alias_diag_4784.where_clause_self_bound_current_wording_
// pinned` already pins. The constraint-validation path keeps the alias on the
// stack.
// ---------------------------------------------------------------------------

// The issue's own repro, verbatim: `Id<Id<int>>` in a local's declared type.
BEZEL_TEST(recursive_type_alias_4764, nested_generic_use_site_accepted_6329) {
    auto r = runAot(
        "module M; "
        "type Id6329a<T> := T; "
        "public static class Program6329a "
        "  public static method Main(): int "
        "    var v: Id6329a<Id6329a<int>> := 3; "
        "    Console.WriteLine(v); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n");
}

// The same nested instantiation three deep: each level re-enters the guarded
// arm once more, so this row is the one that would still fail if the fix
// popped the alias only one level deep.
BEZEL_TEST(recursive_type_alias_4764, triple_nested_generic_use_accepted_6329) {
    auto r = runAot(
        "module M; "
        "type Id6329b<T> := T; "
        "public static class Program6329b "
        "  public static method Main(): int "
        "    var v: Id6329b<Id6329b<Id6329b<int>>> := 3; "
        "    Console.WriteLine(v); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n");
}

// The nested instantiation is not special to a local's declared type: it must
// also resolve in a method RETURN type, in a PARAMETER type, and nested
// inside another generic's argument list — three different reference contexts
// that each re-enter the alias-transparency arm through a different path.
BEZEL_TEST(recursive_type_alias_4764, nested_generic_use_contexts_accepted_6329) {
    auto r = runAot(
        "module M; "
        "type Id6329c<T> := T; "
        "public static class Program6329c "
        "  public static method R(): Id6329c<Id6329c<int>> => 3; "
        "  public static method Take(x: Id6329c<Id6329c<int>>): int => x; "
        "  public static method Main(): int "
        "    var a: Array<Id6329c<Id6329c<int>>> := [R()]; "
        "    Console.WriteLine(Take(a[0])); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n");
}

// The other side of the fix: popping the alias for argument resolution must
// not ACCEPT a real cycle that is still reachable through an alias BODY. This
// row puts a legitimate nested instantiation AND a self-referential body in
// ONE program — so it also pins that the new accept path does not mask the
// §27.8 rejection in a file that exercises both.
BEZEL_TEST(recursive_type_alias_4764, cycle_still_rejected_beside_nested_use_6329) {
    auto r = runAot(
        "module M; "
        "type Id6329d<T> := T; "
        "type Loop6329d<T> := Loop6329d<T>; "
        "public static class Program6329d "
        "  public static method Main(): int "
        "    var v: Id6329d<Id6329d<int>> := 3; "
        "    var w: Loop6329d<int> := 1; "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r,
                                                "type alias `Loop6329d` is recursive"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "27.8"));
}
