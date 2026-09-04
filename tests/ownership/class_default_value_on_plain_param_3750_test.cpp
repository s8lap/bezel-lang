// Spec-derived regression tests for issue #3750 — a class-typed §22.5 default
// value on a PLAIN (non-`owned`) parameter, at the §22.10 primary-ctor
// positional-field and §22.10 base-clause sites, was silently accepted and
// leaked the freshly-constructed instance.
//
// ── THE SHAPE (issue 3750's repro, verbatim, with the §22.10 dimensions
//     the issue calls out) ────────────────────────────────────────────────
//
//     class Res
//       destructor end destructor;     // non-trivial-dispose (§28.9)
//       public method Id(): int => 1;
//     end Res;
//
//     // §22.5 default on a §22.10 PRIMARY-CTOR POSITIONAL FIELD:
//     class Holder(a: int, r: Res := new Res(77))
//     end Holder;
//
//     // §22.10 INHERITANCE-CLAUSE BASE-CALL argument:
//     class Base(r: Res) end Base;
//     class Derived(a: int): Base(new Res(a)) end Derived;
//
// ── WHAT THE DEFECT WAS (measured on r12-cycle-1 @ 3f044f3 + #3742) ──────
//
//   * The §22.5 "Defaults and modifiers" check ran only inside
//     `ExprTC::runMethodBody` (sema/ExprTypecheck.cpp:11096) for ordinary
//     method/constructor parameter defaults — the primary-ctor
//     `positionalFields` loop and the `primaryBaseCtorArgs` loop walked
//     past it. Both sites are SHAPED identically (a default-value or
//     argument-expression producing a fresh owned value bound to a plain
//     positional parameter), so the same predicate belongs at both: the
//     existing check answers the question for parameter-side defaults,
//     but never visited either primary-ctor surface.
//
//   * The first variant was a SILENT ACCEPT — `Holder(5)` allocated an
//     `r` no one ever disposed, and `Live=1 Made=1` matched the
//     construction-and-no-disposal pattern of a leak (LSan 16 B direct,
//     200 iterations: 3200 B). The second variant left the freshly-
//     bound base positional-field `r` borrowed past the base call, with
//     §22.10's capture rule storing the borrow into a compiler-
//     generated capture field that OUTLIVES the base call — the spec's
//     "nothing owns the temporary" pattern (LSan 3200 B / 200 Res
//     instances on the issue body).
//
// ── SCOPE OF THIS FIX (the fix targets the §22.10 surfaces only) ─────────
//
//   §22.5 "Defaults and modifiers" mandates the rejection for a default-
//   value expression that produces a fresh owned value of NON-trivial-
//   dispose class type on a PLAIN parameter — the parameter can never
//   receive ownership, the callee never discharges the value, and the
//   "unbound owned temporary" admission §22.5 carves out requires either
//   trivial-dispose (the existing trivial-dispose carve-out of the run
//   method-body check) or `owned` (a separate path the §22.3 / §28.8
//   transfer-markered parameter takes). Mirror the check at the two
//   primary-ctor surfaces where it was missing.
//
//   Trivial-dispose (e.g. `new StringBuilder()`, the spec's own
//   walked example, and a primary-ctor positional-field shape `class
//   Holder2(a: int, sb: StringBuilder := new StringBuilder())`) is
//   ADMITTED — the predicate's own trivial-dispose exclusion keeps that
//   row green. The spec admits the trivial case on the grounds that the
//   call expression's K8 boundary frees the unbound owned temporary;
//   that disposal pass at the default-fill site is a separate ongoing
//   codegen refinement, R12's #3750 family-tracking notes, and is
//   explicitly out of lane scope.
//
// ── ROW BUDGET (5 rows) ──────────────────────────────────────────────────
//
//   R1: PRIMARY-CTOR POSITIONAL FIELD, non-trivial-dispose on PLAIN — REJECT.
//   R2: PRIMARY-CTOR POSITIONAL FIELD, trivial-dispose on PLAIN — ADMIT.
//   R3: PRIMARY-CTOR POSITIONAL FIELD, non-trivial-dispose on `owned` — ADMIT.
//   R4: §22.10 BASE-CLAUSE ARGUMENT, non-trivial-dispose on PLAIN — REJECT.
//   R5: §22.10 BASE-CLAUSE ARGUMENT, trivial-dispose on PLAIN — ADMIT.

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// A non-trivial-dispose class is the focus of the fix: a destructor flips
// §28.9's verdict and is what `Holder(a: int, r: Res := new Res(77))` must
// reject. `Id()` is the post-construction read used by the positive controls.
const char* kResWithDestructor =
    "class Res "
    "  public constructor(id: int) end constructor; "
    "  destructor end destructor; "
    "  public method Id(): int => 1; "
    "end Res; ";

// A trivial-dispose class — no destructor, no owned field — is what §22.5's
// own walked `new StringBuilder()` example shows stays admissible on a plain
// parameter (the spec's "stays admissible on either form" clause).
const char* kTrivialBox =
    "class Box "
    "  public X: int := 0; "
    "  public constructor() end constructor; "
    "  public method Id(): int => X; "
    "end Box; ";

