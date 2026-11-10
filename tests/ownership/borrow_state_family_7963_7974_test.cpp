// fix #7963 / #7974 (spec §28.3 / §28.3.3 / §28.6 / §28.8.1 / §17.8, v0.355)
// — the STATE-MATCH family of the ownership pass: a borrow assigned into an
// OWNED-state enumerator local (and the reverse), and a `move` marker on a
// BORROW-state local.
//
// ── THE TWO ISSUES ─────────────────────────────────────────────────────────
//
// #7963 — "a borrow assigned into an owned-state enumerator local, or the
// reverse". Two independent gaps, one per direction:
//
//   (A) the `!` SPELLING hid the owned-field read from the §28.6 machinery.
//       §17.8 (v0.350): "Over a named owned binding, `x!` is therefore
//       classified by its position exactly as the bare read `x` is" — the `!`
//       "is purely a type-system assertion" and "does not move, copy, or
//       borrow beyond what reading `x` already does". An `owned
//       IEnumerator<T>?` field is NULLABLE, so the field read is spelled
//       `this.En!` — and the ownership pass recorded NO alias source for that
//       spelling, so §28.6's "Dispose() through a borrow is a compile error"
//       had nothing to fire on. MEASURED on the base (r30-c1 @ 758e406c):
//         `var a := this.En;  a!.Dispose();`  → REJECTED
//         `var a := this.En!; a.Dispose();`   → ACCEPTED   ← the gap
//       and the CLASS-typed twin splits the same way (`var a := this.R;`
//       rejected, `var a := this.R!;` accepted), so the `!` — not the
//       enumerator — was the hole.
//
//   (B) the §28.3.3 state-match rule never reached an ENUMERATOR-typed
//       binding. §28.3.3 (v0.350), verbatim: "Assigning a borrow to an
//       owned-state LHS, or an owned value to a borrowed-state LHS, is
//       rejected — the mismatch would silently drop or double-hold a
//       disposal obligation." The other-binding arm (the #8137 fix-forward)
//       gates on `isBorrowableOwnedSinkType`, which reads every
//       declaration-less builtin head — the `IEnumerator<T>` /
//       `IOwningEnumerator<T>` heads among them — as NOT borrow-bearing, and
//       it additionally requires a BARE-IDENTIFIER RHS. So neither half
//       reached the enumerator family. MEASURED on the base:
//         `var a := G.Gen().GetEnumerator(); a := this.En!;`  → ACCEPTED
//         `var a := this.En!; a := G.It();`                    → ACCEPTED
//
// #7974 — "`move` of a borrow-state local". §28.8.1, quoted by the arm that
// already enforces it for a plain PARAMETER (Ownership_Checks.cpp's
// `recordMovesIn`): "`move` is not admitted on a borrow — the marker asserts
// an ownership transfer, but a PLAIN PARAMETER … carries no obligation to
// transfer." The same sentence governs a borrow-state LOCAL: the marker
// asserts a transfer, and a local holding a borrow has no obligation to
// transfer. §22.3 lists "the marker on a borrow" among the marker's spelling
// errors, and §28.3.3's second half says the same from the state-match side.
//
// THE GAP: the rule was reachable only through the PARAMETER shape and the
// IF-ARM shape (the latter already rejected, by the §28.3 K3.4 marked-arm
// machinery: "the arm transfer marker `move a` requires a named owned
// binding — `a` is not an owned binding at this point"). The plain-binding,
// `??` and return forms were admitted and compiled a value nobody owned into
// an OWNED producer. MEASURED on the base (r30-c1 @ 758e406c), rc=0, no
// ownership diagnostic:
//   `var a := h.En!; var b := move a;`
//   `var a := h.En!; var b := move a ?? G.It();`
//   `var t := this.En!; return move t;`
//
// THE PREDICATE. A `move` marker at a TRANSFER POSITION whose operand is a
// BORROW-STATE local — and, the load-bearing part, a local that is an ALIAS
// of a value someone else in this frame still owns (the pass's own
// `disposeAliasSource`). The BORROWED state alone is not the same thing: it
// is also the state the pass tracks for a BOX-typed local, a
// COLLECTION-LITERAL local, a `CancellationToken` and a borrow of a plain
// PARAMETER, and every one of those keeps an admitted `move`.
//
// ── WHAT IS AND IS NOT REJECTED ────────────────────────────────────────────
//
// The rows below pin the rejections and the controls that must NOT change:
//
//   * §28.3's PRODUCER half is untouched: a fresh producer (`G.It()`) into an
//     `owned` enumerator FIELD is accepted (the field then owns the frame),
//     and `move h.En` out of an `owned` field is accepted (§28.8's explicit
//     transfer).
//   * §15.3's MOVE-ONLY VALUE TYPE keeps its sanctioned `move`: an owning
//     inline array's "Borrowed" state is an artefact of the value-type
//     classification, not a borrow, so `var b := move arr;` stays accepted.
//   * an OWNED local keeps its `move`: `var t := G.It(); var b := move t;`
//     stays accepted, and the program still runs.
//   * the BORROWED state ALONE is not "this local holds a borrow": the pass
//     tracks a BORROWED state for four shapes that carry no obligation to
//     transfer and whose `move` is admitted — a BOX-typed local (§28.9), a
//     COLLECTION-LITERAL local (§15.4's context-typed literal), a
//     `CancellationToken` (stdlib §6.1 — no obligation at all), and a borrow
//     of a plain PARAMETER. The rule's predicate is the pass's OWN alias
//     fact (`disposeAliasSource` — the §28.6 "it is a borrow (an alias) of
//     `X`, which still owns it" source), so the two rules agree by
//     construction: §28.6 refuses a `Dispose()` through such an alias, and
//     §28.3.7 / §28.8.1 refuse a `move` marker on it.
//   * a plain PARAMETER keeps its OWN §28.8.1 wording ("is a plain
//     parameter"), and the new local arm does not fire on it — the parameter
//     twin is reported once, by the arm that owns it.
//   * the §28.3 K3.4 IF-ARM twin keeps its own diagnostic: the new arm must
//     not fire on the marked arm, so the row pins the single report.
//   * the CLASS-typed twins of the §28.3.3 arms stay ACCEPTED on purpose:
//     the general rule for every type and every RHS spelling is issue
//     #4846's (a new reject rule over the whole corpus, with its own
//     blast-radius measurement). This change is the ENUMERATOR family's
//     instance of it, which is what #7963 asks for.
//
// ── THE SECOND ATTEMPT — WHAT THE FIRST ONE MISSED ─────────────────────────
//
// The first attempt closed #7974 completely but left #7963's own population
// open: its §28.3.3 arm read ONE LHS state (`Owned`), recognised ONE RHS
// spelling (a member read), and lived in ONE handler
// (`MethodOwnership::handleAssignment`). Three reachable shapes were left
// behind, and this file's lower half pins each of them:
//
//   * the LHS-STATE list. §28.3.3 (v0.350) enumerates the LHS states "By
//     LHS state": the owned-state LHS (item 1), the MOVED-FROM LHS (item 2:
//     "A **moved-from** LHS is an owned-state LHS holding no live value: it
//     requires an owned RHS and displaces nothing") and the
//     EXPLICITLY-DISPOSED LHS (item 3: "An **explicitly-disposed** LHS
//     requires an owned RHS too"). The first attempt read only `Owned`, so
//     the "dispose first, then store the borrow" and "move the slot out,
//     then store the borrow" spellings fell through it. MEASURED on the base:
//     the issue's own `a03` / `a08` / `a09` / `a11` / `a22` / `a26` / `a27`
//     / `a36` / `a44` probes compiled rc=0 with zero ownership diagnostics.
//   * the ARM-MEET RHS. §28.3's arm-meet row: the meet of a borrow arm with
//     anything else is not an owned value. The issue's own `a40` probe —
//     `a := (if c then this.En! else G.Gen().GetEnumerator());` — compiled
//     rc=0 and LEAKED 2000 B / 50 allocations.
//   * the LAMBDA-BODY spelling. A lambda body's `AssignmentStmt` is handled
//     by a different, deliberately NARROWER arm than `handleAssignment` (and
//     the dataflow that drives `handleAssignment` never enters a lambda
//     body), so the §28.3.3 slot was open there. The issue's own `a21` and
//     `a23` probes compiled rc=0 and LEAKED 4000 B / 100 allocations each.
//
// and ONE more, on the §28.6 side:
//
//   * the ASSIGNMENT spelling of the `!`-transparent owned-field read. The
//     first attempt added the `!`-peel at the DECLARATION site only, so the
//     same §28.6 rule answered differently at the two spellings — the
//     defect class #7963 names. The `disposeAliasSource` derivation in
//     `handleAssignment` accepted a bare-identifier RHS only, with no
//     `exprIsOwnedFieldRead` fallback (unlike `handleLocalDecl` and
//     `handleLocalConstDecl`).
//
// per §28.3, §28.3.3, §28.6, §28.8.1, §22.3, §17.8, §15.3, §29.3, v0.355
// #7963, #7974

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::hasErrorMessage;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

