// §35.3 (v0.334) — the CHOICE-CASE no-temporary-source rule.
//
// §35.3 "Construction and payload ownership" states, of the three argument
// shapes a case payload accepts:
//
//   - An argument to an **owned payload slot** is an owned-consuming position
//     … a fresh producer or unbound owned temporary transfers with no marker.
//   - An argument to a **plain class- or interface-typed payload slot** is a
//     borrow, and §9.7's **no-temporary-source rule** applies verbatim:
//     constructing a choice that holds borrows (§35.4's holds-borrows property
//     — borrow-bearing or owning alike) from a temporary bound to a
//     borrow-carrying payload is rejected (bind the source to a named local
//     first).
//   - **Copyable value**, `string`, and runtime-managed payload arguments copy
//     or share per their type kinds.
//
// The second bullet had no enforcement anywhere. `MethodOwnership` carries a
// complete §9.7 no-temporary-source family — `checkRecordFromTemporary` (the
// record construction), `checkRecordWithFromTemporary` (#4344), and the
// recursive container walker (#5626) — but no CHOICE-CASE member, so
// `Holding.Some(new Widget())` compiled clean, stored a borrow whose source
// (the temporary) dies at the end of that very full expression, and left the
// choice holding a dangling borrow from birth. That is the §9.7 no-temporary-
// source defect the spec sentence rules out by name.
//
// "Verbatim" is literal, and the fix is a structural twin of the record check:
// same per-slot test (`fieldTypeIsBorrowCarrying`), same temporary predicate
// (`exprIsTemporary`), same `null` carve-out, same remedy, same "one
// diagnostic per construction" early return — with `caseDecl->payload` in place
// of `recordPositionalFieldSymbols`. §9.7 supplies the carrier difference
// itself: "Choices that hold borrows … are subject to this section's
// borrow-lifetime rules … with 'record' read as 'record or choice'
// throughout" (§9.7, v0.220, #2180).
//
// The holds-borrows GATE is the rule's own wording and is deliberately read
// "whatever the classification" (§3.6's holds-borrows row), so an OWNING
// choice holding a plain class payload beside its `owned` ones is covered —
// the row `owning_choice_holding_borrows_temporary_rejected` pins that.
//
// CORPUS ROW: tests/conformance/rules/cases/s35_3_0008.bzl pins the
// borrow-bearing shape (`Some(widget: Widget)`, `Holding.Some(new
// Widget())`). Its `diagnostic:` needle is `temporary`.
//
// per §35.3 (v0.334), §9.7, §35.4

#include <string>

#include "support/ownership_support.hpp"
#include "support/aot_support.hpp"  // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

// An ERROR diagnostic naming `sub`. Sub-pinned to the rule under test so an
// unrelated diagnostic (a §7.9 field-init error, say) can never satisfy a row.
static bool hasError(const OwnershipResult& r, const std::string& sub) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(sub) != std::string::npos) return true;
    }
    return false;
}

// The §35.3 reject's invariant prefix. The full message is long and names the
// case and payload; the rule-identifying half is "may not be constructed from
// a temporary", which only this rule renders.
static bool hasChoiceTemporaryError(const OwnershipResult& r) {
    return hasError(r, "may not be constructed from a temporary");
}

namespace {

// `Widget` is the borrow source; `Holding` is the canonical §35.4
// borrow-bearing choice (a plain, unmarked class payload); `Counting` is a
// value-like choice (every payload a value type) — the over-reject control.
// `Owned` carries an `owned` payload (an owned-consuming position, §35.3's
// first bullet); `Mixed` is an OWNING choice that ALSO holds a borrow, which
// §35.3's "borrow-bearing or owning alike" reaches.
const char* kCommon =
    "module M; "
    "class Widget public constructor() end constructor; end Widget; "
    "class Res public constructor() end constructor; "
    "  destructor end destructor; end Res; "
    "choice Holding Some(widget: Widget), None; end Holding; "
    "choice Counting Made(code: int), Named(label: string), None; "
    "  end Counting; "
    "choice Owned Got(owned r: Res), NotModified; end Owned; "
    "choice Mixed Held(owned r: Res, widget: Widget), None; end Mixed; ";

std::string mod(const std::string& decls) {
    return std::string(kCommon) + decls;
}

}  // namespace

