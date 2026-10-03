// Spec-derived regression test for issue #5282 — a method-level `where`
// clause is not enforced when the type argument is INFERRED from the call's
// arguments.
//
// spec §14.5 ("Type-argument inference for generic methods") closing
// paragraph and §14.2 ("Constraints"), v0.313.
//
// ── THE RULE ───────────────────────────────────────────────────────────────
//
// §14.5, closing paragraph: "§14.2 constraint satisfaction is checked after
// inference completes, on the full substitution, exactly as for a written
// type-argument list." The two spellings are therefore indistinguishable to
// §14.2: `Sink.Apply(5)` and `Sink.Apply<int>(5)` propose the SAME
// substitution, and a `where U: Shape` clause must reject both.
//
// §14.2: "Constraint satisfaction is a property of the type alone, checked
// wherever an instantiation occurs" — the call site is the instantiation,
// not the declaration.
//
// ── THE GAP ────────────────────────────────────────────────────────────────
//
// `ExprTC::typeCall` splits method calls on the callee's return type:
// `md && md->returnType` (non-void) and `md && !md->returnType` (void).
// Only the non-void arm builds an ARGUMENT-INFERRED substitution and calls
// `checkMethodGenericConstraints` on it (src/sema/ExprTypecheck_Calls.cpp
// ~L5399, since #729). The void arm's inference equivalent was never
// written: #5162 wired in only the EXPLICIT `<...>`-list half of the two
// checks, because that is the half that has a substitution to consume when
// the method returns nothing.
//
// The consequence is a hole exactly where the issue filed it — a void-
// returning generic method whose type arguments are inferred, called on a
// same-module callee, accepts a substitution its own `where` clause forbids:
//
//   Sink.Apply(5);        // accepted pre-fix, and the program RUNS
//   Sink.Apply<int>(5);   // rejected pre-fix and post-fix, identically
//
// The non-void sibling was NOT silently wrong in its enforcement, only in
// its wording (see the second half of the fix below).
//
// ── THE FIX ────────────────────────────────────────────────────────────────
//
// Two parts, both in `ExprTypecheck_Calls.cpp`:
//
// 1. The void-returning arm gains the sibling's argument-inferred
//    substitution: unify each value parameter's declared type against its
//    argument (§14.5 phase 1, `unifyArgSubst`), then run
//    `checkMethodGenericConstraints` and `checkAsyncInstantiationBorrowBearing`
//    over the result. No return-type substitution applies — there is no
//    return type — so this is the sibling's inference block minus its
//    `rt = ...` line. The inline-`out var` skip (§14.5 / §22.4 / #1164) and
//    the bare-`null` skip (§14.1 / #885/#886) are carried over verbatim so
//    the two arms infer identically.
//
//    A DISAGREEING second binding (§14.5 *Agreement* / #896) makes the arm
//    SKIP the check rather than run it against the first (winning) binding:
//    that binding is not what the call proposed, so a constraint checked
//    against it would name a type the programmer never wrote. The non-void
//    arm instead reports the disagreement and abandons the call (~L5380);
//    this arm has never done that, and adding it is a separate §14.5
//    concern from this issue's where-clause gap — so a conflicting
//    inference on a void generic method is left exactly as accepted today.
//
// 2. `argNameOf` (the §14.2 diagnostic's type-argument speller) learned to
//    read a PRIMITIVE TypeRef. An argument-inferred substitution is built
//    from the argument's sema TypeRef, which for a primitive carries
//    neither `declaration` nor a name-bearing `astNode`, so the lambda
//    fell through to its generic "the type argument" fallback and the
//    non-void inferred form emitted the contentless
//    "type argument `the type argument` does not satisfy the constraint".
//    §14.2 requires the diagnostic to name "the constraint, the offending
//    type argument, and the structural reason" — an unnamed argument
//    satisfies none of that. The explicit-list path never hit this because
//    its TypeRef is resolved from a written `ast::Type` and keeps its name
//    head, which is why the two spellings disagreed in WORDING as well as
//    in enforcement.
//
// ── MEASURED on this lane's pre-fix HEAD (ccd0be14) ────────────────────────
//
//   Sink.Apply(5)          (void, static)   -> exit 0, NO diagnostic  [BUG]
//   s.Apply(5)             (void, instance) -> exit 0, NO diagnostic  [BUG]
//   Sink.Apply<int>(5)     (void, explicit) -> "type argument `int` does not
//                                         satisfy the constraint
//                                         `where U: Shape` (§14.2)"
//   Sink.Id(5)             (NON-void, inferred) -> rejected, but with
//                                         "type argument `the type
//                                         argument` does not satisfy ..."
//                                         [contentless name, part 2]
//   Sink.Id<int>(5)        (NON-void, explicit) -> "type argument `int` ..."
//
// Post-fix all four violating spellings emit the byte-identical diagnostic
// naming `int`, and the satisfying call is still accepted.
//
// ── WHAT EACH ROW PINS ─────────────────────────────────────────────────────
//
//   V1  ★ THE BUG, static + void + inferred. The issue's literal repro.
//   V2  ★ THE BUG, instance + void + inferred. Proves the gap is the
//       arm, not the static/instance spelling.
//   V3  ★ THE BUG, non-void + inferred. Already rejected before the fix;
//       pins part 2 — that the REJECTION NAMES `int`. This row is what
//       makes the two fixes one issue rather than two: without part 1 this
//       row is the only difference the user could see, and it was a
//       contentless message.
//   X1  EXPLICIT-LIST CONTROL for V1. Rejected pre-fix and post-fix with
//       the identical diagnostic, so "the two spellings agree" is pinned
//       as an equality of what the user sees, not merely as a rejection.
//   A1  ACCEPT-CONTROL. `Sink.Apply(c)` with `c: Circle` (which does
//       implement `Shape`) must still compile AND RUN. An under-rejection
//       fix that rejected everything would pass V1-V3 and X1; A1 is what
//       distinguishes a correct check from an over-broad one, and it
//       asserts the exact exit code rather than mere compilation.
//
// Every reject row matches the SPECIFIC diagnostic — the constraint text
// AND the offending type argument — rather than
// `failedAtCompileTimeWithRealLocation` alone, which answers true on both
// sides of a crash-shaped failure. V3 additionally asserts the contentless
// wording is GONE, so part 2 cannot silently regress back.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// The §14.2 diagnostic both parts of the fix are about, spelled the way a
// user must see it for a `Sink.Apply(5)` / `Sink.Apply<int>(5)` call on a
// `where U: Shape` callee with `int` inferred (or written) for `U`.
constexpr const char* kIntFailsShape =
    "type argument `int` does not satisfy the constraint `where U: Shape` "
    "(§14.2)";

