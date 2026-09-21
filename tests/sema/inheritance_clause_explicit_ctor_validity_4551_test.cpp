// Regression tests for issue 4551 — §22.5 argument-binding VALIDITY was
// unchecked for a §22.10 inheritance clause that binds an EXPLICIT base
// constructor, so a malformed clause compiled clean and left the base
// subobject unconstructed — or, worse, bound an argument to a parameter it
// never named (§22.5 "Optional parameters and named arguments" / §22.6 /
// §22.10 "Primary constructors", v0.306).
//
// This is the THIRD site's twin of #4496 (the `base(...)` chain) and #4518
// (the clause bound to the base's PRIMARY parameter list). With those two
// closed this was the last silent arm in the family; #4520 bound this arm BY
// NAME and recorded the deferral, because its subject was binding ORDER and a
// new rejection belonged here.
//
// MEASURED ON 1ee866a9 (this fix's parent), all against `class B` whose only
// constructor is `constructor(a: int, b: string)`, compiled AND RUN with
// `build/bezelc`. Every one compiled at exit 0 with ZERO diagnostics and ran
// to exit 0:
//
//   : B(zz := v, b := "e")    printed `4` / `e` — the argument naming a
//                             parameter that DOES NOT EXIST was not inert: it
//                             supplied `a` BY POSITION. Not merely an
//                             undiagnosed call; a MISBOUND one, and exactly
//                             the pre-#4518 behaviour of the primary-list arm.
//   : B(a := v, a := 2)       printed `0` and a blank line
//   : B(v, a := 2, b := "e")  printed `0` and a blank line
//   : B(v, "e", 3)            printed `0` and a blank line
//   : B(v)                    printed `0` and a blank line
//
// The four quiet rows print `B`'s own declared field initializers: neither
// resolver found a callee, `emitImplicitBase` fell through to its arity-0
// fallback, found no parameterless constructor, and the base was NEVER
// CONSTRUCTED.
//
// THE DIAGNOSTICS ARE NOT NEW TEXT. They are the same strings
// `bindArgsToParams` already emits for a `new T(...)`, a method call, a
// `this(...)` chain, a `base(...)` chain (#4496) and the clause bound to a
// primary list (#4518) — #3178's `FormalsView` exists so that one binder body
// serves every formal source.
//
// WHERE IT REPORTS, per §22.5's OWN split, because this was NOT a one-word
// flip like its two siblings. §22.5: an unbound required parameter "makes the
// candidate inapplicable … so at an overloaded name an applicable sibling
// still resolves and the error is reported only where no candidate survives".
// The binder is therefore loud only when the base declares exactly ONE
// non-static constructor AND the clause does not bind the base's §22.10
// PRIMARY parameter list instead — the second condition being the exact
// complement of the primary-list arm's own gate, so every clause is checked by
// exactly one of the two arms and never by both.
//
// THE CONTROLS ARE THE POINT OF THE FILE, because this is a new rejection and
// the risk is OVER-rejection. Six rows pin shapes that must still compile, and
// each asserts the printed VALUES rather than just `r.ran` — a fix that keeps
// compiling a clause but stops constructing its base is what an
// acceptance-only oracle cannot see.

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {

// `class B` with NO §22.10 primary parameter list and exactly ONE explicit
// constructor — the shape the issue measures. Only the clause varies.
std::string overSoleExplicitCtor(const std::string& clause) {
    return "module M; "
           "class B "
           "  public n: int := 0; "
           "  public s: string := \"\"; "
           "  public constructor(a: int, b: string) "
           "    n := a; "
           "    s := b; "
           "  end constructor; "
           "  public method Show() "
           "    Console.WriteLine($\"n={n} s={s}\"); "
           "  end Show; "
           "end B; "
           "class D(v: int) : " +
           clause +
           " "
           "  public method Go() Show(); end Go; "
           "end D; "
           "class C "
           "  public static method Main(): int "
           "    using var d := new D(4); "
           "    d.Go(); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// PIN 1 — the issue's worst row: an argument naming a parameter the
// constructor does not declare. Pre-fix this was not merely undiagnosed — it
// BOUND `a` by position and the program ran to exit 0 printing `4` / `e`, so
// `!r.ran` alone is a real oracle here and the message is what distinguishes
// the §22.5 reason from a later edit breaking the arm outright.
// ---------------------------------------------------------------------------
// per §22.5 / §22.10, issue #4551
BEZEL_TEST(inheritance_clause_explicit_ctor_validity_4551,
           clause_naming_a_nonexistent_parameter_is_rejected_22_5) {
    auto r = runAot(overSoleExplicitCtor("B(zz := v, b := \"e\")"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "has no parameter named `zz` (§22.5)"));
}

// ---------------------------------------------------------------------------
// PIN 2 — the same parameter named TWICE. §22.5: "naming the same parameter
// twice, is a compile error at the call naming that parameter".
// ---------------------------------------------------------------------------
BEZEL_TEST(inheritance_clause_explicit_ctor_validity_4551,
           clause_binding_one_parameter_twice_by_name_is_rejected_22_5) {
    auto r = runAot(overSoleExplicitCtor("B(a := v, a := 2)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "parameter `a` is bound more than once (§22.5)"));
}

// ---------------------------------------------------------------------------
// PIN 3 — a parameter bound POSITIONALLY and then BY NAME, the other half of
// the same §22.5 sentence ("naming a parameter already bound positionally").
// ---------------------------------------------------------------------------
BEZEL_TEST(inheritance_clause_explicit_ctor_validity_4551,
           clause_binding_a_positional_parameter_again_by_name_rejected_22_5) {
    auto r = runAot(overSoleExplicitCtor("B(v, a := 2, b := \"e\")"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "parameter `a` is bound more than once (§22.5)"));
}

// ---------------------------------------------------------------------------
// PIN 4 — OVER-supplied. This row is why the emit path also has to reach a
// constructor the candidate scan does not select: with three arguments and one
// two-formal constructor there is no candidate of this arity to find, and
// §22.5's over-supply rejection must still be reported against the one
// constructor that exists rather than dropped.
// ---------------------------------------------------------------------------
BEZEL_TEST(inheritance_clause_explicit_ctor_validity_4551,
           over_supplied_clause_is_rejected_22_5) {
    auto r = runAot(overSoleExplicitCtor("B(v, \"e\", 3)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "supplies 3 argument(s) but the parameter list has 2 (§22.5)"));
}

// ---------------------------------------------------------------------------
// PIN 5 — UNDER-supplied: a REQUIRED parameter left unbound. §22.5: "Every
// parameter that declares no default must be bound, positionally or by name."
// The row is the companion to the over-supplied one and is what separates
// "rejects a short clause" from "rejects a short clause whose omitted slot has
// no default" — the #4550 control below omits a DEFAULTED slot and must
// still compile.
// ---------------------------------------------------------------------------
BEZEL_TEST(inheritance_clause_explicit_ctor_validity_4551,
           clause_leaving_a_required_parameter_unbound_is_rejected_22_5) {
    auto r = runAot(overSoleExplicitCtor("B(v)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "required parameter `b` is not supplied (§22.5)"));
}

// ---------------------------------------------------------------------------
// CONTROL — the well-formed NAMED full-arity clause, #4520's own subject. The
// values are asserted so that a lost binding fails here rather than silently
// restoring #4520's dropped-clause behaviour.
// ---------------------------------------------------------------------------
BEZEL_TEST(inheritance_clause_explicit_ctor_validity_4551,
           well_formed_named_clause_still_accepted_4520) {
    auto r = runAot(overSoleExplicitCtor("B(b := \"e\", a := v)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=4 s=e\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — an ALL-POSITIONAL, exactly-supplied clause: the corpus's majority
// shape on this arm.
// ---------------------------------------------------------------------------
BEZEL_TEST(inheritance_clause_explicit_ctor_validity_4551,
           all_positional_exact_clause_still_accepted_22_5) {
    auto r = runAot(overSoleExplicitCtor("B(v, \"e\")"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=4 s=e\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — a clause that legitimately OMITS a §22.5-DEFAULTED parameter
// (#4550's subject). This is the shape a naive "every parameter must be bound"
// reading of the new rejection breaks, and the one that separates PIN 5's
// complaint from "rejects every short clause".
// ---------------------------------------------------------------------------
BEZEL_TEST(inheritance_clause_explicit_ctor_validity_4551,
           clause_omitting_a_defaulted_parameter_still_accepted_4550) {
    auto r = runAot(
        "module M; "
        "class B "
        "  public n: int := 0; "
        "  public s: string := \"\"; "
        "  public constructor(a: int, b: string := \"zz\") "
        "    n := a; "
        "    s := b; "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine($\"n={n} s={s}\"); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(a := v) "
        "  public method Go() Show(); end Go; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(4); "
        "    d.Go(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=4 s=zz\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — the arm's BOUNDARY, and #4518's own control row in this file's
// terms: a base with BOTH a §22.10 primary parameter list and a convenience
// constructor, where the clause binds the PRIMARY list (its arity matches the
// list, not the constructor). That clause belongs to the primary-list arm and
// is checked there; this arm's gate is that arm's exact complement, so it must
// not be reached here — and it must still construct the base.
// ---------------------------------------------------------------------------
BEZEL_TEST(inheritance_clause_explicit_ctor_validity_4551,
           clause_binding_the_primary_list_is_not_checked_here_22_10) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public constructor(a: int) "
        "    this(a, \"conv\"); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine($\"n={n} s={s}\"); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(v, \"x\") "
        "  public method Go() Show(); end Go; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(7); "
        "    d.Go(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=7 s=x\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — the same base, with the clause binding the CONVENIENCE constructor
// by exact arity. This one IS this arm's, and the binder accepts it; the row
// pins that turning the binder loud did not reject the shape #4518 carried as
// its own "unaffected" control.
// ---------------------------------------------------------------------------
BEZEL_TEST(inheritance_clause_explicit_ctor_validity_4551,
           clause_binding_a_convenience_ctor_still_accepted_22_10) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public constructor(a: int) "
        "    this(a, \"conv\"); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine($\"n={n} s={s}\"); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(v) "
        "  public method Go() Show(); end Go; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(7); "
        "    d.Go(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=7 s=conv\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — DECLINED: an OVERLOAD SET. §22.5 makes an unbound required
// parameter an APPLICABILITY failure, "so at an overloaded name an applicable
// sibling still resolves and the error is reported only where no candidate
// survives". With two non-static constructors the binder stays silent here,
// and #4520's by-name binding must still resolve — which `s=two` pins (a lost
// binding would print `one`, or nothing at all).
// ---------------------------------------------------------------------------
BEZEL_TEST(inheritance_clause_explicit_ctor_validity_4551,
           overload_set_is_not_checked_here_declined_22_6) {
    auto r = runAot(
        "module M; "
        "class B "
        "  public n: int := 0; "
        "  public s: string := \"\"; "
        "  public constructor(x: int) "
        "    n := x; "
        "    s := \"one\"; "
        "  end constructor; "
        "  public constructor(x: int, y: string) "
        "    n := x; "
        "    s := y; "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine($\"n={n} s={s}\"); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(x := v, y := \"two\") "
        "  public method Go() Show(); end Go; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(4); "
        "    d.Go(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=4 s=two\n"));
}
