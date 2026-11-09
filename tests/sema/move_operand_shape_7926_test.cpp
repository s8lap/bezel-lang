// fix #7926 (spec §28.8.1 *The `move` expression*, Appendix A.7 `MoveExpr`,
// v0.350): at a BINDING POSITION — the whole RHS of a `var` / `using var`
// declaration — the `move` marker is written on a NAMED OWNED BINDING (a local
// or an `owned` parameter), and on nothing else.
//
//   (a) A CALL whose declared return type is `owned` is a FRESH PRODUCER.
//       §22.3: "A fresh producer ... stays unmarked: no live binding is
//       invalidated, so there is nothing to announce"; §28.3 K3.13: "Fresh
//       producers need no marker anywhere". So `var a := move Mk();` wrote a
//       marker on a value that needs none. Before the fix the program compiled
//       with only the §28.3 must-use auto-drop warning.
//
//   (b) A LITERAL is not a movable location at all. Appendix A.7's `MoveExpr`
//       admits exactly three operand shapes — an `owned` field of a class
//       instance, a slot of an owning collection, and a bare identifier
//       naming a local binding or `owned` parameter — and closes with "Any
//       other operand is a compile error". Before the fix `var b := move 5;`
//       compiled silently.
//
//   (c) A `new` EXPRESSION is the other FRESH PRODUCER. §28.8.1 names it
//       verbatim: "a fresh producer (a `new` expression, a call returning
//       `owned T`, an owned operator result) needs no marker anywhere (K3.13)
//       and takes none, so `move (new Cell(1))` is a compile error at every
//       position, an `owned` argument and an owned-slot store alike; the
//       remedy is to drop the `move`." This is the ISSUE'S OWN THIRD REPRO
//       (`var c := move (new Cell(1));`), and before the fix it compiled with
//       only the §28.3 must-use auto-drop warning.
//
//       THE PARENTHESISED SPELLING IS NOT A DIFFERENT SHAPE. Appendix A.7's
//       `MoveExpr` is `"move" PrimaryExpr`, and source-level parentheses
//       leave NO `ParenExpr` node in the AST (ast/Node.hpp: "source-level
//       parens leave no ParenExpr node — the tree shape encodes grouping —
//       so the parser stamps this flag on the inner expression of a `(...)`
//       group"): the group only groups the parse and stamps `parenthesized`
//       on the node inside it. MEASURED with the shipping parser (this lane's
//       AST-dump probe): `move (new Cell(1))` and `move new Cell(1)` are the
//       SAME `ast::NewExpr` operand, differing only in that flag. So the one
//       arm rejects both spellings, and rows R4 and R5 pin both.
//
// The rule is checked at the LOCAL DECLARATION INITIALIZER only; the other
// positions carry their own authority (§22.3's call-site marker economy at an
// `owned` argument; §28.3's marker-free positions at a `return` / `throw` /
// discard / owned-slot store). The CONTROL rows below pin the positions this
// lane does NOT touch, so a widening of the rule shows up as a failure here.
//
// Rows:
//   R1  `var a := move Mk();` — a fresh-producer call at a binding position
//   R2  `var b := move 5;` — a literal at a binding position
//   R3  `var b := move "s";` — a string literal at a binding position
//   R4  `var c := move (new Cell(1));` — the issue's own third repro: the
//       PARENTHESISED `new` at a binding position
//   R5  `var c := move new Cell(1);` — the BARE `new` at a binding position
//       (the same AST node as R4 — see the note above)
//   R6  CONTROL: `Take(move (new Cell(1)))` at an owned ARGUMENT — still
//       rejected, and still with §22.3's own CALL-SITE diagnostic (this
//       lane's binding-position rule does not take that position over)
//   C1  CONTROL: `arr.Add(move (new Item(3)))` on an `OwningArray<Item>` — the
//       owned-slot store. CURRENT BEHAVIOUR: it still compiles; §28.8.1
//       (v0.350) calls it "a compile error at every position, an `owned`
//       argument and an owned-slot store alike", so #8330 will flip this row
//       to a REJECT. It is pinned here as CURRENT BEHAVIOUR, not as a
//       spec-sanctioned acceptance.
//   C2  CONTROL: `return move C.Fresh();` through an `owned` return — the
//       `return` OPERAND position. FLIPPED by fix #8347 (v0.355): §28.8.1
//       makes the marker on a fresh producer "a compile error at every
//       position" and the operand rule is position-independent, so the
//       `return` operand refuses it too. Pinned as a REJECT by the same
//       fresh-producer diagnostic the binding position carries.
//   C3  CONTROL: `using var h := move c;` — a named owned local (A.7 (c))
//   C4  CONTROL: `move _f` — an `owned` field (A.7 (a))
//   C5  CONTROL: `move a[0]` — an owning collection's slot (A.7 (b))
//   C6  CONTROL: `Take(move c)` — a named owned binding at an owned argument
//   C7  CONTROL: `move (r with(X := 1))` — the record-update operand, still
//       rejected with ITS diagnostic (not this lane's)
//   C8  CONTROL: `move _f with new Cell()` — the replacing `move`, still
//       rejected with the #5923 diagnostic (not this lane's)

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// The stable substring of the #7926 fresh-producer diagnostic: §22.3's own
// wording, which the ownership pass's call-site twin carries too.
constexpr const char* kFreshProducer = "not written on a fresh producer";