// ===========================================================================
// REJECT — §35.3's second bullet, in its shapes.
// ===========================================================================

// CORPUS ROW s35_3_0008: the borrow-bearing choice, a fresh `new Widget()`
// producer bound to its plain class payload.
BEZEL_TEST(ownership_choice_case_from_temporary,
           borrow_bearing_choice_temporary_rejected_s35_3_0008) {
    auto r = parseAndCheckOwnership(mod(
        "class Program public static method Main() "
        "  var h := Holding.Some(new Widget()); "
        "  _ := h; "
        "end Main; end Program; "));
    BEZEL_EXPECT_TRUE(hasChoiceTemporaryError(r));
}

// An INTERFACE-typed payload is named by the same bullet ("plain class- OR
// interface-typed payload slot"), so the reject reaches it too.
BEZEL_TEST(ownership_choice_case_from_temporary,
           interface_payload_temporary_rejected_spec_35_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "interface ILog method Note(): int; end ILog; "
        "class Impl implements ILog public method Note(): int => 0; end method; "
        "  constructor() end constructor; end Impl; "
        "choice Holding Some(l: ILog), None; end Holding; "
        "class Program public static method Main() "
        "  var h := Holding.Some(new Impl()); "
        "  _ := h; "
        "end Main; end Program; ");
    BEZEL_EXPECT_TRUE(hasChoiceTemporaryError(r));
}

// A COLLECTION LITERAL is §9.7's own named temporary ("a collection or array
// literal"), reached through an `Array<T>`-typed plain payload — the exact
// worked example of the record rule, transported to a choice payload. It needs
// no inline-array carve-out: choice case payloads are class/interface types
// (§35.3), not inline arrays.
BEZEL_TEST(ownership_choice_case_from_temporary,
           collection_literal_payload_temporary_rejected_spec_35_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "choice Holding Some(items: Array<int>), None; end Holding; "
        "class Program public static method Main() "
        "  var h := Holding.Some([10, 20, 30]); "
        "  _ := h; "
        "end Main; end Program; ");
    BEZEL_EXPECT_TRUE(hasChoiceTemporaryError(r));
}

// §35.3's parenthetical — "borrow-bearing or owning alike" — is the load-
// bearing half of the gate: an OWNING choice (§35.4 rule 1, an `owned`
// payload) that ALSO carries a plain borrow-carrying payload HOLDS BORROWS
// and so takes the rule. This row is what makes the gate a real §35.4
// holds-borrows test rather than a synonym for "borrow-bearing".
BEZEL_TEST(ownership_choice_case_from_temporary,
           owning_choice_holding_borrows_temporary_rejected_spec_35_3) {
    auto r = parseAndCheckOwnership(mod(
        "class Program public static method Main() "
        "  var m := Mixed.Held(new Res(), new Widget()); "
        "  _ := m; "
        "end Main; end Program; "));
    BEZEL_EXPECT_TRUE(hasChoiceTemporaryError(r));
}

// §9.7's null-source form, carried over verbatim: a `null` argument stores no
// borrow, so it is never a dangling one and is explicitly permitted. The
// reject must NOT fire on it even though the slot is borrow-carrying and the
// shape is otherwise the rejected one.
BEZEL_TEST(ownership_choice_case_from_temporary,
           null_argument_to_borrow_payload_admitted_spec_9_7_null_source) {
    auto r = parseAndCheckOwnership(mod(
        "class Program public static method Main() "
        "  var h := Holding.Some(null); "
        "  _ := h; "
        "end Main; end Program; "));
    BEZEL_EXPECT_FALSE(hasChoiceTemporaryError(r));
}

