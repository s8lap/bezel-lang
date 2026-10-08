// Spec-derived tests for issue #1965 — the v0.180 `owned` assignment-position
// transfer marker: `y := owned x;`. The follow-up to #1962 (which landed the
// binding forms `var y := owned x;` / `using var y := owned x;`).
//
// §28.3 (v0.180): "Reading an owned local `x` in a position that requires an
// owned value ... transfers ownership out of `x`. ... The transfer is spelled ...
// in local binding and assignment positions with the same marker — `var y :=
// owned x;`, `y := owned x;`, `using var y := owned x;`." The canonical spec
// example is the loop accumulator "keep the best, dispose the rest":
//   var best: Res? := null;
//   for var i in 0..<count do
//     var cur := new Res(i);
//     if ... then best := owned cur;   // transfer; displaced best disposed (§28.7)
//     else cur.Dispose(); end;
//   end;
// §28.3 (line ~5738, restated v0.180): reassignment is permitted only when the
// RHS's state matches the LHS's — an owned-state LHS requires an owned RHS (a
// fresh producer OR `x := owned y;`) and disposes the displaced value per
// drop-on-overwrite (§28.7); a polymorphic-null LHS (`var x: T? := null;`) adopts
// owned state from a marked transfer; a marked transfer into a borrowed-state LHS
// is rejected (the state-match rule).
//
// Before this fix the marker did not PARSE in the assignment position ("expected
// expression" at `owned`), though the binding form (#1962) and the call-site form
// `M(owned x)` (§22.3) already did. These tests pin: (1) the marked assignment
// parses + type-checks (poly-null and owned-init LHS); (2) it MOVES the source (a
// later read of the moved-from binding is rejected); (3) the marker on a borrow /
// fresh producer / borrowed-state LHS is rejected; (4) AOT + ASan/LSan value tests
// that the transfer disposes the moved-in value AND the displaced (drop-on-
// overwrite) value EXACTLY ONCE on the scope-end AND exception-unwind edges, for
// both class- and interface-typed sources.

#include <string>
#include <string_view>

