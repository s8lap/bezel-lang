// Spec-derived tests for fix #4449 — §22.16 (v0.303) requires the compiler to
// emit a warning at a `static invariant` clause naming that the clause is
// inert in this revision (the contract-proving prover fragment that would
// discharge it has not landed). The warning fires ONCE per clause at its
// declaration site, on `static invariant` ONLY — plain `invariant` is
// unaffected, and the program verdict does not change.
//
// §22.16's "Out of scope in this revision" rule, verbatim: "the compiler
// **must** emit a warning at the clause naming that fact. ... The warning
// changes no verdict — every program that compiled before compiles still
// — and it is withdrawn, not merely silenced, when the prover fragment
// lands."
//
// The fix is in src/parser/Parser_Decls.cpp at the InvariantDecl parse arm:
// when `isStatic` is true (the `static` modifier was present), the parser
// emits exactly one `Severity::Warning` at the `invariant` keyword with the
// wording the issue fixes:
//
//   "`static invariant` is not instrumented in this revision — the clause
//    is parsed and otherwise inert until the contract-proving prover
//    fragment lands (§22.16)"
//
// Three properties per the issue, exercised below:
//   P1. Fires on `static invariant` ONLY, never on plain `invariant`.
//   P2. Fires ONCE per clause (per declaration site), not at each method
//       boundary — a class with one static-invariant clause and several
//       methods yields ONE warning.
//   P3. Verdicts are unchanged — a `static invariant(false);` program still
//       runs to its expected exit value (the warning does NOT promote to
//       an error).
//
// Each test names the property in its case name so a regression can be
// bisected by case (e.g. "P2 broke on commit X" maps to
// `static_invariant_inertness_warning_4449_once_per_clause_spec_22_16`).
//
// Pre-fix: ALL THREE properties failed — `static invariant` parsed with
// zero diagnostics (this was the filing's own measurement, v0.302 HEAD).

#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include <string>
#include <string_view>

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

using bezel::test::parseAndCollect;
using bezel::test::runAot;

namespace {

bezel::test::SemaResult semaAll(std::string_view src) {
    auto r = parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff SOME diagnostic at Warning severity matches the inertness-rule
// needle. The needle is fixed by the issue (#4449) and cited verbatim from
// §22.16; nothing in this test file should weaken the substring (LANE_RULES
// §3: never weaken an oracle).
bool hasInertnessWarning(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("`static invariant` is not instrumented in this "
                           "revision") == std::string::npos) continue;
        if (d.message.find("§22.16") == std::string::npos) continue;
        return true;
    }
    return false;
}

// True iff NO inertness warning fired (a plain-`invariant` program — or a
// program with no invariant clauses at all — must not spuriously warn).
bool lacksInertnessWarning(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("`static invariant` is not instrumented in this "
                           "revision") == std::string::npos) continue;
        return false;
    }
    return true;
}

// Counts the number of inertness-warning diagnostics in `r`. Used by P2
// ("fires ONCE per clause"): a class with N static-invariant clauses must
// produce EXACTLY N warnings, no more.
int countInertnessWarnings(const bezel::test::SemaResult& r) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("`static invariant` is not instrumented in this "
                           "revision") == std::string::npos) continue;
        ++n;
    }
    return n;
}

int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// =====================================================================
// P1 — fires on `static invariant` ONLY, never on plain `invariant`.
// =====================================================================

