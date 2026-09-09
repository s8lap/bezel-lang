// fix #4113 — a choice's PROPERTY whose body is `(case this of …)` IR-
// verification-crashed codegen ("Basic Block ... does not have
// terminator!"), degrading through the #2491 backstop to a zero return
// instead of compiling correctly (spec §35.2/§35.5, v0.295).
//
// ── ROOT CAUSE (traced through the codegen call graph, not guessed) ───────
// `choiceTypeOf` (LlvmBackend_Setup.cpp) recovers a bare `this`/`base`
// identifier's choice type — sema attaches no exprType to `this` inside a
// named-type member body (fix #2047) — by finding the currently-emitting
// METHOD: either `currentFn_` is registered in `methodFns_` (a real
// MethodSymbol's function), or `currentSpecMethodSymbol_` names one. Both
// checks answer correctly for a genuine choice INSTANCE METHOD body.
//
// A choice PROPERTY's getter is emitted through THREE different call
// sites, and NONE of them is a MethodSymbol's function:
//   1. An expression-bodied getter is INLINED at each read site
//      (LlvmBackend_Exprs.cpp) — `currentFn_` is the CALLER's function
//      (e.g. `main`) while the getter body runs.
//   2. A property read as a function value builds a small forwarding thunk
//      (`makePropertyThunk`, LlvmBackend_Layout.cpp) — `currentFn_` is that
//      thunk (named `pthunk$N`), never a MethodSymbol's function.
//   3. A property implementing an INTERFACE member gets a real standalone
//      accessor function built once for itable dispatch
//      (`ensurePropertyAccessorFn`, LlvmBackend_Assign.cpp, named
//      `bz$<Choice>$<Prop>$propget`) — also never registered in
//      `methodFns_` (it has no MethodSymbol of its own).
// At all three, `choiceTypeOf("this")` found nothing, `patternMatches`
// could not recognize the `Circle(_)`/`Square(_)` choice-case patterns
// against an unknown scrutinee type, and `lowerCaseExpr` bailed out
// returning nullptr — leaving the already-created `case.arm`/`case.next`/
// `case.end` basic blocks orphaned in the function with no terminator, the
// exact IR-verifier failure this issue reports.
//
// Each of the three sites ALREADY has an identical, working precedent for
// an ENUM receiver: `currentEnumMethod_` is saved, set to the enum
// TypeSymbol, and restored around the getter body's evaluation (CG30 /
// fix #574 / fix #2501), because `enumTypeOf("this")` has the exact same
// "this has no exprType" recovery problem and the exact same "not a real
// method function" gap at these three sites. No analogous variable existed
// for Choice.
//
// ── THE FIX ────────────────────────────────────────────────────────────
// A new `currentChoiceMethod_` member (LlvmBackend_impl.hpp) mirrors
// `currentEnumMethod_` exactly. `choiceTypeOf` (LlvmBackend_Setup.cpp)
// consults it as a THIRD fallback (after the existing `methodFns_` /
// `currentSpecMethodSymbol_` checks) when recovering `this`/`base`. All
// three call sites above now save/set/restore it around the getter body's
// evaluation, exactly parallel to their existing `currentEnumMethod_`
// handling.
//
// ── MEASURED (isolated pre-fix worktree at the #4114 fix commit, so these
// numbers isolate #4113 alone; --version confirmed, no -modified suffix) ──
// Pre-fix: the issue's own minimal repro, a generic-choice variant, a
// property-read-as-function-value variant, AND an interface-implementing
// variant all IR-verification-crash — the last one crashes in BOTH the
// inlined call site AND the separately-built `propget` accessor function
// at once (two "does not have terminator!" reports in one compile),
// confirming site 1 and site 3 are independently reachable. All four
// compile and run correctly post-fix. An enum property with the identical
// `case this of` shape, and a choice METHOD with the identical body
// (the issue's own documented workaround), are unaffected on both sides —
// confirming the defect and the fix are choice-property-specific.
//
// ── WHAT EACH ROW IS FOR ───────────────────────────────────────────────
//   T1  the issue's own minimal repro (§35.2/§35.5): a non-generic choice's
//       expression-bodied property, body `(case this of …)`. Site 1.
//   T2  same shape, GENERIC choice — a second, independently-measured
//       pre-fix crash (see the issue's own narrowing notes, which found
//       the generic case crashes too, unlike #4114's generic case).
//   T3  property READ AS A FUNCTION VALUE (`var f: () -> bool := s.P;`) —
//       site 2 (`makePropertyThunk`), a DIFFERENT crashing function name
//       (`pthunk$N`) than T1's `main`, confirming this is an
//       independently-reached call site, not incidentally fixed by T1's.
//   T4  the choice ALSO implements an interface declaring the same
//       property — site 3 (`ensurePropertyAccessorFn`'s
//       `bz$<Choice>$<Prop>$propget`), measured to crash SEPARATELY from
//       (and simultaneously with) site 1's inlined read in the same
//       compile.
//   T5  ACCEPT-SIDE CONTROL: an enum property with the identical
//       `case this of` shape is unaffected — pins that `currentEnumMethod_`
//       and its call sites are untouched by this fix.
//   T6  ACCEPT-SIDE CONTROL: a choice METHOD (not a property) with the
//       identical `case this of` body — the issue's own documented
//       workaround — keeps compiling and running correctly.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::diagnosticMessageContains;