// ── the STABLE rule identifiers ───────────────────────────────────────────
// A wording tweak that rephrases the sentence must not silently break a row,
// and an unrelated rule firing must not satisfy one.

// §28.6 (v0.350) *Who may call Dispose()* — the borrow-through-`Dispose()`
// rejection.
constexpr const char* kDisposeThroughBorrow = "it is a borrow (an alias) of";

// §28.3 (v0.355) *State match on reassignment*, first half — a borrow into an
// owned-state LHS.
constexpr const char* kOwnedStateGetsBorrow =
    "is in the OWNED state and the right-hand side is a BORROW";

// §28.3.3 (v0.350) *State match on reassignment*, "By LHS state" — items 2
// and 3: the MOVED-FROM and the EXPLICITLY-DISPOSED slot require an owned
// RHS exactly as the OWNED one does. The state NAME is what discriminates
// the three rows from one another.
constexpr const char* kDisposedStateGetsBorrow =
    "is in the DISPOSED state and the right-hand side is a BORROW";
constexpr const char* kMovedFromStateGetsBorrow =
    "is in the MOVED-FROM state and the right-hand side is a BORROW";

// The ARM-MEET RHS spelling (an `if`/`case` expression whose meet is not an
// owned meet) — the §28.3.3 slot with no single owned field to name, so the
// diagnostic opens differently from the member-read spelling.
constexpr const char* kArmMeetBorrowStore = "cannot assign a BORROW to";

