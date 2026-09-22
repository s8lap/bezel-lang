// Regression tests for issue #4576 gap (b) — §22.5/§22.10: "no constructor
// of this arity at all" against a §22.10 inheritance clause that binds an
// EXPLICIT base constructor was completely unreported. Where the base
// declares an OVERLOAD SET (more than one non-static constructor) — or ZERO
// non-static constructors — and NEITHER arity scan in `ExprTC::visitTopDecl`
// (the exact-arity match, and #4550's "more formals, possibly covered by
// defaults" fallback) finds any candidate at all, the clause silently
// resolved no callee: `emitImplicitBase` fell through to its arity-0
// fallback and the base was left UNCONSTRUCTED with zero diagnostics.
//
// This is the sibling of #4551, which made the same binder loud for the
// SOLE-constructor case (`nonStaticCtorCount == 1`) by grabbing that one
// constructor and letting `bindArgsToParams`'s own arity check report the
// reason. #4551's own header explicitly declined this shape: "'No
// constructor of this arity at all.' Left to whatever diagnoses an
// unresolvable base call today — which, measured, is nothing." This fix
// closes that deferral. It does NOT touch #4576's gap (a) (a doubly-bound or
// unknown-named parameter against a SURVIVING sibling of an overload set —
// #4551 already declines that shape with its own control, reasserted here as
// PIN 5) or gap (c) (§5.1 assignability of a bound argument).
//
// spec §22.5 "Optional parameters and named arguments" ("the error is
// reported only where no candidate survives"), §22.10 "Primary constructors"
// ("the base call appears in the inheritance clause ... This replaces an
// explicit base(...) call"), v0.307.
//
// MEASURED pre-fix, on this lane's HEAD (d488f37b, via `runAot`): `class
// D(v: int) : B(v, v, v)` against a base `B` declaring `constructor(a: int)`
// (arity 1) and `constructor(a: int, b: int)` (arity 2) — neither of which
// can ever accept 3 arguments, with or without defaults — compiled at exit 0
// with NO diagnostic; `d.Go()` on the resulting object printed the FIELD
// INITIALIZERS (`n=0 s=0`), not either constructor's work, because the base
// subobject was never constructed.

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {

// `class B` with an OVERLOAD SET of two non-static constructors (arity 1 and
// arity 2, no §22.10 primary parameter list) and no combination of defaults
// that could ever admit a 3-argument clause. Only the clause varies.
std::string overOverloadSetNoDefaults(const std::string& clause) {
    return "module M; "
           "class B "
           "  public n: int := 0; "
           "  public s: int := 0; "
           "  public constructor(a: int) "
           "    n := a; "
           "  end constructor; "
           "  public constructor(a: int, b: int) "
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
// PIN 1 — the ★ bug: three arguments against an overload set whose largest
// constructor takes two. Neither the exact-arity scan nor #4550's more-
// formals-with-defaults fallback finds a candidate, and (unlike #4551's own
// repro) the base declares MORE than one non-static constructor, so #4551's
// `soleCandidate` fallback does not reach this shape either.
// ---------------------------------------------------------------------------
// per §22.5 / §22.10, issue #4576 gap (b)
BEZEL_TEST(inheritance_clause_ctor_no_arity_candidate_4576,
           overload_set_with_no_arity_candidate_at_all_is_rejected) {
    auto r = runAot(overOverloadSetNoDefaults("B(v, v, v)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "supplies 3 argument(s), but no non-static constructor of `B` "
           "declares that many parameters"));
}

// ---------------------------------------------------------------------------
// PIN 2 — the same hole with a base declaring ZERO non-static constructors
// (no explicit constructor, and no §22.10 primary parameter list at all).
// #4551's `soleCandidate` fallback requires `nonStaticCtorCount == 1` and
// does not reach `== 0` either — a second, independent instance of the same
// silent gap.
// ---------------------------------------------------------------------------
// per §22.5 / §22.10, issue #4576 gap (b)
BEZEL_TEST(inheritance_clause_ctor_no_arity_candidate_4576,
           base_with_zero_non_static_constructors_and_a_clause_is_rejected) {
    auto r = runAot(
        "module M; "
        "class B "
        "  public n: int := 0; "
        "end B; "
        "class D(v: int) : B(v) "
        "  public method Go() Console.WriteLine($\"n={n}\"); end Go; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(4); "
        "    d.Go(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "supplies 1 argument(s), but no non-static constructor of `B` "
           "declares that many parameters"));
}

// ---------------------------------------------------------------------------
// NO-REGRESSION 1 — the SAME overload set as PIN 1, with a clause whose arity
// EXACTLY matches one sibling. The exact-arity scan finds that sibling
// before this fix's new diagnostic block ever runs, so the call must still
// compile and run, with the correct field values — an acceptance-only oracle
// would not see a construction this fix's new code path never reaches.
// ---------------------------------------------------------------------------
// per §22.5 / §22.10, issue #4576 gap (b) — no-regression control
BEZEL_TEST(inheritance_clause_ctor_no_arity_candidate_4576,
           overload_set_with_exact_arity_candidate_still_constructs) {
    auto r = runAot(overOverloadSetNoDefaults("B(v, 9)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=4 s=9\n"));
}

// ---------------------------------------------------------------------------
// NO-REGRESSION 2 — an overload set where the viable candidate is found only
// by #4550's fallback (a constructor with MORE formals than the clause
// supplies, the omitted slot carrying a default). Must still compile and
// run — the fallback's own candidate is what #4551 already established as
// reachable, and this fix's new block must never override a candidate that
// fallback found.
// ---------------------------------------------------------------------------
// per §22.5 / §22.10, issue #4576 gap (b) — no-regression control
BEZEL_TEST(inheritance_clause_ctor_no_arity_candidate_4576,
           overload_set_with_defaulted_fallback_candidate_still_constructs) {
    auto r = runAot(
        "module M; "
        "class B "
        "  public n: int := 0; "
        "  public s: int := 0; "
        // Deliberately arity 0, NOT arity 1 — the clause below supplies one
        // argument, and an arity-1 sibling here would satisfy the EXACT-arity
        // scan first and never exercise the fallback this row targets.
        "  public constructor() "
        "  end constructor; "
        "  public constructor(a: int, b: int := 7) "
        "    n := a; "
        "    s := b; "
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
        "    using var d := new D(4); "
        "    d.Go(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    // Neither the exact-arity scan (no ctor of arity 1) nor an ambiguity
    // (only ONE ctor has arity > 1) blocks resolution: #4550's fallback
    // finds `constructor(a: int, b: int := 7)` as the sole wider-arity
    // candidate, `clauseCtor` is non-null, and this fix's new diagnostic
    // (gated on `!clauseCtor`) must not fire over it. `a` binds
    // positionally (4) and `b` takes its default (7).
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=4 s=7\n"));
}

// ---------------------------------------------------------------------------
// NO-REGRESSION 3 — the primary-list arm's complement gate. A base with a
// §22.10 primary parameter list of one arity plus a convenience constructor
// of another: the clause binds the PRIMARY list (`explicitCtorOfThisArity`
// is false for THIS clause's arity), which the block above (fix #4518)
// already owns. This fix's `!clauseBindsPrimaryList` gate must keep this
// shape out of its new diagnostic.
// ---------------------------------------------------------------------------
// per §22.5 / §22.10, issue #4576 gap (b) — no-regression control
BEZEL_TEST(inheritance_clause_ctor_no_arity_candidate_4576,
           clause_binding_the_primary_list_is_not_reached_by_this_fix) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: int) "
        "  public constructor(a: int) "
        "    this(a, 0); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine($\"n={n} s={s}\"); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(v, 9) "
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
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=4 s=9\n"));
}