// ── T1 — the issue's own minimal repro (§35.2/§35.5). Pre-fix: IR
// verification fails ("Basic Block ... does not have terminator!"),
// degraded through the #2491 backstop to a zero/null return.
BEZEL_TEST(codegen_choice_property_case_this_crash_4113,
           nongeneric_choice_expression_property_case_this_body_compiles_and_runs_spec_35_2_35_5) {
    auto r = runAot(
        "module M4113A; choice Shape4113A "
        "  Circle4113A(radius: double), Square4113A(side: double); "
        "  public property IsCircle: bool => "
        "    (case this of "
        "       Circle4113A(_) => true, "
        "       Square4113A(_) => false); "
        "end Shape4113A; "
        "class C "
        "  public static method Main(): int "
        "    var s := Shape4113A.Circle4113A(2.0); "
        "    return (if s.IsCircle then 42 else 1); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "does not have terminator"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "internal:"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ── T2 — same shape, GENERIC choice. The issue's own narrowing notes say
// this crashes too (unlike #4114, where the generic+trivial-body cell was
// spared) — measured independently here rather than assumed.
BEZEL_TEST(codegen_choice_property_case_this_crash_4113,
           generic_choice_expression_property_case_this_body_compiles_and_runs_spec_35_2_35_5_35_11) {
    auto r = runAot(
        "module M4113B; choice Shape4113B<T> "
        "  Circle4113B(radius: double), Square4113B(side: T); "
        "  public property IsCircle: bool => "
        "    (case this of "
        "       Circle4113B(_) => true, "
        "       Square4113B(_) => false); "
        "end Shape4113B; "
        "class C "
        "  public static method Main(): int "
        "    var s: Shape4113B<double> := Shape4113B<double>.Circle4113B(2.0); "
        "    return (if s.IsCircle then 42 else 1); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "does not have terminator"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "internal:"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ── T3 — property read AS A FUNCTION VALUE: makePropertyThunk builds a
// standalone forwarding thunk (`pthunk$N`), a THIRD distinct function that
// never registers in methodFns_ — independently reached from T1's inlined
// call site.
BEZEL_TEST(codegen_choice_property_case_this_crash_4113,
           choice_property_read_as_function_value_case_this_body_compiles_and_runs_spec_35_2_35_5_27_6) {
    auto r = runAot(
        "module M4113C; choice Shape4113C "
        "  Circle4113C(radius: double), Square4113C(side: double); "
        "  public property IsCircle: bool => "
        "    (case this of "
        "       Circle4113C(_) => true, "
        "       Square4113C(_) => false); "
        "end Shape4113C; "
        "class C "
        "  public static method Main(): int "
        "    var s := Shape4113C.Circle4113C(2.0); "
        "    var f: () -> bool := s.IsCircle; "
        "    return (if f() then 42 else 1); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "does not have terminator"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ── T4 — the choice ALSO implements an interface declaring the same
// property: ensurePropertyAccessorFn builds a real standalone itable
// accessor (`bz$Shape4113D$IsCircle$propget`). Measured pre-fix to crash
// there AND at the direct inlined read simultaneously, in one compile —
// confirming sites 1 and 3 are independently reachable, not one masking
// the other.
BEZEL_TEST(codegen_choice_property_case_this_crash_4113,
           interface_implementing_choice_property_case_this_body_compiles_and_runs_spec_35_2_35_5_35_8) {
    auto r = runAot(
        "module M4113D; "
        "interface IShape4113D readonly property IsCircle: bool; end IShape4113D; "
        "choice Shape4113D: IShape4113D "
        "  Circle4113D(radius: double), Square4113D(side: double); "
        "  public property IsCircle: bool => "
        "    (case this of "
        "       Circle4113D(_) => true, "
        "       Square4113D(_) => false); "
        "end Shape4113D; "
        "class C "
        "  public static method Main(): int "
        "    var s := Shape4113D.Circle4113D(2.0); "
        "    return (if s.IsCircle then 42 else 1); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "does not have terminator"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ── T5 — ACCEPT-SIDE CONTROL: an enum property with the IDENTICAL
// `case this of` shape is unaffected (pins that currentEnumMethod_ and its
// three call sites are untouched by this fix).
BEZEL_TEST(codegen_choice_property_case_this_crash_4113,
           enum_property_case_this_body_unaffected_spec_10_5_accept_control) {
    auto r = runAot(
        "module M4113E; enum Color4113E "
        "  Red4113E, Green4113E, Blue4113E; "
        "  public property IsRed: bool => "
        "    (case this of "
        "       Red4113E => true, Green4113E => false, Blue4113E => false); "
        "end Color4113E; "
        "class C "
        "  public static method Main(): int "
        "    var c := Color4113E.Red4113E; "
        "    return (if c.IsRed then 42 else 1); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "does not have terminator"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ── T6 — ACCEPT-SIDE CONTROL: a choice METHOD (not a property) with the
// identical `case this of` body — the issue's own documented workaround —
// keeps compiling and running correctly (choiceTypeOf's pre-existing
// methodFns_ recovery, #2047, unaffected by this fix).
BEZEL_TEST(codegen_choice_property_case_this_crash_4113,
           choice_method_case_this_body_unaffected_spec_35_2_35_5_accept_control) {
    auto r = runAot(
        "module M4113F; choice Shape4113F "
        "  Circle4113F(radius: double), Square4113F(side: double); "
        "  public method IsCircle(): bool => "
        "    (case this of "
        "       Circle4113F(_) => true, "
        "       Square4113F(_) => false); "
        "end Shape4113F; "
        "class C "
        "  public static method Main(): int "
        "    var s := Shape4113F.Circle4113F(2.0); "
        "    return (if s.IsCircle() then 42 else 1); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "does not have terminator"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}
