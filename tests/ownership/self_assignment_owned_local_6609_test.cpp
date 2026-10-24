// fix #6609 / §28.3 bullet 1 (spec v0.339): `t := t;` on an owned-state
// local is ACCEPTED where the spec requires rejection.
//
// v0.339 added to §28.3's *State match on reassignment* (bullet 1), verbatim:
//
//   "an **owned-state** LHS requires an owned RHS — a fresh producer, or a
//   named owned binding under the explicit transfer marker (`x := move y;`) —
//   and disposes the displaced value per drop-on-overwrite (§28.7), subject to
//   the consume-site freeze below; a **borrowed-state** LHS requires a borrow
//   (or `null`) RHS. A self-assignment is no exception: for an owned-state
//   class-typed local `t`, `t := t;` stores the unmarked read `t`, a borrow,
//   into an owned-state LHS and is rejected at the assignment by this rule,
//   not a §28.7 discharge."
//
// MEASURED on the parent (`3992d898`): the issue's repro compiles CLEAN, rc=0,
// the only diagnostic being the pre-existing §28.3 auto-drop *warning*. The
// control arm (the same file with `t := t;` removed) is rc=0 with the same
// warning, so the probe parses and the difference is the self-assignment
// itself. ROW 1 is the issue's actual repro and FAILS without the fix.
//
// WHAT IS AND IS NOT REJECTED, and why each exclusion is load-bearing —
// `t := t` is an extremely common idiom, so every one of these was MEASURED on
// the parent and is pinned here as an ACCEPT row:
//   * a MOVE-ONLY VALUE TYPE keeps §15.3's own stated no-op (§15.3
//     *Whole-value reassignment*: "self-assignment `arr := arr;` … is
//     well-defined … self-assignment is therefore a no-op"). ROW 5.
//   * a PRIMITIVE and a STRING local carry no disposal obligation, so the
//     state-match rule's first bullet does not apply. ROWS 6 and 7.
//   * a BORROW-state local is the rule's SECOND bullet — "a borrowed-state LHS
//     requires a borrow (or `null`) RHS" — and an unmarked read is exactly
//     that borrow. ROW 3.
//   * an `owned` FIELD self-assignment was ALREADY rejected by a different arm
//     (§28.3/§28.5, "a borrow cannot be stored into an `owned` field"), and
//     must keep that diagnostic. ROW 4 is the discriminator pair.
//   * `t := move t` is the §28.3 / §22.3 explicit-transfer spelling and is
//     accepted. ROW 8.
//   * two DISTINCT owned locals — `t := u` — is an ordinary §28.7
//     drop-on-overwrite. ROW 9.
//   * §21.1's `const` local self-assignment keeps ITS OWN diagnostic; the two
//     rules are distinct and each must fire as itself. ROW 10.
//
// NOT COVERED, REPORTED: §28.3's state-match bullet is general, and
// `var t := new Res(); t := r;` with `r` a `Res` PARAMETER is the same
// violation through a different spelling — MEASURED accepted rc=0 on the
// parent, and it stays accepted here. The fix is scoped to the case v0.339
// names. Widening it to every borrow RHS would reject every
// `t := someBorrowingCall()` shape and needs its own census.
//
// per §28.3, §15.3, §28.7, §22.3, §21.1, v0.339, #6609

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A STABLE rule identifier, so a wording tweak that rephrases the sentence
// does not silently break the row while an unrelated rule firing cannot
// satisfy it. §-reference plus the key term of the clause.
constexpr const char* kSelfAssign = "cannot self-assign";

// §28.3/§28.5's own diagnostic for the `owned` FIELD arm, pinned by ROW 4 so
// the local fix cannot be seen to have stolen or duplicated it.
constexpr const char* kOwnedField =
    "a borrow cannot be stored into an `owned` field";

// §21.1's own diagnostic for a `const` local, pinned by ROW 10.
constexpr const char* kConstAssign = "cannot assign to const local";

// A plain class with a public field, so the program has a value to own and
// nothing else that could pre-empt the rule under test.
const char* kCell = R"(
module M;
class Cell
  public value: int := 0;
end Cell;
)";