// §28.3 (v0.355) *State match on reassignment*, second half — an owned value
// into a borrowed-state LHS.
constexpr const char* kBorrowedStateGetsOwned = "is in the BORROWED state";

// §28.3 / §28.8.1 — the `move`-on-a-borrow rejection, NEW local wording.
constexpr const char* kMoveNotAdmitted = "`move` is not admitted on a borrow";
// …and the PRE-EXISTING parameter wording, which the new arm must not
// shadow and must not duplicate.
constexpr const char* kMoveNotAdmittedParameter = "is a plain parameter";
// The local clause of the new wording — the discriminator that proves a row
// was reported by the NEW arm and not by the parameter arm.
constexpr const char* kBorrowCarriesNoObligation =
    "is a borrow and carries no obligation to transfer";

// §28.3 K3.4 (v0.209) — the IF-ARM twin's own diagnostic.
constexpr const char* kArmTransferMarker = "requires a named owned binding";

// ── the programs ──────────────────────────────────────────────────────────
//
// Each preamble leaves `class H` OPEN, so a row can add its own methods to
// it; `prog` closes `class H` and appends the `Main` class.

// The enumerator family: a producer of `IEnumerable<int>`, a producer of
// `IEnumerator<int>`, and a holder with two `owned` enumerator fields
// (nullable, so the field read is spelled `!`).
const char* kEnumHdr = R"(module M;
class G
  public static method Gen(): IEnumerable<int>
    try
      yield return 1;
      yield return 2;
    finally
      Console.WriteLine("fin");
    end;
  end Gen;
  public static method It(): IEnumerator<int>
    try
      yield return 5;
    finally
      Console.WriteLine("fin");
    end;
  end It;
end G;
class H
  public owned En: IEnumerator<int>? := null;
  public owned E2: IEnumerator<int>? := null;
  public method Fill()
    En := G.Gen().GetEnumerator();
  end Fill;
)";

// A plain class with a `owned` field, for the class-typed twins.
const char* kClassHdr = R"(module M;
class Res
  public constructor() end constructor;
  destructor
    Console.WriteLine("fin");
  end destructor;
end Res;
class H
  public owned R: Res := new Res();
)";

// The class preamble of the `!`-transparency rows: a NULLABLE `owned` field,
// so `this.R!` is the spelling.
const char* kClassNullableHdr = R"(module M;
class Res
  public constructor() end constructor;
  destructor
    Console.WriteLine("fin");
  end destructor;
end Res;
class H
  public owned R: Res? := new Res();
)";

// A plain owning class, for the `move`-on-a-borrow rows.
const char* kBoxHdr = R"(module M;
class Box
  public V: int := 0;
  public constructor() end constructor;
  destructor
    Console.WriteLine("fin");
  end destructor;
end Box;
class H
  public owned B: Box? := null;
  public method Fill()
    B := new Box();
  end Fill;
)";

// Assemble a program: `hdr` + `methods` (the body of `class H`) + the
// `Main` class running `mainBody`.
std::string prog(const std::string& hdr, const std::string& methods,
                 const std::string& mainBody) {
    return hdr + methods + "end H;\n"
                  "class C\n"
                  "  public static method Main(): int\n" +
           mainBody + "    return 0;\n  end Main;\nend C;\n";
}

// The `Main` body that builds a holder, fills it and runs `body` on it.
std::string holderMain(const std::string& body) {
    return "    using var h := new H();\n    h.Fill();\n" + body;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// #7963 (A) — the `!` spelling hides the owned-field read from §28.6
// ═══════════════════════════════════════════════════════════════════════════

// The issue's own shape: an `owned IEnumerator<T>?` field is nullable, so the
// read is `this.En!`, and the local bound to it is a BORROW of a value the
// FIELD still owns. §28.6 (v0.350) *Who may call Dispose()* rejects a
// `Dispose()` through such a borrow.
//
// MEASURED on the base (r30-c1 @ 758e406c): ACCEPTED, rc=0, no ownership
// diagnostic — the `!` spelling recorded no alias source, so the rule had
// nothing to fire on.
BEZEL_TEST(borrow_state_family_7963_7974,
           enumerator_bang_read_dispose_through_borrow_rejected_spec_28_6) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        kEnumHdr,
        "  public method Use()\n"
        "    var a := this.En!;\n"
        "    a.Dispose();\n"
        "  end Use;\n",
        holderMain("    h.Use();\n")));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kDisposeThroughBorrow));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "§28.6"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "`En`"));
}

// The CLASS-typed twin of the row above, with the same NULLABLE `owned`
// field: §17.8 (v0.350) makes the `!` spelling transparent, so the class
// twin and the enumerator twin must answer alike. MEASURED on the base:
// ACCEPTED, rc=0 (while the BARE spelling of the same read is rejected — the
// `!` was the hole).
BEZEL_TEST(borrow_state_family_7963_7974,
           class_bang_read_dispose_through_borrow_rejected_spec_28_6) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        kClassNullableHdr,
        "  public method Use()\n"
        "    var a := this.R!;\n"
        "    a.Dispose();\n"
        "  end Use;\n",
        "    using var h := new H();\n    h.Use();\n"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kDisposeThroughBorrow));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "§28.6"));
}