// The stable substring of the #7926 fresh-producer diagnostic for the `new`
// shape. It names the SHAPE, so it distinguishes this lane's BINDING-position
// diagnostic ("... a fresh producer (a `new` expression) — ...") from §22.3's
// CALL-SITE one ("... a fresh producer (a `new` expression or a call returning
// an owned result) — ..."), which the closing parenthesis makes non-matching.
constexpr const char* kFreshProducerNew = "fresh producer (a `new` expression)";

// The stable substring that names the CALL operand shape of the same
// fresh-producer diagnostic — the shape `return move C.Fresh();` writes.
constexpr const char* kFreshProducerCall =
    "fresh producer (a call returning `owned T`)";

// The stable substring of the ownership pass's own CALL-SITE fresh-producer
// diagnostic (§22.3) — the position this lane does NOT touch.
constexpr const char* kCallSiteFreshProducer =
    "the `move` call-site marker is not written on a fresh producer";

// The stable substring of the #7926 literal-operand diagnostic.
constexpr const char* kLiteralOperand = "not admitted on a literal";

// A `Cell` with a destructor — a fresh producer of a class type, so the
// §28.3 must-use auto-drop warning fires on a plain `var` binding.
const char* kCell =
    "class Cell "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "end Cell; ";

}  // namespace