// ---------------------------------------------------------------------------
// PIN 5 — ⚠⚠⚠ THIS ROW PINS A KNOWN DEFECT, NOT SPEC-REQUIRED BEHAVIOUR.
// ⚠⚠⚠
//
// It is a CHARACTERIZATION test only: it records what the compiler currently
// DOES (pre-existing, unfixed, WRONG per §22.5) so that THIS commit — which
// deliberately does not touch gap (a) — does not accidentally change it as a
// side effect. It is NOT an assertion that `n=4 s=9` is the correct answer
// for this program. The correct answer, once #4576 gap (a) is fixed, is a
// REJECTION naming `zz` as an unknown parameter — because `zz` is not a real
// parameter name on EITHER constructor in the overload set, and §22.5 says
// naming a nonexistent parameter "is a compile error at the call naming that
// parameter — not an applicability failure, so at an overloaded name it is
// diagnosed rather than resolved away", exactly the wording #4551 read to
// justify gap (b) above.
//
// Gap (a) (a doubly-bound or unknown-named parameter against a SURVIVING
// sibling of an overload set) needs a scan over every candidate's parameter
// NAMES, not just arity, per #4551's own recorded decline — a different
// shape from this commit's gap (b) fix, and NOT attempted here.
//
// >>> WHEN GAP (a) LANDS, THIS ROW'S EXPECTATION MUST FLIP to a rejection.
// >>> Flipping it is the CORRECT outcome of that fix, not a regression to
// >>> investigate — do not "fix" the future failure by reverting gap (a).
// ---------------------------------------------------------------------------
// per §22.5 / §22.6, issue #4576 gap (a), NOT fixed by this commit — pins
// the pre-existing defective behaviour as a characterization, see #4551's
// own overload-set control for the sibling shape
BEZEL_TEST(inheritance_clause_ctor_no_arity_candidate_4576,
           overload_set_unknown_named_parameter_gap_a_known_defect_pin) {
    auto r = runAot(overOverloadSetNoDefaults("B(zz := v, b := 9)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // KNOWN-DEFECT VALUE, not a correctness claim (see the block comment
    // above): `zz` is not a real parameter name on EITHER sibling, but the
    // 2-argument arity still finds the 2-formal constructor's exact-arity
    // match, and both arguments bind POSITIONALLY (`a := zz-argument`,
    // `b := 9`) rather than being rejected. This is exactly the shape
    // #4576's gap (a) records as still open, and this row exists to detect
    // an ACCIDENTAL behaviour change from this commit, not to certify the
    // value as right.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=4 s=9\n"));
}