// Declarations shared by the reject rows: an interface `Shape`, a class
// `Circle` that genuinely implements it (for A1), and a sink whose `Apply`
// carries the `where U: Shape` clause under test. `decls()` takes the module
// name, the `Apply` modifier (`"public static method"` or
// `"public method"`) and the sink body, so a row varies only the CALL.
std::string decls(const char* mod, const char* applyMod,
                  const char* sinkBody) {
    return std::string("module ") + mod + "; "
        "interface Shape "
        "  method Area(): int; "
        "end Shape; "
        "class Circle: Shape "
        "  public method Area(): int "
        "    return 3; "
        "  end Area; "
        "end Circle; "
        "class Sink "
        "  " + applyMod + " Apply<U>(x: U) where U: Shape " + sinkBody +
        "  end Apply; "
        "end Sink; ";
}

}  // namespace

// ── V1 ★ THE BUG — static, void-returning, type argument INFERRED ──────────
//
// The issue's literal repro, byte for byte. `5` is self-determined, so §14.5
// phase 1 binds `U = int`; `int` is a primitive, and no primitive conforms
// to a user interface bound it does not declare (§14.2 *Interface
// conformance* clause 5 admits only the primitives their stdlib declaration
// lists), so `where U: Shape` is unsatisfied. Pre-fix: exit 0, no
// diagnostic, program runs.
BEZEL_TEST(sema_method_where_inferred_typearg_5282,
           inferred_arg_on_void_static_method_rejects_spec_14_5_14_2) {
    auto r = runAot(decls("M5282V1", "public static method",
                          "Console.WriteLine(\"applied\");\n    ") +
                    "public static class Program "
                    "  public static method Main(): int "
                    "    Sink.Apply(5); "
                    "    return 0; "
                    "  end Main; "
                    "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kIntFailsShape));
}

// ── V2 ★ THE BUG — instance, void-returning, type argument INFERRED ────────
//
// The issue's parenthetical ("the instance-method spelling behaves the
// same"). Same arm, same gap; a receiver `s: Sink` instead of the type name.
// Pins that the fix is keyed on the arm (return-type split), not on the
// static-member spelling.
BEZEL_TEST(sema_method_where_inferred_typearg_5282,
           inferred_arg_on_void_instance_method_rejects_spec_14_5_14_2) {
    auto r = runAot(decls("M5282V2", "public method",
                          "Console.WriteLine(\"applied\");\n    ") +
                    "public static class Program "
                    "  public static method Main(): int "
                    "    var s := new Sink(); "
                    "    s.Apply(5); "
                    "    return 0; "
                    "  end Main; "
                    "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kIntFailsShape));
}

// ── V3 ★ Part 2 — non-void, inferred: the rejection must NAME `int` ────────
//
// This row's call was ALREADY rejected before the fix — the non-void arm
// has run the check since #729 — but the diagnostic read "type argument
// `the type argument` does not satisfy ...", naming no type at all. §14.2
// requires the instantiation-site diagnostic to name the offending type
// argument. So the user's two spellings of one call disagreed in WORDING as
// well as in enforcement. Pins the fixed wording, and pins that the
// contentless form is GONE.
BEZEL_TEST(sema_method_where_inferred_typearg_5282,
           inferred_arg_on_nonvoid_method_names_offending_type_spec_14_2) {
    auto r = runAot("module M5282V3; "
                    "interface Shape "
                    "  method Area(): int; "
                    "end Shape; "
                    "class Sink "
                    "  public static method Apply<U>(x: U): U where U: Shape "
                    "    return x; "
                    "  end Apply; "
                    "end Sink; "
                    "public static class Program "
                    "  public static method Main(): int "
                    "    var r := Sink.Apply(5); "
                    "    Console.WriteLine(\"applied\");\n    "
                    "    return 0; "
                    "  end Main; "
                    "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kIntFailsShape));
    // The pre-fix wording — a fallback string where the type should be.
    BEZEL_EXPECT_FALSE(
        diagnosticMessageContains(r, "type argument `the type argument`"));
}

// ── X1 EXPLICIT-LIST CONTROL for V1 ────────────────────────────────────────
//
// The written type-argument list was always rejected, and the issue's whole
// claim is that it and the inferred form must agree. Asserting the same
// exact diagnostic text on both sides pins the agreement as an equality of
// what the user sees — not merely "both rejected", which a too-broad check
// would also satisfy.
BEZEL_TEST(sema_method_where_inferred_typearg_5282,
           explicit_arg_on_void_static_method_rejects_identically_spec_14_2) {
    auto r = runAot(decls("M5282X1", "public static method",
                          "Console.WriteLine(\"applied\");\n    ") +
                    "public static class Program "
                    "  public static method Main(): int "
                    "    Sink.Apply<int>(5); "
                    "    return 0; "
                    "  end Main; "
                    "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kIntFailsShape));
}

// ── A1 ACCEPT-CONTROL — a satisfying inferred substitution still RUNS ──────
//
// `c: Circle` infers `U = Circle`, which DOES implement `Shape`, so §14.2
// is satisfied and the call is well-formed. Every reject row above passes
// against a check that rejects unconditionally; this row is what shows the
// check is a real constraint check. Asserts the exact exit code (7) rather
// than mere compilation, so a program that compiled but mis-typed or
// mis-dispatched would fail too.
BEZEL_TEST(sema_method_where_inferred_typearg_5282,
           satisfying_inferred_arg_on_void_method_compiles_and_runs_spec_14_2) {
    auto r = runAot(decls("M5282A1", "public static method",
                          "Console.WriteLine(\"applied\");\n    ") +
                    "public static class Program "
                    "  public static method Main(): int "
                    "    var c := new Circle(); "
                    "    Sink.Apply(c); "
                    "    return 7; "
                    "  end Main; "
                    "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