#include "support/aot_support.hpp"       // runAotChecked — AOT + ASan/LSan oracle
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// True iff some error diagnostic's message contains `needle`.
bool errorSays(const OwnershipResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// PARSE + ACCEPT: the marked assignment parses and type-checks.
// ---------------------------------------------------------------------------

// §28.3: `y := owned x;` into a polymorphic-null LHS (`var y: T? := null;`) — the
// canonical loop-accumulator adopt. Before the fix this failed to PARSE.
BEZEL_TEST(owned_assign_marker, poly_null_lhs_marked_transfer_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; class T end T; "
        "class C public static method Take(owned x: T): int "
        "  var y: T? := null; "
        "  y := owned x; "
        "  return 0; "
        "end Take; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 / §28.7: `y := owned x;` into an OWNED-state LHS — the displaced owned
// value is disposed by drop-on-overwrite; no error (a §28.3 auto-drop warning on
// the final `y` value is expected, not an error).
BEZEL_TEST(owned_assign_marker, owned_lhs_marked_transfer_drops_on_overwrite_28_7) {
    auto r = parseAndCheckOwnership(
        "module M; class T end T; "
        "class C public static method Take(owned x: T): int "
        "  var y := new T(); "
        "  y := owned x; "
        "  y.Dispose(); "
        "  return 0; "
        "end Take; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// MOVE SEMANTICS: the marked transfer leaves the source moved-from (§28.3).
// ---------------------------------------------------------------------------

// §28.3: after `y := owned x;`, a later read of `x` is a use-after-move — the
// marker MOVED it, exactly as the call-site and binding markers do.
BEZEL_TEST(owned_assign_marker, marked_transfer_leaves_source_moved_from) {
    auto r = parseAndCheckOwnership(
        "module M; class T end T; "
        "class C public static method Take(owned x: T): int "
        "  var y: T? := null; "
        "  y := owned x; "
        "  var z := x; "                    // read of moved-from x — rejected
        "  return 0; "
        "end Take; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "was already moved out"));
    BEZEL_EXPECT_TRUE(errorSays(r, "§28.3"));
}

// §28.3 (v0.180): the UNMARKED form `y := x;` does NOT move — it borrows, so `x`
// remains an owned binding that can subsequently be consumed. (Pins the marker's
// opt-in nature: unmarked stays a borrow.)
BEZEL_TEST(owned_assign_marker, unmarked_assignment_borrows_source_still_movable) {
    auto r = parseAndCheckOwnership(
        "module M; class T end T; "
        "class C public static method Take(owned x: T): int "
        "  var y: T? := null; "
        "  y := x; "                         // unmarked: borrow, does not move x
        "  using var h := owned x; "          // x still owned → marked move OK
        "  return 0; "
        "end Take; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// REJECT: the marker on a non-owned-binding RHS / borrowed-state LHS (§28.3).
// ---------------------------------------------------------------------------

// §28.3: the marker on a BORROW parameter is an error — a borrow is owned
// elsewhere and cannot be transferred.
BEZEL_TEST(owned_assign_marker, marker_on_borrow_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class T end T; "
        "class C public static method M(b: T): int "
        "  var y: T? := null; "
        "  y := owned b; "
        "  return 0; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow"));
}

// §28.3: the marker on a FRESH PRODUCER (`new`) is an error — no live binding is
// invalidated, so there is nothing to announce (mirrors §22.3 / #1962).
BEZEL_TEST(owned_assign_marker, marker_on_fresh_producer_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class T end T; "
        "class C public static method M(): int "
        "  var y: T? := null; "
        "  y := owned new T(); "
        "  return 0; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "fresh producer"));
}

// §28.3 (line ~5738, state-match): a marked transfer into a BORROWED-state LHS is
// rejected — an owned value handed to a borrow slot would drop or double-hold the
// obligation.
BEZEL_TEST(owned_assign_marker, marked_transfer_into_borrowed_lhs_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class T end T; "
        "class C public static method M(owned x: T, src: T): int "
        "  var y := src; "                   // y: borrowed-state (aliases src)
        "  y := owned x; "                    // marked transfer into borrow — reject
        "  return 0; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "state-match"));
}

// ---------------------------------------------------------------------------
// CODEGEN (AOT + ASan/LSan): the marked transfer + drop-on-overwrite dispose
// EXACTLY ONCE. A static live-instance counter; Main returns the residual live
// count, which is 0 iff every Cell was disposed exactly once. runAotChecked
// additionally fails on any leak / double-free the counter alone would not catch.
// ---------------------------------------------------------------------------

// §28.3: `y := owned x;` into a polymorphic-null LHS adopts the owned parameter
// and disposes it at scope end — exactly once. One Cell, one destructor call.
BEZEL_TEST(owned_assign_marker, poly_null_adopt_disposes_once_value_28_3) {
    std::string src =
        "module M; "
        "class Cell "
        "  public static live: int := 0; "
        "  public constructor() Cell.live := Cell.live + 1; end constructor; "
        "  destructor Cell.live := Cell.live - 1; end destructor; "
        "end Cell; "
        "class C "
        "  public static method Take(owned x: Cell): int "
        "    var y: Cell? := null; "
        "    y := owned x; "                  // adopt: disposed once at scope end
        "    return 7; "
        "  end Take; "
        "  public static method Main(): int "
        "    var w := new Cell(); "
        "    var r := C.Take(move w); "       // §22.3 call-site transfer
        "    return Cell.live; "               // 0 — disposed exactly once
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.7: the OWNED-state LHS drop-on-overwrite. `y` first owns a fresh Cell, then
// `y := owned x;` DISPOSES that displaced Cell and adopts x's Cell. The displaced
// value must be disposed exactly once (fail-without: leak) and the moved-in value
// exactly once (fail-without: double-free). Two Cells, both disposed once → 0.
BEZEL_TEST(owned_assign_marker, owned_lhs_drop_on_overwrite_disposes_once_28_7) {
    std::string src =
        "module M; "
        "class Cell "
        "  public static live: int := 0; "
        "  public constructor() Cell.live := Cell.live + 1; end constructor; "
        "  destructor Cell.live := Cell.live - 1; end destructor; "
        "end Cell; "
        "class C "
        "  public static method Take(owned x: Cell): int "
        "    var y := new Cell(); "           // y owns Cell #1
        "    y := owned x; "                   // drop Cell #1 (§28.7); adopt x's Cell #2
        "    y.Dispose(); "                    // dispose Cell #2
        "    return 0; "
        "  end Take; "
        "  public static method Main(): int "
        "    var w := new Cell(); "            // Cell #2
        "    var r := C.Take(move w); "
        "    return Cell.live; "               // 0 — both disposed exactly once
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.3 / §28.7: the canonical loop accumulator — "keep the best, dispose the
// rest". On each iteration `best := owned cur;` disposes the displaced prior best
// by drop-on-overwrite and adopts the new winner; the losing `cur` is disposed on
// the else path; the final best is disposed explicitly. Every Res disposed
// exactly once → 0 (fail-without: displaced-best leak or a double-free).
BEZEL_TEST(owned_assign_marker, loop_accumulator_disposes_all_once_28_7) {
    std::string src =
        "module M; "
        "class Res "
        "  public static live: int := 0; "
        "  public v: int; "
        "  public constructor(n: int) this.v := n; Res.live := Res.live + 1; end constructor; "
        "  public method Score(): int return this.v; end Score; "
        "  destructor Res.live := Res.live - 1; end destructor; "
        "end Res; "
        "class C "
        "  public static method Best(count: int): int "
        "    var best: Res? := null; "
        "    for var i in 0..<count do "
        "      var cur := new Res(i); "
        "      if best = null or cur.Score() > best!.Score() then "
        "        best := owned cur; "          // drop displaced best (§28.7); adopt cur
        "      else "
        "        cur.Dispose(); "
        "      end; "
        "    end; "
        "    var result := 0; "
        "    if best <> null then "
        "      result := best!.Score(); "
        "      best!.Dispose(); "
        "    end; "
        "    return result; "
        "  end Best; "
        "  public static method Main(): int "
        "    var r := C.Best(5); "
        "    return Res.live; "                // 0 — all disposed exactly once
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.3: the EXCEPTION-unwind edge. `y := owned x;` moves the sole ownership into
// `y`; Boom() throws before any discharge. The frame must dispose `y` exactly once
// on the unwind edge (the source `x` is moved-from → unwind-ineligible, so it is
// not also disposed) → Cell.live == 0 (ASan/LSan catches a leak or double-free).
BEZEL_TEST(owned_assign_marker, marked_transfer_disposes_on_unwind_edge_28_3) {
    std::string src =
        "module M; "
        "class AppError: Exception end AppError; "
        "class Cell "
        "  public static live: int := 0; "
        "  public constructor() Cell.live := Cell.live + 1; end constructor; "
        "  destructor Cell.live := Cell.live - 1; end destructor; "
        "end Cell; "
        "class C "
        "  public static method Boom(): int throw new AppError(); return 0; end Boom; "
        "  public static method Inner(owned x: Cell): int "
        "    var y: Cell? := null; "
        "    y := owned x; "                   // marked transfer; y is the sole owner
        "    var t := C.Boom(); "              // throws before any discharge
        "    return 0; "
        "  end Inner; "
        "  public static method Main(): int "
        "    var w := new Cell(); "
        "    try "
        "      var r := C.Inner(move w); "
        "    catch AppError => _ := 1; "
        "    end; "
        "    return Cell.live; "               // 0 — disposed exactly once on unwind
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.3: the INTERFACE-typed polymorphic-null adoption. `y: IThing? := null;
// y := owned x;` where the `owned` source is interface-typed — classOfLocal
// recovers only the interface symbol (no static disposer), so the drop / scope-end
// disposal must dispatch dynamically. The concrete Cell is disposed exactly once
// → Cell.live == 0 (fail-without would be a 16-byte LSan leak, as in #1962).
BEZEL_TEST(owned_assign_marker, poly_null_adopt_interface_disposes_once_28_3) {
    std::string src =
        "module M; "
        "interface IThing method Ping(): int; end IThing; "
        "class Cell: IThing "
        "  public static live: int := 0; "
        "  public constructor() Cell.live := Cell.live + 1; end constructor; "
        "  public method Ping(): int return 1; end Ping; "
        "  destructor Cell.live := Cell.live - 1; end destructor; "
        "end Cell; "
        "class C "
        "  public static method Take(owned x: IThing): int "
        "    var y: IThing? := null; "
        "    y := owned x; "                   // interface source, inferred slot
        "    var p := y!.Ping(); "
        "    return 0; "
        "  end Take; "
        "  public static method Main(): int "
        "    var w := new Cell(); "
        "    var r := C.Take(move w); "
        "    return Cell.live; "               // 0 — disposed exactly once
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