// ===========================================================================
// ADMIT CONTROLS — the nearest neighbours §35.3 admits. A check that rejects
// too much is the same defect class pointed the other way, so each control
// states what would fail if the fix were deleted or over-broad.
// ===========================================================================

// ADMIT CONTROL 1 — the spec's own stated remedy: "bind the source to a named
// local first". Same construction, same borrow-bearing choice, the argument is
// a PLACE instead of a temporary. WHAT WOULD FAIL IF THE FIX WERE DELETED:
// nothing here — this is an admit row and pins the opposite: if `exprIsTemporary`
// were dropped from the check (rejecting every borrow-carrying payload
// argument regardless of its source), THIS row turns red. The REJECT rows above
// are what fail if the check is deleted.
// ADMIT CONTROL 2 is its sibling in the same program, proving the reject keys
// on the SOURCE SHAPE and not on the choice type.
BEZEL_TEST(ownership_choice_case_from_temporary,
           named_local_source_admitted_spec_35_3_remedy) {
    auto r = parseAndCheckOwnership(mod(
        "class Program public static method Main() "
        "  var w := new Widget(); "
        "  var h := Holding.Some(w); "
        "  _ := h; "
        "end Main; end Program; "));
    BEZEL_EXPECT_FALSE(hasChoiceTemporaryError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ADMIT CONTROL 2 — a PLACE argument that is not a bare identifier: an index
// slot, `exprIsTemporary`'s other non-temporary form. Fails if the temporary
// predicate narrowed to IdentifierExpr-only.
BEZEL_TEST(ownership_choice_case_from_temporary,
           index_expression_source_admitted_spec_9_7_place) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Widget public constructor() end constructor; "
        "  destructor end destructor; end Widget; "
        "choice Holding Some(widget: Widget), None; end Holding; "
        "class Program public static method Main() "
        "  using var arr := new OwningArray<Widget>(); "
        "  _ := arr.Add(new Widget()); "
        "  var h := Holding.Some(arr[0]); "
        "  _ := h; "
        "end Main; end Program; ");
    BEZEL_EXPECT_FALSE(hasChoiceTemporaryError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ADMIT CONTROL 3 — an `owned` payload slot is an OWNED-CONSUMING position
// (§35.3's FIRST bullet), not a borrow: "a fresh producer or unbound owned
// temporary transfers with no marker". A temporary there is an ordinary
// unbound owned temporary under §28.3 and is ADMITTED. This is the row that
// separates the two §35.3 bullets from each other: without the
// `modifier == "owned"` skip, `Owned.Got(new Res())` would be rejected by the
// second bullet's machinery, denying a program the spec admits.
BEZEL_TEST(ownership_choice_case_from_temporary,
           owned_payload_temporary_admitted_spec_35_3_first_bullet) {
    auto r = parseAndCheckOwnership(mod(
        "class Program public static method Main() "
        "  var f := Owned.Got(new Res()); "
        "  _ := f; "
        "end Main; end Program; "));
    BEZEL_EXPECT_FALSE(hasChoiceTemporaryError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ADMIT CONTROL 4 — a choice that holds NO borrows takes no such borrow, so a
// temporary bound to it is an ordinary unbound owned temporary. This is the
// holds-borrows GATE's own row: if the gate were keyed on the SLOT alone (any
// payload, borrow-carrying or not) rather than on §35.4's holds-borrows
// property, this value-like choice would be rejected and every temporary
// bound to any value-typed payload in every choice in the corpus would break.
BEZEL_TEST(ownership_choice_case_from_temporary,
           value_like_choice_temporary_admitted_spec_35_4_holds_borrows) {
    auto r = parseAndCheckOwnership(mod(
        "class Program public static method Main() "
        "  var c := Counting.Made(7); "
        "  _ := c; "
        "  var s := Counting.Named(\"x\"); "
        "  _ := s; "
        "end Main; end Program; "));
    BEZEL_EXPECT_FALSE(hasChoiceTemporaryError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ADMIT CONTROL 5 — an OWNING choice with NO borrow-carrying payload does not
// hold borrows, so §35.3's second bullet does not reach it even though the
// choice is owning. This row separates "owning" (which the reject row uses)
// from "holds borrows" (which the reject row actually keys on): without it,
// `Owned.Got(new Res())` could be admitted merely by being owning, and the
// owning+borrow row above could be admitted for the wrong reason.
BEZEL_TEST(ownership_choice_case_from_temporary,
           owning_choice_without_borrows_temporary_admitted_spec_35_4) {
    auto r = parseAndCheckOwnership(mod(
        "class Program public static method Main() "
        "  var f := Owned.Got(new Res()); "
        "  _ := f; "
        "end Main; end Program; "));
    BEZEL_EXPECT_FALSE(hasChoiceTemporaryError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
// ===========================================================================
// AOT ORACLE — this is a borrow/LIFETIME rule, so the rows that RUN are what
// the native sanitizer exercises. `parseAndCheckOwnership` is a compile-only
// oracle; without these two the ASan leg would report "0 checks skipped" while
// executing nothing. `runAotChecked` forks the real AOT path, links against
// the sanitizer runtime, and runs the binary. These are the §35.3 REMEDY
// shapes, so they must compile, run, and be clean under ASan/LSan.
// ===========================================================================

// The spec's stated remedy, end to end: bind the source to a named local whose
// scope outlives the choice, then construct. The choice really does hold a
// borrow of `w`, the program reads the borrowed value back through the match,
// and the sanitizer confirms nothing dangles. == 7.
//
// The source here is the CORRECTED spelling of each construct (a field named
// without the `_` convention, a `case` arm list closed by a bare `end`, an
// access through a `method` rather than a bare field read). An earlier draft
// used the `case` form with the arms separated by semicolons and read `Tag`
// directly off the bound pattern; that did not PARSE, so `runAotChecked`
// returned `ran == false` and the row failed on `BEZEL_EXPECT_TRUE(r.ran)`
// rather than on anything about §35.3 — the vacuous-oracle shape. Every AOT
// row below therefore asserts `r.ran` FIRST.
BEZEL_TEST(ownership_choice_case_from_temporary,
           aot_named_local_remedy_runs_clean_spec_35_3) {
    std::string src =
        "module M; "
        "class Widget "
        "  private Tag: int; "
        "  public constructor(tag: int) Tag := tag; end constructor; "
        "  public method Value(): int => Tag; "
        "end Widget; "
        "choice Holding Some(widget: Widget), None; end Holding; "
        "class Program "
        "  public static method Main(): int "
        "    var w := new Widget(7); "
        "    var h := Holding.Some(w); "
        "    case h of "
        "      Some(var got) => do return got.Value(); end, "
        "      None => do return 0; end "
        "    end; "
        "  end Main; "
        "end Program;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// The §35.3 FIRST bullet, end to end: an `owned` payload slot is an
// owned-consuming position, so a fresh producer transfers with no marker. The
// owning choice is bound in a `do` block so its §35.4 "disposal is defined by
// the active case" pass runs at block exit, and the sanitizer's LSan leg must
// find nothing leaked. == 3.
//
// This is the row that would fail loudly if the §35.3 check over-rejected the
// owned-consuming direction: it compiles, RUNS, and disposes cleanly. A check
// that keyed on the slot's TYPE rather than its `owned` MARKER would reject it
// at compile time.
BEZEL_TEST(ownership_choice_case_from_temporary,
           aot_owned_payload_temporary_runs_clean_spec_35_3) {
    std::string src =
        "module M; "
        "class Res "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "choice Owned Got(owned r: Res), NotModified; end Owned; "
        "class Program "
        "  public static method Main(): int "
        "    do "
        "      var f := Owned.Got(new Res()); "
        "      _ := f; "
        "    end; "
        "    return 3; "
        "  end Main; "
        "end Program;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}
