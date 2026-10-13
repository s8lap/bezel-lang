// fix #5875 — §21.1 / §22.3 / §22.4 / §28.3 (v0.334). Pins WHICH pre-declared
// locals are in an "adoptable state" at an `out owned` argument position, and
// in particular that a §21.1 `const` local is in NONE of them.
//
// ── WHY THIS FILE EXISTS ─────────────────────────────────────────────────
//
// `ExprTC::checkRefOutOperandCategory` decides the §22.4 pre-declared-local
// rule by casting the target's symbol decl to `ast::LocalDecl*`. A `const`
// local's decl is an `ast::LocalConstDecl` — a different node with a
// differently-named `value` field — so the `const` case was decided by the
// `var` cast MISSING, not by any rule. Fix #5875 made that an explicit sibling
// arm (the site is a `LOCALDECL-ONLY` walker ratchet red, #3591's family).
//
// This change is BEHAVIOUR-NEUTRAL: the `const` local was rejected before it
// and is rejected now, because it reached the §22.4 rejection by not matching
// the `var` cast. That is exactly why it needs rows — a behaviour-neutral
// refactor of a DECISION is unguarded unless something pins the decision. Each
// row below is green before the fix and after it, and each one goes red if a
// later lane reads the new sibling arm as "admit `const` locals".
//
// ── THE RULES ─────────────────────────────────────────────────────────────
//
// QUOTED (§28.3 *Ownership state of locals*, v0.334) — the closed list:
//
//   "Two states qualify, and no others: an **uninitialized** local (`var x: T;`
//    at a non-defaultable `T` …), and one in the **polymorphic null state**
//    (`var x: T? := null;` …)"
//
// QUOTED (§22.4 *Inline `out` declarations*, v0.334):
//
//   "A **pre-declared local** passed as plain `out x` is admitted against `out
//    owned` exactly when it is in one of §28.3's **two adoptable states**."
//
// QUOTED (§21.1 *Runtime `const` locals*, v0.334) — the prohibition this site
// defers to, and which is independent of §28.3:
//
//   "A `const` local of either classification is **not an assignable
//    location**: assignment, compound assignment, `++`/`--`, and **binding to
//    a `ref` or `out` parameter** are compile errors naming the declaration."
//
// And grammar A.6 makes a local `const`'s initializer MANDATORY
// (`LocalConstDecl ::= "const" Id (":" T)? ":=" ConstExpr ";"` — no optional
// group, unlike `LocalDecl`), so a `const` local is never in the uninitialized
// state either. Combined: a `const` binding is in NEITHER adoptable state, and
// the position is separately forbidden.
//
// QUOTED (§22.3, v0.334) — the gate the admitted form has to clear anyway:
// an `out` argument "must be an **assignable storage location**", enumerated
// as "a **mutable local variable**".
//
// ── ROWS ──────────────────────────────────────────────────────────────────
//
//   C1  THE ROW FOR THE NEW ARM. `const c: Res? := null;` — the
//       polymorphic-null SPELLING, the one shape a `var` would be admitted in.
//       Rejected. If the sibling arm were misread as admitting `const`, this
//       row is what turns red.
//   C2  BOTH AUTHORITIES, NAMED. `const c := new Res(1);` is rejected TWICE —
//       by this site (§22.4) and independently by §21.1 ("must not take an
//       owned production"). Asserting both keeps the two from drifting into
//       one covering for the other.
//   C3  THE SIBLING STILL WORKS. `var c: Res?;` — the uninitialized form #5811
//       admits — must still be ADMITTED. This is the row that would catch an
//       `else if` mis-nested into the `var` arm's adoptability test, which
//       would silently disable the feature rather than reject something.
//   C4  THE NON-ADOPTABLE INITIALIZED STATE. `var c := new Res(1);` is
//       initialized to a non-null value, so it is in neither adoptable state —
//       rejected, with the same §22.4 message as C1. This is the boundary
//       between C3 and C1: initialization is what decides, not the spelling.
//
// Oracle: `runAot` (the strict AOT oracle). Reject rows assert the SPECIFIC
// §22.4 message at a real source location; the admit row asserts the program
// RAN and returned 0.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// §22.4's own stable predicate. Mirrors the needle the site reports.
const char kAdoptable[] = "in an adoptable state";

// §21.1's independent authority, for the one row that asserts both.
const char kConstNoOwned[] = "must not take an owned production";

// The nullable callee: `out owned r: Res?`. The two adoptable states are both
// spellable against it, so C1, C3 and C4 differ only in the DECLARATION.
const char kCalleeNullable[] =
    "class MakeLib "
    "  public static method Make(out owned r: Res?): bool "
    "    r := new Res(2); "
    "    return true; "
    "  end Make; "
    "end MakeLib; ";

// The non-nullable callee, for C2 — the shape on which §21.1's
// owned-production rule also fires.
const char kCalleeOwned[] =
    "class MakeLib "
    "  public static method Make(out owned r: Res): bool "
    "    r := new Res(2); "
    "    return true; "
    "  end Make; "
    "end MakeLib; ";

const char kRes[] =
    "class Res "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "end Res; ";

std::string prog(const std::string& callee, const std::string& decl) {
    return std::string("module OOC; ") + kRes + callee +
           "class C public static method Main(): int " + decl +
           "  var ok := MakeLib.Make(out c); "
           "  if ok then return 0; end if; "
           "  return 1; "
           "end Main; end C;";
}

}  // namespace

// ── C1 — THE ROW FOR THE NEW SIBLING ARM. A `const` local in the
// polymorphic-null spelling: the one declaration that is spelled exactly like
// an admitted `var`, and is still not adoptable.
//
// MEASURED on base 9ad4e6d0 and on the fix: rejected with §22.4's message.
BEZEL_TEST(sema_out_owned_const_local_adoptable_state_5875,
           c1_const_local_polymorphic_null_not_adopted_spec_28_3) {
    auto r = runAot(prog(kCalleeNullable, "const c: Res? := null; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kAdoptable));
}

// ── C2 — BOTH AUTHORITIES, NAMED. The rejection comes from this site AND,
// independently, from §21.1. Asserting both is what stops either one from
// being "fixed" by deleting the other.
BEZEL_TEST(sema_out_owned_const_local_adoptable_state_5875,
           c2_const_local_owned_producer_rejected_by_both_spec_21_1) {
    auto r = runAot(prog(kCalleeOwned, "const c := new Res(1); "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kAdoptable));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kConstNoOwned));
}

// ── C3 — THE SIBLING STILL WORKS. The uninitialized `var` that #5811 admits.
// If the new `else if` were mis-nested into the `var` arm this row goes red —
// it is the guard against a change that silently DISABLES a feature instead of
// rejecting something.
//
// MEASURED: accepted and returns 0, with only §28.3's implicit-dispose
// warning (the callee assigns, and Main returns without discharging).
BEZEL_TEST(sema_out_owned_const_local_adoptable_state_5875,
           c3_var_uninitialized_still_adopted_spec_28_3) {
    auto r = runAot(prog(kCalleeNullable, "var c: Res?; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── C4 — THE NON-ADOPTABLE INITIALIZED STATE. Same spelling family as C3,
// but initialized to a non-null value, so it is in neither of §28.3's two
// states. This is the boundary that makes C1 and C3 mean something: it is
// INITIALIZATION that decides, not the `var`/`const` keyword alone.
BEZEL_TEST(sema_out_owned_const_local_adoptable_state_5875,
           c4_var_initialized_nonnull_not_adopted_spec_28_3) {
    auto r = runAot(prog(kCalleeNullable, "var c: Res? := new Res(1); "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kAdoptable));
}