// ── ROW 1 — the defect: the issue's actual repro ──────────────────────────
// `var t := new Res(); t := t;` — an owned-state class-typed local storing its
// own unmarked read. ACCEPTED rc=0 before the fix.
BEZEL_TEST(self_assignment_owned_local_6609,
           owned_class_local_self_assign_rejected_spec_28_3_v0_339) {
    std::string src = std::string(kCell) + R"(
class Program
  public static method Main(): int
    var t := new Cell();
    t := t;
    return 0;
  end Main;
end Program;
)";
    auto r = runAotChecked(src);
    // REJECTED at compile time — the strict oracle: `!r.ran` plus a real
    // `<path>:<line>:<col>:` diagnostic location, so a crash or a toolchain
    // break cannot satisfy this row.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kSelfAssign));
    // NOT vacuous on the accept side: the driver really rejected, and the
    // needle is the rule's own, not some other diagnostic riding along.
    BEZEL_EXPECT_FALSE(r.ran);
}

// ROW 1b — the CONTROL ARM for ROW 1. The byte-identical file with `t := t;`
// removed. It must still compile and run, which is what proves the pair
// differs because of the self-assignment and not because of a parse error or
// a broken preamble.
BEZEL_TEST(self_assignment_owned_local_6609,
           control_without_the_self_assign_compiles_spec_28_3_v0_339) {
    std::string src = std::string(kCell) + R"(
class Program
  public static method Main(): int
    var t := new Cell();
    return 0;
  end Main;
end Program;
)";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── ROW 2 — a nullable owned local is still class-typed ────────────────────
// `var t: Cell? := new Cell(); t := t;` — v0.339 says "class-typed", and a
// `Cell?` is a class type. An arm gated on a non-nullable declared type would
// miss this.
BEZEL_TEST(self_assignment_owned_local_6609,
           nullable_owned_local_self_assign_rejected_spec_28_3_v0_339) {
    std::string src = std::string(kCell) + R"(
class Program
  public static method Main(): int
    var t: Cell? := new Cell();
    t := t;
    return 0;
  end Main;
end Program;
)";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kSelfAssign));
}

// ── ROW 3 — the BORROWED-state half of the rule stays legal ────────────────
// `var b := r;` binds a borrow-state local; `b := b;` then stores a borrow
// into a borrowed-state LHS, which §28.3's SECOND bullet explicitly requires
// ("a borrowed-state LHS requires a borrow (or `null`) RHS"). An arm that did
// not read the LHS's dataflow state would wrongly reject this. ACCEPTED
// rc=0 before the fix and after.
BEZEL_TEST(self_assignment_owned_local_6609,
           borrow_state_local_self_assign_accepted_spec_28_3) {
    std::string src = std::string(kCell) + R"(
class Program
  public static method Borrow(b2: Cell): int
    var b := b2;
    b := b;
    return 0;
  end Borrow;
  public static method Main(): int
    var t := new Cell();
    return Borrow(t);
  end Main;
end Program;
)";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kSelfAssign));
}

// ── ROW 4 — the `owned` FIELD arm keeps ITS OWN diagnostic ─────────────────
// `this.f := this.f;` is the same violation one level down, and §28.3's state
// match rejects it — but a DIFFERENT arm has owned that diagnostic since long
// before this fix, with its own §28.3/§28.5 wording. This row is the
// discriminator pair for ROW 1: if the new arm had reached fields it would
// either duplicate the message (two diagnostics for one store) or replace it
// (this row goes red).
BEZEL_TEST(self_assignment_owned_local_6609,
           owned_field_self_assign_keeps_28_5_diagnostic_spec_28_5) {
    std::string src = std::string(kCell) + R"(
class Holder
  public owned f: Cell := new Cell();
  public method Make(): int
    this.f := this.f;
    return 0;
  end Make;
end Holder;
class Program
  public static method Main(): int
    var h := new Holder();
    return h.Make();
  end Main;
end Program;
)";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // The PRE-EXISTING diagnostic, not the new one.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kOwnedField));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kSelfAssign));
}

// ── ROW 5 — §15.3's move-only no-op survives ──────────────────────────────
// §15.3 *Whole-value reassignment*: "A reassignment whose RHS consumes the LHS
// — self-assignment `arr := arr;` … is well-defined: … self-assignment is
// therefore a no-op. This rule holds at **locals**." `arr` there is an owning
// INLINE ARRAY, a move-only value type, and the no-op is stated for exactly
// that kind. An arm that did not exclude move-only value types would reject a
// no-op the spec defines. ACCEPTED rc=0 before the fix and after.
BEZEL_TEST(self_assignment_owned_local_6609,
           move_only_array_self_assign_noop_accepted_spec_15_3) {
    std::string src = std::string(kCell) + R"(
class Program
  public static method Main(): int
    var arr: [3 owned]Cell := [new Cell(), new Cell(), new Cell()];
    arr := arr;
    return 0;
  end Main;
end Program;
)";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kSelfAssign));
}