// CONTROL — the BARE spelling of the same read was ALREADY rejected before
// this change and must stay rejected: the fix makes the two spellings agree,
// it does not relax the bare one. §17.8 / §28.6 (v0.350).
BEZEL_TEST(borrow_state_family_7963_7974,
           bare_read_dispose_through_borrow_control_rejected_spec_28_6) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        kEnumHdr,
        "  public method Use()\n"
        "    var a := this.En;\n"
        "    a!.Dispose();\n"
        "  end Use;\n",
        holderMain("    h.Use();\n")));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kDisposeThroughBorrow));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "§28.6"));
}

// ═══════════════════════════════════════════════════════════════════════════
// #7963 (B) — §28.3.3's state match on the ENUMERATOR family
// ═══════════════════════════════════════════════════════════════════════════

// The issue's `a01` / `a08` / `a10` shape: an OWNED-state enumerator local
// (a fresh producer) receiving a BORROW of the `owned` field. §28.3.3
// (v0.350): "Assigning a borrow to an owned-state LHS … is rejected — the
// mismatch would silently drop or double-hold a disposal obligation."
//
// MEASURED on the base: ACCEPTED, rc=0 — the other-binding arm gates on
// `isBorrowableOwnedSinkType` (an enumerator head is not borrow-bearing) and
// on a BARE-IDENTIFIER RHS (this RHS is a member read).
BEZEL_TEST(borrow_state_family_7963_7974,
           borrow_into_owned_enumerator_local_rejected_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        kEnumHdr,
        "  public method Use()\n"
        "    var a := G.Gen().GetEnumerator();\n"
        "    a := this.En!;\n"
        "    a.Dispose();\n"
        "  end Use;\n",
        holderMain("    h.Use();\n")));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kOwnedStateGetsBorrow));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "§28.3"));
}

// The MIRROR half — the issue's "(or the reverse)": a BORROWED-state
// enumerator local (bound to a borrow of the `owned` field) receiving an
// OWNED value (a fresh producer). §28.3.3 (v0.350): "…or an owned value to a
// borrowed-state LHS, is rejected". §28.3's borrowed-state bullet: "a
// borrowed-state LHS requires a borrow (or `null`) RHS".
//
// MEASURED on the base: ACCEPTED, rc=0.
BEZEL_TEST(borrow_state_family_7963_7974,
           owned_into_borrowed_enumerator_local_rejected_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        kEnumHdr,
        "  public method Use()\n"
        "    var a := this.En!;\n"
        "    a := G.It();\n"
        "    a.Dispose();\n"
        "  end Use;\n",
        holderMain("    h.Use();\n")));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kBorrowedStateGetsOwned));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "§28.3"));
}

// The issue's `a08` / `a10` shape: the same violation through an `owned`
// PARAMETER — `e` is an OWNED-state enumerator binding (the parameter owns
// the frame it was handed) receiving a BORROW of the `owned` field. §28.3.3
// (v0.350), same rule, same diagnostic.
//
// MEASURED on the base: ACCEPTED, rc=0.
BEZEL_TEST(borrow_state_family_7963_7974,
           borrow_into_owned_enumerator_parameter_rejected_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        kEnumHdr,
        "  public method Y(owned e: IEnumerator<int>)\n"
        "    e := this.En!;\n"
        "    e.Dispose();\n"
        "  end Y;\n",
        holderMain("    h.Y(h.En!);\n")));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kOwnedStateGetsBorrow));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "§28.3"));
}

// CONTROL — a FRESH PRODUCER stored into an `owned` enumerator FIELD: the
// field owns the frame outright, no borrow, no error. §28.3 Producers
// (v0.350).
BEZEL_TEST(borrow_state_family_7963_7974,
           fresh_producer_into_owned_field_control_accepted_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        prog(kEnumHdr, "", holderMain("    h.E2 := G.It();\n")));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL — `move` OUT of an `owned` field through the owned local transfers
// the frame (§28.8); the store is not a borrow.
BEZEL_TEST(borrow_state_family_7963_7974,
           move_out_of_owned_field_control_accepted_spec_28_8) {
    OwnershipResult r = parseAndCheckOwnership(
        prog(kEnumHdr, "", holderMain("    h.E2 := move h.En;\n")));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ═══════════════════════════════════════════════════════════════════════════
// #7974 — a `move` marker on a BORROW-state local
// ═══════════════════════════════════════════════════════════════════════════

// The plain-BINDING form: `var b := move a;` over a borrow-state `a`.
// §28.8.1 (v0.350) via §28.3: the marker asserts an ownership transfer, and
// a local holding a borrow carries no obligation to transfer.
//
// MEASURED on the base (r30-c1 @ 758e406c): ACCEPTED, rc=0.
BEZEL_TEST(borrow_state_family_7963_7974,
           move_of_borrow_local_binding_rejected_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        kBoxHdr, "",
        "    using var h := new H();\n"
        "    h.Fill();\n"
        "    var a := h.B!;\n"
        "    using var b := move a;\n"
        "    b.V := 1;\n"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMoveNotAdmitted));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kBorrowCarriesNoObligation));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "§28.3"));
    // The new arm owns this report: the PARAMETER twin's wording must not be
    // what fired.
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, kMoveNotAdmittedParameter));
}