// Stable rule identifier to match against the diagnostic. Both new sites
// cite §22.5 in their message text — the same string the run-method-body
// check has used since #1684 — so the substring is identical to the
// historical-pinned fragment the ownership test corpus already searches on.
const std::string kRuleFragment = "non-trivial-dispose";

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// R1 — REJECT. The headline #3750 repro shape at the §22.10 primary-ctor
// positional-field site: a non-trivial-dispose producer on a PLAIN
// parameter's `:= Expr` default.
// FAIL-WITHOUT: accepted with NO diagnostic, and the resulting
// `new Holder(5)` allocates an `r` no one ever disposes (16 B direct
// leak, measured on 3f044f3). The fix's positional-field reject
// surfaces §22.5 at the parameter declaration.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.5 "Defaults and modifiers" / §22.10 / §28.9 / §28.3 rule 5,
// issue #3750
BEZEL_TEST(class_default_value_on_plain_param_3750,
           primary_ctor_positional_field_nontrivial_on_plain_rejected) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kResWithDestructor +
        "class Holder(a: int, r: Res := new Res(77)) end Holder; "
        "class App "
        "  public static method Main(): int => 0; "
        "end App; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors() &&
                      bezel::test::hasErrorMessage(
                          r, "default value of primary constructor parameter"));
    BEZEL_EXPECT_TRUE(bezel::test::hasErrorMessage(r, kRuleFragment));
}

// ═══════════════════════════════════════════════════════════════════════════
// R2 — ADMIT. The §22.5 trivial-dispose carve-out at the same primary-ctor
// site: the spec explicitly says a trivial-dispose producer stays admissible
// on a plain parameter. Verifies the predicate's trivial-dispose exclusion
// is preserved when it is moved into the positional-field loop.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.5 / §28.9 trivial-dispose carve-out, issue #3750
BEZEL_TEST(class_default_value_on_plain_param_3750,
           primary_ctor_positional_field_trivial_dispose_on_plain_admitted) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kTrivialBox +
        "class Holder(a: int, b: Box := new Box()) end Holder; "
        "class App "
        "  public static method Main(): int => 0; "
        "end App; ");
    BEZEL_EXPECT_TRUE(!bezel::test::hasErrorMessage(r, kRuleFragment));
}

// ═══════════════════════════════════════════════════════════════════════════
// R3 — ADMIT. The §22.5 / §22.3 owned-parameter admitting escape: when the
// primary-ctor parameter is declared `owned`, the parameter receives
// ownership, §28.8 transfers it, and §28.4's generated `Dispose()` walks it.
// Verifies the new positional-field check gates on `modifier.empty()`
// (plain) only — `owned` parameters stay admitted.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.5 "Defaults and modifiers" / §22.3 / §28.8 / §28.4, issue #3750
BEZEL_TEST(class_default_value_on_plain_param_3750,
           primary_ctor_positional_field_nontrivial_on_owned_admitted) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kResWithDestructor +
        "class Holder(a: int, owned r: Res := new Res(77)) "
        "  public constructor() end constructor; "
        "  destructor r.Dispose(); end destructor; "
        "end Holder; "
        "class App "
        "  public static method Main(): int => 0; "
        "end App; ");
    BEZEL_EXPECT_TRUE(!bezel::test::hasErrorMessage(r, kRuleFragment));
}

// ═══════════════════════════════════════════════════════════════════════════
// R4 — REJECT. The headline #3750 repro shape at the §22.10 inheritance-
// clause base-call site: a `new Res(a)` argument bound to a base-class
// PLAIN positional parameter. §28.3 rule 5: a borrow of the temporary is
// stored past the base call's K8 boundary into a §22.10 capture field.
// FAIL-WITHOUT: accepted with NO diagnostic, and the loop in §22.5 / §28.3
// of the spec is what the issue body's 200-iteration measurement looks
// at — `made=200 live=200` is the silent accept, LSan reports 3200 B /
// 200 instances on the rejected row.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.5 "Defaults and modifiers" applied to §22.10 inheritance-clause
// base-call arguments / §22.10 capture rule / §28.3 rule 5 / §28.9,
// issue #3750
BEZEL_TEST(class_default_value_on_plain_param_3750,
           base_clause_argument_nontrivial_on_plain_rejected) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kResWithDestructor +
        "class Base(r: Res) "
        "  public method Use(): int => r.Id(); "
        "end Base; "
        "class Derived(a: int): Base(new Res(a)) end Derived; "
        "class App "
        "  public static method Main(): int => 0; "
        "end App; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors() &&
                      bezel::test::hasErrorMessage(r, "base-call argument"));
    BEZEL_EXPECT_TRUE(bezel::test::hasErrorMessage(r, kRuleFragment));
}

// ═══════════════════════════════════════════════════════════════════════════
// R5 — ADMIT. The §22.5 trivial-dispose carve-out at the §22.10 base-
// clause site: a `new Box()` argument bound to a base-class plain
// positional parameter stays admissible. Verifies the new base-clause
// check's trivial-dispose exclusion.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.5 / §28.9 trivial-dispose carve-out at §22.10 base-clause site,
// issue #3750
BEZEL_TEST(class_default_value_on_plain_param_3750,
           base_clause_argument_trivial_dispose_on_plain_admitted) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kTrivialBox +
        "class Base(r: Box) "
        "  public method Use(): int => r.Id(); "
        "end Base; "
        "class Derived(a: int): Base(new Box()) end Derived; "
        "class App "
        "  public static method Main(): int => 0; "
        "end App; ");
    BEZEL_EXPECT_TRUE(!bezel::test::hasErrorMessage(r, kRuleFragment));
}