// ── ROW 6 — a primitive local carries no obligation ────────────────────────
BEZEL_TEST(self_assignment_owned_local_6609,
           primitive_local_self_assign_accepted_spec_28_3) {
    std::string src = R"(
module M;
class Program
  public static method Main(): int
    var i := 1;
    i := i;
    return 0;
  end Main;
end Program;
)";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kSelfAssign));
}

// ── ROW 7 — a string local carries no obligation either ────────────────────
// §28.3 (v0.339) says explicitly that an unclassified local "stays outside the
// ownership rules, as a `string` local does": nothing disposes it and its
// reads borrow from nothing, so there is no owned-state LHS to state-match.
BEZEL_TEST(self_assignment_owned_local_6609,
           string_local_self_assign_accepted_spec_28_3) {
    std::string src = R"(
module M;
class Program
  public static method Main(): int
    var s := "a";
    s := s;
    return 0;
  end Main;
end Program;
)";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kSelfAssign));
}

// ── ROW 8 — the sanctioned spelling: an explicit transfer ─────────────────
// `t := move t;` is §28.3's "a named owned binding under the explicit transfer
// marker". It is the way to write the same intent, and rejecting it would leave
// the rule with no spelling at all. ACCEPTED rc=0 before the fix and after.
BEZEL_TEST(self_assignment_owned_local_6609,
           marked_transfer_of_itself_accepted_spec_28_3) {
    std::string src = std::string(kCell) + R"(
class Program
  public static method Main(): int
    var t := new Cell();
    t := move t;
    return 0;
  end Main;
end Program;
)";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kSelfAssign));
}

// ── ROW 9 — two DISTINCT owned locals: ordinary drop-on-overwrite ─────────
// `t := u;` is not a self-assignment; an arm that compared SPELLINGS rather
// than SYMBOLS would wrongly reject it.
//
// This row is a COMPILE-TIME acceptance check (`parseAndCheckOwnership`)
// rather than a `runAot`/`runAotChecked` run, and the reason is §28.6, not
// convenience: an unmarked store of a named owned binding into another local
// is a BORROW store, so `t.Dispose()` afterwards is itself rejected — "cannot
// call `Dispose()` on `t`: it is a borrow (an alias) of `u`" — and the pair
// cannot be tidied without `move`, which would stop being the shape this row
// is about. Both oracles run the program, so both would see the resulting
// leak and turn a clean ACCEPT into a red for an unrelated reason. The claim
// under test is purely "this is NOT a self-assignment", which is a question
// about diagnostics, so it is asked of the diagnostics directly.
BEZEL_TEST(self_assignment_owned_local_6609,
           distinct_owned_locals_accepted_spec_28_7) {
    std::string src = std::string(kCell) + R"(
class Program
  public static method Main(): int
    var t := new Cell();
    var u := new Cell();
    t := u;
    return 0;
  end Main;
end Program;
)";
    auto r = parseAndCheckOwnership(src);
    // No ERROR at all — the store is accepted. (The pre-existing §28.3
    // auto-drop WARNING is expected and is a Warning, not an Error.)
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, kSelfAssign));
}

// ── ROW 10 — §21.1's const rule keeps its own diagnostic ──────────────────
// A `const` local is single-assignment (§21.1), so `t := t;` on one is
// §21.1's "cannot assign to const local", a DISTINCT rule that must fire as
// itself rather than being pre-empted by the §28.3 arm. Mirrors
// getter_return_binding_provenance_6404's
// `const_local_self_assignment_names_21_1` row.
BEZEL_TEST(self_assignment_owned_local_6609,
           const_local_self_assign_names_21_1_spec_21_1) {
    std::string src = std::string(kCell) + R"(
class Program
  public static method Main(): int
    const t := new Cell();
    t := t;
    return 0;
  end Main;
end Program;
)";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kConstAssign));
}

}  // namespace