// The RETURN form: `return move t;` over a borrow-state `t` handed the
// caller a value the pass then classified as an OWNED producer. §28.3 /
// §28.8.1 (v0.350).
//
// MEASURED on the base: ACCEPTED, rc=0.
BEZEL_TEST(borrow_state_family_7963_7974,
           move_of_borrow_local_return_rejected_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        kEnumHdr,
        "  public method Get(): IEnumerator<int>\n"
        "    var t := this.En!;\n"
        "    return move t;\n"
        "  end Get;\n",
        holderMain("    h.Get().Dispose();\n")));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMoveNotAdmitted));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kBorrowCarriesNoObligation));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "§28.3"));
}

// The COALESCE form: `var b := move a ?? G.It();`. A.7 (v0.355)
// `MoveExpr ::= "move" PrimaryExpr`, so `move a ?? x` parses as
// `(move a) ?? x` — the `??`'s LEFT operand is the transfer position.
//
// MEASURED on the base: ACCEPTED, rc=0.
BEZEL_TEST(borrow_state_family_7963_7974,
           move_of_borrow_local_coalesce_rejected_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        kEnumHdr, "",
        holderMain(
            "    var ok := h.En!.MoveNext();\n"
            "    var a: IEnumerator<int>? := h.En;\n"
            "    var b := move a ?? G.It();\n"
            "    b.Dispose();\n")));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMoveNotAdmitted));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kBorrowCarriesNoObligation));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "§28.3"));
}

// The ASSIGNMENT form: `b := move a;` over a borrow-state `a`. The same rule
// at the assignment's transfer position. §28.3 / §28.8.1 (v0.350).
BEZEL_TEST(borrow_state_family_7963_7974,
           move_of_borrow_local_assignment_rejected_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        kBoxHdr, "",
        "    using var h := new H();\n"
        "    h.Fill();\n"
        "    var a := h.B!;\n"
        "    var b := new Box();\n"
        "    b := move a;\n"
        "    b.V := 1;\n"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMoveNotAdmitted));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kBorrowCarriesNoObligation));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "§28.3"));
}

// The IF-ARM twin — the WORKING twin of this rule, and a MUST-NOT-CHANGE
// row: the §28.3 K3.4 marked-arm machinery already rejects it, with its OWN
// diagnostic. The new arm must not fire on it, so the row pins BOTH the
// presence of the K3.4 report and the ABSENCE of the new §28.8.1 report —
// one rule, one report.
BEZEL_TEST(borrow_state_family_7963_7974,
           move_of_borrow_local_if_arm_keeps_k3_4_report_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        kEnumHdr, "",
        holderMain(
            "    var ok := h.En!.MoveNext();\n"
            "    var a := h.En!;\n"
            "    var b := (if ok then move a else G.It());\n"
            "    b.Dispose();\n")));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kArmTransferMarker));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "§28.3"));
    // The new arm must NOT double-report the same operand.
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, kBorrowCarriesNoObligation));
}

// CONTROL — a plain PARAMETER's `move` keeps the arm that owns it, with the
// parameter wording, and the new local arm does not fire on it. §28.8.1
// (v0.350) / `recordMovesIn` (Ownership_Checks.cpp).
BEZEL_TEST(borrow_state_family_7963_7974,
           move_of_plain_parameter_keeps_parameter_report_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        kBoxHdr,
        "  public static method Take(b: Box): int\n"
        "    var x := move b;\n"
        "    return 0;\n"
        "  end Take;\n",
        "    var t := new Box();\n"
        "    return Take(t);\n"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMoveNotAdmitted));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMoveNotAdmittedParameter));
    // The local clause of the NEW wording must not be what fired.
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, kBorrowCarriesNoObligation));
}

