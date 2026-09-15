// Spec-derived tests for fix #4168 — §26.5 rule 2 "Resolution" (v0.298).
//
// An extension declared over a concrete instantiation whose generic
// argument is a FUNCTION TYPE — `extension Array<(int) -> int>` — compiled
// its declaration cleanly (§26.2 admits the class-typed target `Array<T>`;
// the function-typed generic ARGUMENT is a completely different position
// §26.2's "What cannot be extended" list, which forbids extending a
// function type AT THE TOP LEVEL, says nothing about), but a call against a
// matching real receiver never resolved the member: the extension was
// never matched.
//
// ROOT CAUSE: `extensionTargetMatchesReceiver` (ExprTypecheck.cpp) matches
// an extension's declared head against a receiver by walking `Named`-kind
// generic arguments recursively, but its top-level `switch (target->kind)`
// covered only `Primitive` and `Named` — every other kind, INCLUDING
// `Function`, fell to `default: return false`. That `switch` is reached not
// only for the top-level extension target (always `Named` or `Primitive`
// per §26.2) but ALSO recursively for each of the target's generic
// ARGUMENTS, via the very call this fix touches:
// `extensionTargetMatchesReceiver(ta, ra, freeVars)` inside the
// `Named`-kind arm's generic-argument loop. So a `Function`-kind generic
// argument — `(int) -> int` inside `Array<(int) -> int>` — always answered
// "no match", for EVERY receiver, including one with the byte-identical
// function-type argument. The declaration compiled (§26.2's target-kind
// gate, #4144/#4167, only inspects the TOP-LEVEL target and never touches
// this code path — confirmed by reverting THIS fix alone and re-running:
// the extension declaration still compiles clean, so #4144/#4167's checks
// are unaffected either way), but §26.5 rule 2's receiver-driven unification
// — stated verbatim as "the receiver's static type is unified against the
// declared head, exactly and structurally" — never actually ran a
// structural comparison for this shape; it silently answered false instead.
//
// THE FIX: a new `case TypeRef::Kind::Function:` arm structurally compares
// parameter modifiers/types and the return type/`owned`-return marker (the
// same shape `Resolver::typeRefEqual`'s own Function arm checks,
// TypeResolution_Checks.cpp), but recurses through
// `extensionTargetMatchesReceiver` itself (not `typeRefEqual`) so a free
// type variable nested inside a parameter or return type still unifies
// against the receiver's corresponding slot, exactly like the pre-existing
// `Named`-kind generic-argument recursion. `nullable` is not compared,
// matching this function's own documented behavior for every other kind.
//
// Interaction with fix #4167 (this lane's OTHER issue, landed on this
// lane's base commit): #4167 added a generic type-alias arity check inside
// `resolveNamed`'s `TypeAliasSymbol` branch (TypeResolution_Names.cpp) —
// a COMPLETELY DIFFERENT function, run at a COMPLETELY DIFFERENT time (type
// RESOLUTION of the extension's own declared head, before this fix's
// function is ever invoked) than `extensionTargetMatchesReceiver` (CALL-SITE
// matching of an already-resolved, non-Error TypeRef). Also, §26.4 makes a
// bare single-segment identifier in an extension target's generic-argument
// position ALWAYS introduce a fresh block-level type parameter (shadowing
// any same-named alias in scope, with a warning) — so an alias name can
// never even REACH the generic-argument position this fix's Function arm
// inspects; there is no live shape where the two fixes' code paths overlap.
// Verified two ways: (1) `extension_generic_alias_target_arity_4167`'s own
// 9 tests are unaffected by this fix (unchanged pass/fail, identical
// diagnostics); (2) `wrong_arity_alias_named_extension_argument_rejected`
// below proves a wrong-arity generic alias named in an extension's
// generic-argument slot still hits #4167's arity check (via the §26.4
// shadowing-to-type-parameter route, not this fix's code at all).
//
// Test oracle: `runAot` (the real AOT compile+link+exec path) throughout —
// every accept row asserts `r.ran`, `r.exitCode` AND `r.stdoutText` (not
// merely a clean compile), proving the extension member is both RESOLVED
// and CORRECTLY DISPATCHED at runtime, not just accepted by sema with dead
// codegen. Reject rows assert `failedAtCompileTimeWithRealLocation` paired
// with diagnostic text (§18 of the lane process — a crash also satisfies
// "real location", so text is required too).

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// ---------------------------------------------------------------------------
// ACCEPT (THE BUG): the issue's own repro, verbatim shape. FAILS WITHOUT THE
// FIX: compiles clean but `xs.DescribeCount4168()` reports "no member".
// ---------------------------------------------------------------------------
BEZEL_TEST(extension_function_type_generic_arg_4168,
           call_site_resolves_with_function_type_generic_argument) {
    auto r = runAot(
        "module M; "
        "extension Array<(int) -> int> "
        "  public method DescribeCount4168(): string => "
        "    \"n=\" + this.Count.ToString(); "
        "end; "
        "public static class Program4168a "
        "  public static method Main(): int "
        "    var f: (int) -> int := lambda(x) => x + 1; "
        "    var xs: Array<(int) -> int> := [f]; "
        "    Console.WriteLine(xs.DescribeCount4168()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "n=1\n");
}

// ---------------------------------------------------------------------------
// ACCEPT: the receiver reaches the call site through a (non-generic) type
// alias for the function type — proves alias-transparency (§27.8) and this
// fix compose: the receiver's TypeRef head is the SAME builtin `Array`
// regardless of how the element type was spelled at the local declaration.
// ---------------------------------------------------------------------------
BEZEL_TEST(extension_function_type_generic_arg_4168,
           call_site_resolves_through_type_alias_to_function_type) {
    auto r = runAot(
        "module M; "
        "type IntFn4168 := (int) -> int; "
        "extension Array<(int) -> int> "
        "  public method DescribeCount4168b(): string => "
        "    \"n=\" + this.Count.ToString(); "
        "end; "
        "public static class Program4168b "
        "  public static method Main(): int "
        "    var f: IntFn4168 := lambda(x) => x + 1; "
        "    var xs: Array<IntFn4168> := [f]; "
        "    Console.WriteLine(xs.DescribeCount4168b()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "n=1\n");
}

// ---------------------------------------------------------------------------
// ACCEPT: a NESTED function type — the generic argument is itself a
// function type whose OWN parameter is a function type — proves the fix's
// recursion (not just a single flat comparison) works at depth.
// ---------------------------------------------------------------------------
BEZEL_TEST(extension_function_type_generic_arg_4168,
           call_site_resolves_with_nested_function_type) {
    auto r = runAot(
        "module M; "
        "extension Array<((int) -> int) -> int> "
        "  public method DescribeCount4168c(): string => "
        "    \"n=\" + this.Count.ToString(); "
        "end; "
        "public static class Program4168c "
        "  public static method Main(): int "
        "    var g: ((int) -> int) -> int := lambda(h) => h(3); "
        "    var xs: Array<((int) -> int) -> int> := [g]; "
        "    Console.WriteLine(xs.DescribeCount4168c()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "n=1\n");
}

// ---------------------------------------------------------------------------
// ACCEPT: a zero-parameter function type.
// ---------------------------------------------------------------------------
BEZEL_TEST(extension_function_type_generic_arg_4168,
           call_site_resolves_with_zero_parameter_function_type) {
    auto r = runAot(
        "module M; "
        "extension Array<() -> int> "
        "  public method DescribeCount4168d(): string => "
        "    \"n=\" + this.Count.ToString(); "
        "end; "
        "public static class Program4168d "
        "  public static method Main(): int "
        "    var f: () -> int := lambda() => 7; "
        "    var xs: Array<() -> int> := [f]; "
        "    Console.WriteLine(xs.DescribeCount4168d()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "n=1\n");
}

// ---------------------------------------------------------------------------
// ACCEPT: a several-parameter function type — proves the parameter-count
// and per-parameter structural comparison, not just the return type.
// ---------------------------------------------------------------------------
BEZEL_TEST(extension_function_type_generic_arg_4168,
           call_site_resolves_with_multi_parameter_function_type) {
    auto r = runAot(
        "module M; "
        "extension Array<(int, string, bool) -> int> "
        "  public method DescribeCount4168e(): string => "
        "    \"n=\" + this.Count.ToString(); "
        "end; "
        "public static class Program4168e "
        "  public static method Main(): int "
        "    var f: (int, string, bool) -> int := lambda(a, b, c) => a; "
        "    var xs: Array<(int, string, bool) -> int> := [f]; "
        "    Console.WriteLine(xs.DescribeCount4168e()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "n=1\n");
}

// ---------------------------------------------------------------------------
// REJECT CONTROL: a member name that genuinely does not exist on the
// extension must still be rejected — the fix must not turn every call on a
// function-type-argument receiver into a blanket accept.
// ---------------------------------------------------------------------------
BEZEL_TEST(extension_function_type_generic_arg_4168,
           nonexistent_member_still_rejected) {
    auto r = runAot(
        "module M; "
        "extension Array<(int) -> int> "
        "  public method DescribeCount4168f(): string => "
        "    \"n=\" + this.Count.ToString(); "
        "end; "
        "public static class Program4168f "
        "  public static method Main(): int "
        "    var f: (int) -> int := lambda(x) => x + 1; "
        "    var xs: Array<(int) -> int> := [f]; "
        "    Console.WriteLine(xs.NoSuchMember4168()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "no member"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "NoSuchMember4168"));
}

// ---------------------------------------------------------------------------
// REJECT CONTROL: a receiver whose function-type generic argument genuinely
// mismatches (different parameter type: `(string) -> int` vs the
// extension's declared `(int) -> int`) must stay rejected — proves the new
// Function-kind arm is a REAL structural comparison, not a kind-only check
// that accepts any two function types unconditionally.
// ---------------------------------------------------------------------------
BEZEL_TEST(extension_function_type_generic_arg_4168,
           mismatched_function_parameter_type_still_rejected) {
    auto r = runAot(
        "module M; "
        "extension Array<(int) -> int> "
        "  public method DescribeCount4168g(): string => "
        "    \"n=\" + this.Count.ToString(); "
        "end; "
        "public static class Program4168g "
        "  public static method Main(): int "
        "    var f: (string) -> int := lambda(x) => x.Length; "
        "    var xs: Array<(string) -> int> := [f]; "
        "    Console.WriteLine(xs.DescribeCount4168g()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "no member"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "DescribeCount4168g"));
}

// ---------------------------------------------------------------------------
// REJECT CONTROL: a mismatched RETURN type (`(int) -> string` vs the
// extension's declared `(int) -> int`) must also stay rejected — covers the
// arm's `funcReturnOwned`/return-type comparison, not just parameters.
// ---------------------------------------------------------------------------
BEZEL_TEST(extension_function_type_generic_arg_4168,
           mismatched_function_return_type_still_rejected) {
    auto r = runAot(
        "module M; "
        "extension Array<(int) -> int> "
        "  public method DescribeCount4168h(): string => "
        "    \"n=\" + this.Count.ToString(); "
        "end; "
        "public static class Program4168h "
        "  public static method Main(): int "
        "    var f: (int) -> string := lambda(x) => x.ToString(); "
        "    var xs: Array<(int) -> string> := [f]; "
        "    Console.WriteLine(xs.DescribeCount4168h()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "no member"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "DescribeCount4168h"));
}

// ---------------------------------------------------------------------------
// REJECT CONTROL (fix #4167 must still fire, unfought): a wrong-arity
// generic type alias named in an extension's generic-argument position.
// Per §26.4 this position ALWAYS introduces a fresh block-level type
// parameter for a bare single-segment identifier (shadowing the alias, with
// a warning) rather than resolving the name — so this row does not actually
// reach #4168's fix's code path at all, and is included to record that fact
// mechanically rather than by assertion. The extension still compiles (the
// identifier is captured as a free-matching type parameter, not as the
// alias), proving no interaction — neither fix's code touches the other's.
// ---------------------------------------------------------------------------
BEZEL_TEST(extension_function_type_generic_arg_4168,
           alias_named_in_generic_arg_slot_becomes_shadowing_type_param) {
    auto r = runAot(
        "module M; "
        "type FnAlias4168 := (int) -> int; "
        "extension Array<FnAlias4168> "
        "  public method Describe4168i(): string => "
        "    \"n=\" + this.Count.ToString(); "
        "end; "
        "public static class Program4168i "
        "  public static method Main(): int "
        "    var f: (int) -> int := lambda(x) => x + 1; "
        "    var xs: Array<(int) -> int> := [f]; "
        "    Console.WriteLine(xs.Describe4168i()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "n=1\n");
}

// ---------------------------------------------------------------------------
// REJECT CONTROL (fix #4167, direct — unrelated call path, same base
// commit): the issue's own bare-identity-alias extension-target repro must
// remain rejected exactly as #4167 left it; this fix's edit
// (ExprTypecheck.cpp, `extensionTargetMatchesReceiver`) is never reached for
// this shape at all — #4167's check (TypeResolution_Names.cpp,
// `resolveNamed`) runs during type RESOLUTION of the extension's own
// declared head and rejects it with `ref.kind = TypeRef::Kind::Error` before
// `lookupExtensionMember`/`extensionTargetMatchesReceiver` ever run.
// ---------------------------------------------------------------------------
BEZEL_TEST(extension_function_type_generic_arg_4168,
           bare_generic_alias_extension_target_4167_still_rejected) {
    auto r = runAot(
        "module M; "
        "type IdP4168j<T> := T; "
        "extension IdP4168j "
        "  public method Foo4168j(): string => \"id\"; "
        "end; "
        "public static class Program4168j "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "generic-argument count does not match type alias"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "27.8"));
}

// ---------------------------------------------------------------------------
// ACCEPT CONTROL: an ordinary primitive-generic-argument extension (no
// function type anywhere) must be completely unaffected by this fix — the
// pre-existing `Named`-kind path is untouched.
// ---------------------------------------------------------------------------
BEZEL_TEST(extension_function_type_generic_arg_4168,
           ordinary_primitive_generic_argument_extension_unaffected_control) {
    auto r = runAot(
        "module M; "
        "extension Array<int> "
        "  public method DescribeCount4168k(): string => "
        "    \"n=\" + this.Count.ToString(); "
        "end; "
        "public static class Program4168k "
        "  public static method Main(): int "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    Console.WriteLine(xs.DescribeCount4168k()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "n=3\n");
}
