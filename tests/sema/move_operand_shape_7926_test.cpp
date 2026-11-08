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
//   C1  CONTROL: `arr.Add(move (new Item(3)))` on an `OwningArray<Item>` — the
//       owned-slot store, still COMPILING (the `move` of a fresh producer at a
//       marker-free position is not this lane's rule)
//   C2  CONTROL: `return move C.Fresh();` through an `owned` return — still
//       COMPILING
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

// C1 — CONTROL: the owned-slot store. `arr.Add(move (new Item(3)))` on an
// `OwningArray<Item>` is a marker-free position, and the `move` of a fresh
// producer there is NOT this lane's rule: it must still compile.
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
BEZEL_TEST(sema_move_operand_shape_7926,
           control_owned_return_of_a_fresh_producer_still_compiles) {
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
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
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