// CONTROL — an OWNED local keeps its `move`: the marker transfers a value the
// local owns. §28.3 (v0.355) / §28.8.1. The program must COMPILE AND RUN —
// this is the accept side of the new rule, and it is not vacuous.
BEZEL_TEST(borrow_state_family_7963_7974,
           move_of_owned_local_control_compiles_and_runs_spec_28_3) {
    auto r = runAot(prog(kEnumHdr, "",
                         "    var t := G.It();\n"
                         "    var b := move t;\n"
                         "    b.Dispose();\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kMoveNotAdmitted));
}

// CONTROL — §15.3's MOVE-ONLY VALUE TYPE keeps its sanctioned `move`: an
// owning inline array's "Borrowed" state is an artefact of the value-type
// classification, not a borrow, so the bare `move` is the normal — and for a
// plain (non-`owned`) parameter the only meaningful — way to transfer. The
// program must COMPILE AND RUN. §15.3 (v0.355).
BEZEL_TEST(borrow_state_family_7963_7974,
           move_of_move_only_value_type_control_compiles_and_runs_spec_15_3) {
    auto r = runAot(
        "module M;\n"
        "class Cell\n"
        "  public value: int := 0;\n"
        "end Cell;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var arr: [3 owned]Cell := [new Cell(), new Cell(), new Cell()];\n"
        "    var b := move arr;\n"
        "    Console.WriteLine(\"x\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kMoveNotAdmitted));
}

// CONTROL — a BOX-typed local keeps its `move`: `Object` is a box, and a box
// carries NO disposal obligation (§28.9), so the pass tracks the local's state
// as BORROWED while the local OWNS the value the marked transfer handed it.
// The BORROWED state ALONE is therefore not "this local holds a borrow" —
// the rule's predicate is the pass's own `disposeAliasSource` alias fact, and
// this shape has none. §28.9 (v0.355).
BEZEL_TEST(borrow_state_family_7963_7974,
           move_of_box_typed_local_control_accepted_spec_28_9) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res public constructor() end constructor; end Res; "
        "class C "
        "  public static method Main(): int "
        "    var s: Object := new Res(); "
        "    var y: Object := move s; "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL — a COLLECTION-LITERAL local keeps its `move`: the literal is
// CONTEXT-TYPED (§15.4), so the pass cannot classify the production and
// tracks the local's state as BORROWED, while §28.3's producers table makes
// it the OWNER and `move a` is §28.3.7's sanctioned transfer. §28.3
// Producers / §15.4 (v0.350).
BEZEL_TEST(borrow_state_family_7963_7974,
           move_of_collection_literal_owner_control_accepted_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var a: Array<int> := [1,2,3]; "
        "    var il: IList<int> := move a; "
        "    il.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL — a `CancellationToken` carries NO disposal obligation at all
// (stdlib §6.1), so there is nothing for the marker to transfer and the
// spelling stays admitted. §28.3 / stdlib §6.1 (v0.355).
BEZEL_TEST(borrow_state_family_7963_7974,
           move_of_cancellation_token_control_accepted_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var cts := new CancellationTokenSource(); "
        "    var tok: CancellationToken := cts.Token; "
        "    var t2 := move tok; "
        "    cts.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL — a borrow of a PLAIN (unconstrained generic) PARAMETER keeps its
// `move`: `t` is a borrow of a borrow, and the pass cannot know whether the
// substituted value is a class, so the marker is not a transfer it can
// refuse. §28.3.7 / §28.8.1 (v0.355).
BEZEL_TEST(borrow_state_family_7963_7974,
           move_of_plain_parameter_alias_control_accepted_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; class Box<T> "
        "  public static method Try(x: T): T "
        "    var t := x; "
        "    var y := move t; "
        "    return y; "
        "  end Try; "
        "end Box;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The end-to-end shape: the DRIVER rejects the issue's own program, with a
// real source location — not merely the in-process ownership pass. The
// strict oracle (`failedAtCompileTimeWithRealLocation`) distinguishes a
// genuine compile-time rejection from a crash, a hang or a toolchain break.
// #7963 (A).
BEZEL_TEST(borrow_state_family_7963_7974,
           enumerator_bang_read_dispose_rejected_by_the_driver_spec_28_6) {
    auto r = runAot(prog(
        kEnumHdr,
        "  public method Use()\n"
        "    var a := this.En!;\n"
        "    a.Dispose();\n"
        "  end Use;\n",
        holderMain("    h.Use();\n")));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kDisposeThroughBorrow));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The end-to-end shape for #7974's binding form. #7974.
BEZEL_TEST(borrow_state_family_7963_7974,
           move_of_borrow_local_rejected_by_the_driver_spec_28_3) {
    auto r = runAot(prog(
        kBoxHdr, "",
        "    using var h := new H();\n"
        "    h.Fill();\n"
        "    var a := h.B!;\n"
        "    using var b := move a;\n"
        "    b.V := 1;\n"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kMoveNotAdmitted));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ═══════════════════════════════════════════════════════════════════════════
// #7963 (B) — the §28.3.3 slot, REACHED — the LHS-state list, the ARM-MEET
// RHS, and the LAMBDA-BODY spelling (fix #7963, second attempt).
//
// The v1 arm of this fix read ONE LHS state — `Owned` — and ONE RHS spelling
// — a member read — and lived in ONE handler — `handleAssignment`. §28.3.3
// (v0.350) names three LHS states ("By LHS state": the owned-state LHS, the
// MOVED-FROM LHS, the EXPLICITLY-DISPOSED LHS), and a lambda body's
// assignment is handled by a different, deliberately narrower arm.
//
// MEASURED on the base (r30-c1 @ 758e406c) and on the v1 tip: the issue's
// own probes `a03`, `a08`, `a09`, `a11`, `a22`, `a26`, `a27`, `a36`, `a40`
// and `a44` compiled rc=0 with zero ownership diagnostics, and `a40`,
// `a21` and `a23` LEAKED (2000 B / 50 allocations, 4000 B / 100, 4000 B /
// 100).
// ═══════════════════════════════════════════════════════════════════════════

// The EXPLICITLY-DISPOSED LHS: the slot was disposed, and a BORROW is then
// stored into it. §28.3.3 (v0.350), "By LHS state" item 3: "An
// **explicitly-disposed** LHS requires an owned RHS too and displaces a
// **logically disposed** value". The issue's own `a03` / `a08` / `a11` /
// `a22` / `a26` / `a27` / `a36` / `a44` probes.
//
// MEASURED on the base: ACCEPTED, rc=0, zero ownership diagnostics.
BEZEL_TEST(borrow_state_family_7963_7974,
           borrow_into_disposed_enumerator_local_rejected_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        kEnumHdr,
        "  public method Use()\n"
        "    var a := G.Gen().GetEnumerator();\n"
        "    a.Dispose();\n"
        "    a := this.En!;\n"
        "  end Use;\n",
        holderMain("    h.Use();\n")));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kDisposedStateGetsBorrow));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "§28.3"));
}