// R1 — the fresh-producer call at a binding position.
BEZEL_TEST(sema_move_operand_shape_7926,
           fresh_producer_call_is_not_admitted_at_a_binding_position) {
    auto r = runAot(
        "module M7926A; " + std::string(kCell) +
        "class Program "
        "  public static method Mk(): owned Cell "
        "    return new Cell(); "
        "  end Mk; "
        "  public static method Main() "
        "    var a := move Mk(); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreshProducer));
}

// R2 — the literal operand at a binding position.
BEZEL_TEST(sema_move_operand_shape_7926,
           literal_is_not_admitted_at_a_binding_position) {
    auto r = runAot(
        "module M7926B; "
        "class Program "
        "  public static method Main() "
        "    var b := move 5; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kLiteralOperand));
}

// R3 — a string literal is the same shape.
BEZEL_TEST(sema_move_operand_shape_7926,
           string_literal_is_not_admitted_at_a_binding_position) {
    auto r = runAot(
        "module M7926C; "
        "class Program "
        "  public static method Main() "
        "    var b := move \"s\"; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kLiteralOperand));
}

// R4 — THE ISSUE'S OWN THIRD REPRO, verbatim: the PARENTHESISED `new` at a
// binding position. §28.8.1 (v0.350): "a fresh producer (a `new` expression,
// ...) needs no marker anywhere (K3.13) and takes none, so `move (new
// Cell(1))` is a compile error at every position".
BEZEL_TEST(sema_move_operand_shape_7926,
           parenthesised_new_is_not_admitted_at_a_binding_position) {
    auto r = runAot(
        "module M7926R4; " + std::string(kCell) +
        "class Program "
        "  public static method Main(): int "
        "    var c := move (new Cell()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreshProducerNew));
}

// R5 — the BARE `new` spelling at a binding position. It is the SAME AST node
// as R4 (source-level parentheses leave no `ParenExpr` node — the parser
// stamps `parenthesized` on the node inside the group), so the one arm
// rejects both spellings; this row pins that the rule is about the SHAPE and
// not about the spelling.
BEZEL_TEST(sema_move_operand_shape_7926,
           bare_new_is_not_admitted_at_a_binding_position) {
    auto r = runAot(
        "module M7926R5; " + std::string(kCell) +
        "class Program "
        "  public static method Main(): int "
        "    var c := move new Cell(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreshProducerNew));
}

// R6 — CONTROL: the same shape at an owned ARGUMENT is still rejected, and
// still by the OWNERSHIP PASS's own §22.3 call-site diagnostic. This lane's
// rule is a BINDING-position rule; it must not take the call-site position
// over, and the two diagnostics must stay distinguishable.
BEZEL_TEST(sema_move_operand_shape_7926,
           control_owned_argument_keeps_the_call_site_diagnostic) {
    auto r = runAot(
        "module M7926R6; " + std::string(kCell) +
        "class Program "
        "  public static method Take(owned p: Cell) "
        "    using var h := move p; "
        "  end Take; "
        "  public static method Main(): int "
        "    Take(move (new Cell())); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kCallSiteFreshProducer));
}

// C1 — CONTROL: the owned-slot store. `arr.Add(move (new Item(3)))` on an
// `OwningArray<Item>` is a marker-free position.
//
// CURRENT BEHAVIOUR, NOT A SPEC-SANCTIONED ACCEPTANCE. §28.8.1 (v0.350)
// names this exact position: "so `move (new Cell(1))` is a compile error at
// every position, an `owned` argument and an owned-slot store alike". The
// program still compiles today; #8330 owns the owned-slot-store half and will
// flip this row to a REJECT. The row is pinned here so that THIS lane's
// change is shown not to have moved the position.
BEZEL_TEST(sema_move_operand_shape_7926,
           control_owned_slot_store_still_compiles) {
    auto r = runAot(
        "module M7926D; "
        "class Item "
        "  public constructor(v: int) end constructor; "
        "  destructor end destructor; "
        "end Item; "
        "class Program "
        "  public static method Main(): int "
        "    using var arr := new OwningArray<Item>(); "
        "    arr.Add(move (new Item(3))); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// C2 — CONTROL: `return move C.Fresh();` through an `owned` return.
//
// FLIPPED by fix #8347 (spec §28.8.1 / Appendix A.7 `MoveExpr` / §28.3 K3.13,
// v0.355). This row used to pin the ACCEPTANCE of the marker at the `return`
// OPERAND ("CURRENT BEHAVIOUR, NOT A SPEC-SANCTIONED ACCEPTANCE … #8330 owns
// that half and will flip this row"). §28.8.1 makes the marker on a fresh
// producer "a compile error at every position", and the operand rule is
// position-independent, so the `return` operand refuses it too; the remedy is
// to drop the `move`. The diagnostic is the OPERAND-SHAPE wording, byte-
// identical to narrow #7926's binding-position diagnostic — the one rule
// speaks with one voice at every position where it fires.
BEZEL_TEST(sema_move_operand_shape_7926,
           control_owned_return_of_a_fresh_producer_is_refused) {
    auto r = runAot(
        "module M7926E; " + std::string(kCell) +
        "class C "
        "  public static method Fresh(): owned Cell return new Cell(); end Fresh; "
        "  public static method Take(): owned Cell return move C.Fresh(); end Take; "
        "end C; "
        "class Program "
        "  public static method Main(): int "
        "    using var a := C.Take(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreshProducer));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreshProducerCall));
}

// C3 — CONTROL: a named owned local (A.7 alternative (c)).
BEZEL_TEST(sema_move_operand_shape_7926,
           control_named_owned_local_is_still_admitted) {
    auto r = runAot(
        "module M7926F; " + std::string(kCell) +
        "class Program "
        "  public static method Main(): int "
        "    var c := new Cell(); "
        "    using var h := move c; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// C4 — CONTROL: an `owned` field (A.7 alternative (a)).
BEZEL_TEST(sema_move_operand_shape_7926,
           control_owned_field_is_still_admitted) {
    auto r = runAot(
        "module M7926G; " + std::string(kCell) +
        "class Holder "
        "  private owned _f: Cell := new Cell(); "
        "  public method Take(): owned Cell "
        "    var old := move _f; "
        "    _f := new Cell(); "
        "    return old; "
        "  end Take; "
        "end Holder; "
        "class Program "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    using var c := h.Take(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// C5 — CONTROL: an owning collection's slot (A.7 alternative (b)).
BEZEL_TEST(sema_move_operand_shape_7926,
           control_owning_slot_is_still_admitted) {
    auto r = runAot(
        "module M7926H; " + std::string(kCell) +
        "class Program "
        "  public static method Main(): int "
        "    using var a := new OwningArray<Cell>(); "
        "    a.Add(new Cell()); "
        "    using var x := move a[0]; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// C6 — CONTROL: a named owned binding at an owned-argument position.
BEZEL_TEST(sema_move_operand_shape_7926,
           control_named_owned_binding_at_an_owned_argument_is_still_admitted) {
    auto r = runAot(
        "module M7926I; " + std::string(kCell) +
        "class Program "
        "  public static method Take(owned p: Cell) "
        "    using var h := move p; "
        "  end Take; "
        "  public static method Main(): int "
        "    var c := new Cell(); "
        "    Take(move c); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// C7 — CONTROL: the record-update operand keeps ITS diagnostic (§9.5's
// tie-break; the #7847 rule) — this lane's operand rule does not take it
// over.
BEZEL_TEST(sema_move_operand_shape_7926,
           control_record_update_operand_keeps_its_own_diagnostic) {
    auto r = runAot(
        "module M7926J; "
        "record Pt(X: int, Y: int); "
        "class Program "
        "  public static method Main(): int "
        "    var r := new Pt(0, 2); "
        "    var t := move (r with(X := 1)); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "record-update"));
}

// C8 — CONTROL: the replacing `move` keeps the #5923 diagnostic — this lane
// does not touch it.
BEZEL_TEST(sema_move_operand_shape_7926,
           control_replacing_move_keeps_the_5923_diagnostic) {
    auto r = runAot(
        "module M7926K; " + std::string(kCell) +
        "class Holder "
        "  private owned _f: Cell := new Cell(); "
        "  public method Take(): owned Cell "
        "    return move _f with new Cell(); "
        "  end Take; "
        "end Holder; "
        "class Program "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    using var c := h.Take(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "5923"));
}