// P1, accept path: a plain (non-static) `invariant(x > 0);` in an
// ordinary class compiles with ZERO inertness warnings. The clause is
// enforced at runtime (codegen's `Emitter::emitInvariantCheck` includes
// non-static invariants), so no inertness signal is appropriate.
BEZEL_TEST(static_invariant_inertness_warning_4449,
           plain_invariant_does_not_warn_spec_22_16) {
    auto r = semaAll(
        "module M; "
        "class C1 "
        "  public x: int := 1; "
        "  invariant(x > 0); "
        "  public constructor() end constructor; "
        "end C1; "
        "class Main1 public static method Main(): int return 0; "
        "end Main; end Main1;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(lacksInertnessWarning(r));
}

// P1, the issue's primary shape: a single `static invariant(true);` clause
// in an ordinary class emits EXACTLY ONE inertness warning at the clause.
// The wording the issue fixes is asserted via substring match.
BEZEL_TEST(static_invariant_inertness_warning_4449,
           static_invariant_emits_warning_spec_22_16) {
    auto r = semaAll(
        "module M; "
        "class C2 "
        "  public x: int := 1; "
        "  static invariant(true); "
        "  public constructor() end constructor; "
        "end C2; "
        "class Main2 public static method Main(): int return 0; "
        "end Main; end Main2;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInertnessWarning(r));
}

// P1, sibling-shape cross-check: plain `invariant` SIDE BY SIDE with a
// `static invariant` in the same class. The plain clause is enforced; the
// static clause is inert. The diagnostic must fire on the static clause
// ONLY — never on the plain one.
BEZEL_TEST(static_invariant_inertness_warning_4449,
           plain_invariant_sibling_does_not_warn_spec_22_16) {
    auto r = semaAll(
        "module M; "
        "class C3 "
        "  public x: int := 1; "
        "  invariant(x > 0); "
        "  static invariant(true); "
        "  public constructor() end constructor; "
        "end C3; "
        "class Main3 public static method Main(): int return 0; "
        "end Main; end Main3;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // The static clause's warning fires:
    BEZEL_EXPECT_TRUE(hasInertnessWarning(r));
    // … and EXACTLY one — the plain `invariant` must not add a phantom one:
    BEZEL_EXPECT_EQ(countInertnessWarnings(r), 1);
}

// =====================================================================
// P2 — fires ONCE per clause, not at each method boundary.
// =====================================================================

// P2: a class with THREE methods and ONE static-invariant clause yields
// EXACTLY ONE inertness warning, not three (one per method boundary).
// This pins the fix's "fire at the clause's declaration site" property
// against any future regression that might emit at every instrumented
// method entry (a pattern that would silently downgrade the diagnostic
// into background noise).
BEZEL_TEST(static_invariant_inertness_warning_4449,
           once_per_clause_spec_22_16) {
    auto r = semaAll(
        "module M; "
        "class C4 "
        "  public x: int := 1; "
        "  static invariant(true); "
        "  public constructor() end constructor; "
        "  public method A(): int return x; end A; "
        "  public method B(): int return x + 1; end B; "
        "  public method C(): int return x + 2; end C; "
        "end C4; "
        "class Main4 public static method Main(): int return 0; "
        "end Main; end Main4;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(countInertnessWarnings(r), 1);
}

// P2, multi-clause arm: TWO `static invariant` clauses in one class yield
// EXACTLY TWO inertness warnings (one per clause). This is the positive
// proof that the count is "per clause" and not "per class" or "per
// method" — a future regression that mistakenly hoists the warning to a
// "per class" rule would fail this case.
BEZEL_TEST(static_invariant_inertness_warning_4449,
           two_static_invariants_two_warnings_spec_22_16) {
    auto r = semaAll(
        "module M; "
        "class C5 "
        "  public x: int := 1; "
        "  static invariant(true); "
        "  static invariant(x > 0); "
        "  public constructor() end constructor; "
        "end C5; "
        "class Main5 public static method Main(): int return 0; "
        "end Main; end Main5;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(countInertnessWarnings(r), 2);
}

// =====================================================================
// P3 — verdict is unchanged: the program still compiles and runs to its
// declared exit value. The warning does NOT promote to an error.
// =====================================================================

// P3 (mirrors fix #4206's first repro, now with the inertness warning
// expected alongside): a `static invariant(true);` program runs to 42.
// Pre-fix this ran at exit 0 with zero diagnostics; post-fix it runs at
// exit 42 with ONE inertness warning — verdict unchanged.
BEZEL_TEST(static_invariant_inertness_warning_4449,
           verdict_unchanged_run_to_exit_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Account4449 "
        "  private _balance: decimal := 0m; "
        "  static invariant(true); "
        "  public constructor() end constructor; "
        "end Account4449; "
        "class C4449 public static method Main(): int "
        "  var a := new Account4449(); "
        "  return 42; "
        "end Main; end C4449;"),
        42);
}

// P3 (the inertness proof from #4206, carried forward): a
// `static invariant(false);` program still does NOT fault — the inert
// clause is not instrumented, so `ContractViolationException` is never
// raised. The catch arm's discriminator stays at the default (0).
BEZEL_TEST(static_invariant_inertness_warning_4449,
           static_invariant_false_still_inert_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class C4449b "
        "  public x: int := 0; "
        "  static invariant(false); "
        "  public constructor() end constructor; "
        "  public static method Main(): int "
        "    var r: int := 0; "
        "    try "
        "      var a := new C4449b(); "
        "    catch ContractViolationException => r := 99; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C4449b;"),
        0);
}