// The same violation through an `owned` PARAMETER — the issue's own `a08`
// probe. The parameter owns the frame it was handed, so the slot is an
// owned-state slot; after the explicit `Dispose()` it is the
// EXPLICITLY-DISPOSED one.
//
// MEASURED on the base: ACCEPTED, rc=0.
BEZEL_TEST(borrow_state_family_7963_7974,
           borrow_into_disposed_enumerator_parameter_rejected_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        kEnumHdr,
        "  public method Y(owned e: IEnumerator<int>)\n"
        "    e.Dispose();\n"
        "    e := this.En!;\n"
        "  end Y;\n",
        holderMain("    h.Y(h.En!);\n")));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kDisposedStateGetsBorrow));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "§28.3"));
}

// The MOVED-FROM LHS: the slot's obligation was transferred out, and a
// BORROW is then stored into it. §28.3.3 (v0.350), "By LHS state" item 2: "A
// **moved-from** LHS is an owned-state LHS holding no live value: it requires
// an owned RHS and displaces nothing". The issue's own `a09` probe.
//
// MEASURED on the base: ACCEPTED, rc=0.
BEZEL_TEST(borrow_state_family_7963_7974,
           borrow_into_moved_from_enumerator_local_rejected_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        kEnumHdr,
        "  public method Y(owned e: IEnumerator<int>)\n"
        "    var keep := move e;\n"
        "    e := this.En!;\n"
        "    keep.Dispose();\n"
        "  end Y;\n",
        holderMain("    h.Y(h.En!);\n")));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMovedFromStateGetsBorrow));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "§28.3"));
}

// The ARM-MEET RHS spelling: the RHS is an `if` expression whose meet is not
// an owned meet — one arm is a borrow of the `owned` field, the other a
// fresh producer. §28.3's arm-meet row: the meet of a borrow arm with
// anything else is not an owned value, so the store would install a borrow
// (or the mixed meet §28.3 K7.3 rejects outright) into the slot. The issue's
// own `a40` probe — which LEAKED 2000 B / 50 allocations on the base.
//
// MEASURED on the base: ACCEPTED, rc=0, 2000 B / 50 allocations leaked.
BEZEL_TEST(borrow_state_family_7963_7974,
           arm_meet_borrow_into_disposed_enumerator_local_rejected_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        kEnumHdr,
        "  public method Use(i: int)\n"
        "    var a := G.Gen().GetEnumerator();\n"
        "    a.Dispose();\n"
        "    a := (if i mod 2 = 0 then this.En! else G.Gen().GetEnumerator());\n"
        "  end Use;\n",
        holderMain("    h.Use(1);\n")));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kArmMeetBorrowStore));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kDisposedStateGetsBorrow));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "§28.3"));
}

// The LAMBDA-BODY spelling. A lambda body's `AssignmentStmt` is handled by a
// different, deliberately NARROWER arm than `handleAssignment` — "only
// `checkRead` … + `recordMovesIn` … — NOT the full `handleAssignment`" — and
// the dataflow that drives `handleAssignment` never enters a lambda body, so
// the §28.3.3 slot was open in this spelling. The issue's own `a21` probe —
// which LEAKED 4000 B / 100 allocations on the base.
//
// MEASURED on the base: ACCEPTED, rc=0, 4000 B / 100 allocations leaked.
BEZEL_TEST(borrow_state_family_7963_7974,
           lambda_body_borrow_into_enumerator_local_rejected_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        kEnumHdr,
        "  public method Use()\n"
        "    const f := lambda() => do\n"
        "      var a := G.Gen().GetEnumerator();\n"
        "      a := this.En!;\n"
        "    end;\n"
        "    f();\n"
        "  end Use;\n",
        holderMain("    h.Use();\n")));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kOwnedStateGetsBorrow));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "§28.3"));
}

// #7963 (A) at the ASSIGNMENT spelling: the `!`-transparent owned-field read
// stored into a local by an ASSIGNMENT must record the same alias source the
// DECLARATION spelling records, so §28.6's "Dispose() through a borrow is a
// compile error" answers ALIKE at the two spellings.
//
// The store here is LEGAL on its own — the local is BORROW-state, and §28.3's
// borrowed-state bullet admits a borrow RHS — so the rejection comes from
// the §28.6 rule alone, which is exactly the point: the alias source must
// survive the store.
//
// MEASURED on the base and on the v1 tip: ACCEPTED, rc=0 — the assignment
// spelling recorded NO alias source, so the `.Dispose()` through the borrow
// had nothing to fire on (the declaration twin `var a := this.En!;
// a.Dispose();` is rejected on both).
BEZEL_TEST(borrow_state_family_7963_7974,
           assignment_site_bang_read_dispose_through_borrow_rejected_spec_28_6) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        kEnumHdr,
        "  public method Use()\n"
        "    var a := this.En!;\n"
        "    a := this.En!;\n"
        "    a.Dispose();\n"
        "  end Use;\n",
        holderMain("    h.Use();\n")));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kDisposeThroughBorrow));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "§28.6"));
}

// The CLASS-typed twin of the row above: §17.8 (v0.350) makes the `!`
// spelling transparent for EVERY type ("`x!` is therefore classified by its
// position exactly as the bare read `x` is"), so the assignment spelling must
// record the alias source for a class-typed owned field too — the same way
// the declaration spelling already does (the `class_bang_read_dispose…` row
// above).
//
// MEASURED on the base and on the v1 tip: ACCEPTED, rc=0.
BEZEL_TEST(borrow_state_family_7963_7974,
           class_assignment_site_bang_read_dispose_through_borrow_rejected_spec_28_6) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        kClassNullableHdr,
        "  public method Use()\n"
        "    var a := new Res();\n"
        "    a := this.R!;\n"
        "    a.Dispose();\n"
        "  end Use;\n",
        "    using var h := new H();\n    h.Use();\n"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kDisposeThroughBorrow));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "§28.6"));
}

// CONTROL — the CLASS-typed twin of the §28.3.3 slot stays ACCEPTED: the
// general rule for every type is issue #4846's (a new reject rule over the
// whole corpus, with its own blast-radius measurement) and is deliberately
// NOT taken here. The row pins that the enumerator family's arm did not
// widen into the class family.
//
// MEASURED on the base and on the v1 tip: ACCEPTED (one §28.3 auto-drop
// WARNING, no error).
BEZEL_TEST(borrow_state_family_7963_7974,
           class_borrow_into_owned_local_control_accepted_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class G "
        "  public static method Make(): owned Res "
        "    return new Res(); "
        "  end Make; "
        "end G; "
        "class H "
        "  public owned R: Res := new Res(); "
        "  public method Use() "
        "    var a := G.Make(); "
        "    a := this.R; "
        "  end Use; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    using var h := new H(); "
        "    h.Use(); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL — a NON-ENUMERATOR declaration-less builtin head (`IList<R>`) stays
// ACCEPTED: the scope is the FOUR ITERATOR-FORMAT ENUMERATOR heads
// (`common::isIterFormatEnumeratorHead`), not "every declaration-less builtin
// head".
//
// MEASURED on the base and on the v1 tip: ACCEPTED.
BEZEL_TEST(borrow_state_family_7963_7974,
           non_enumerator_head_borrow_into_owned_local_control_accepted_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class R public constructor() end constructor; end R; "
        "class G "
        "  public static method Lst(): IList<R> "
        "    var a: Array<R> := [new R(), new R()]; "
        "    return a; "
        "  end Lst; "
        "end G; "
        "class H "
        "  public owned L: IList<R>? := null; "
        "  public method Fill() L := G.Lst(); end Fill; "
        "  public method Use() "
        "    var a := G.Lst(); "
        "    a := this.L!; "
        "  end Use; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    using var h := new H(); "
        "    h.Fill(); "
        "    h.Use(); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL — an ARM MEET whose arms are BOTH owned producers stays ACCEPTED:
// the arm-meet shape is only a BORROW when one of its arms is an
// owned-field read. The row pins that the new arm did not widen into "any
// arm-meet RHS".
//
// MEASURED on the base and on the v1 tip: ACCEPTED.
BEZEL_TEST(borrow_state_family_7963_7974,
           owned_arm_meet_into_owned_enumerator_local_control_accepted_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        kEnumHdr,
        "  public method Use(i: int)\n"
        "    var a := G.Gen().GetEnumerator();\n"
        "    a := (if i mod 2 = 0 then G.It() else G.It());\n"
        "  end Use;\n",
        holderMain("    h.Use(1);\n")));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL — a BORROW into a BORROWED-state enumerator local stays ACCEPTED:
// §28.3's borrowed-state bullet admits a borrow RHS ("a borrowed-state LHS
// requires a borrow (or `null`) RHS"), and §28.3.3's FIRST half is about
// the owned-state slots only.
//
// MEASURED on the base and on the v1 tip: ACCEPTED.
BEZEL_TEST(borrow_state_family_7963_7974,
           borrow_into_borrowed_enumerator_local_control_accepted_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        kEnumHdr,
        "  public method Use()\n"
        "    var a := this.En!;\n"
        "    a := this.En!;\n"
        "  end Use;\n",
        holderMain("    h.Use();\n")));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The end-to-end shape for the ARM-MEET RHS — the strict oracle
// (`failedAtCompileTimeWithRealLocation`) distinguishes a genuine
// compile-time rejection from a crash, a hang or a toolchain break. #7963 (B).
BEZEL_TEST(borrow_state_family_7963_7974,
           arm_meet_borrow_store_rejected_by_the_driver_spec_28_3) {
    auto r = runAot(prog(
        kEnumHdr,
        "  public method Use(i: int)\n"
        "    var a := G.Gen().GetEnumerator();\n"
        "    a.Dispose();\n"
        "    a := (if i mod 2 = 0 then this.En! else G.Gen().GetEnumerator());\n"
        "  end Use;\n",
        holderMain("    h.Use(1);\n")));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kArmMeetBorrowStore));
    BEZEL_EXPECT_FALSE(r.ran);
}
