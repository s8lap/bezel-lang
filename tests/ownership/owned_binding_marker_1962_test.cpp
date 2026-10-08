// Spec-derived tests for issue #1962 — the v0.180 `owned` binding-position
// transfer marker: `var y := owned x;` / `using var y := owned x;`.
//
// §28.3 (v0.180): "Reading an owned local `x` in a position that requires an
// owned value ... transfers ownership out of `x`. ... The transfer is spelled
// where it is not already announced by the target ... in local binding and
// assignment positions with the same marker — `var y := owned x;`, `y := owned
// x;`, `using var y := owned x;` (§28.10 adoption)." §28.10 (v0.180): "Adopting
// a named owned binding. `using var y := owned x;` transfers a named owned local
// or `owned` parameter into a scope-bound binding: `y` takes over the disposal
// obligation and `x` is left moved-from ... An UNMARKED `using var y := x;` ...
// does not adopt: it is a borrow." This exact form appears in the §30.15
// ProcessAll example (`using var sender := owned tx;`), §30.7, and §30.14.
//
// Before this fix the marker did not PARSE in the binding-initializer position
// ("expected expression" at `owned`), though the call-site form `M(owned x)`
// (§22.3) already did — blocking the spec's canonical adopt-the-owned idiom.
// These tests pin: (1) the marked binding parses + type-checks; (2) it MOVES the
// source (a later read of the moved-from binding is rejected); (3) the marker on
// a borrow / fresh producer is rejected; (4) an AOT + ASan/LSan value test that
// the transfer disposes the resource EXACTLY ONCE on the scope-end path.

#include <string>
#include <string_view>

#include "support/aot_support.hpp"       // runAotChecked — AOT + ASan/LSan oracle
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
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
// PARSE + ACCEPT: the marked binding forms parse and type-check.
// ---------------------------------------------------------------------------

// §28.10: `using var y := owned x;` adopts an `owned` parameter — the §30.15
// canonical form. Before the fix this failed to PARSE ("expected expression").
BEZEL_TEST(owned_binding_marker, using_var_adopts_owned_param_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; class T end T; "
        "class C public static method Take(owned x: T): int "
        "  using var y := owned x; "
        "  return 0; "
        "end Take; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3: `var y := owned x;` — the plain-`var` marked transfer. The new binding
// is an owned local (auto-dropped at scope end with the §28.3 must-use warning);
// no error.
BEZEL_TEST(owned_binding_marker, plain_var_marked_transfer_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; class T end T; "
        "class C public static method Take(owned x: T): int "
        "  var y := owned x; "
        "  return 0; "
        "end Take; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3: the marked transfer of an owned LOCAL (not just a parameter).
BEZEL_TEST(owned_binding_marker, using_var_adopts_owned_local_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; class T end T; "
        "class C public static method M(): int "
        "  var w := new T(); "
        "  using var y := owned w; "
        "  return 0; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// MOVE SEMANTICS: the marked transfer leaves the source moved-from (§28.3).
// ---------------------------------------------------------------------------

// §28.3: after `using var y := owned x;`, a later read of `x` is a
// use-after-move — the marker MOVED it, exactly as the call-site marker does.
BEZEL_TEST(owned_binding_marker, marked_transfer_leaves_source_moved_from) {
    auto r = parseAndCheckOwnership(
        "module M; class T end T; "
        "class C public static method Take(owned x: T): int "
        "  using var y := owned x; "
        "  var z := x; "                    // read of moved-from x — rejected
        "  return 0; "
        "end Take; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "was already moved out"));
    BEZEL_EXPECT_TRUE(errorSays(r, "§28.3"));
}

// §28.10 (v0.180): the UNMARKED form `var y := x;` does NOT move — it borrows,
// so `x` remains an owned binding that can subsequently be consumed. (This pins
// the marker's opt-in nature: unmarked stays a borrow.)
BEZEL_TEST(owned_binding_marker, unmarked_binding_borrows_source_still_movable) {
    auto r = parseAndCheckOwnership(
        "module M; class T end T; "
        "class C public static method Take(owned x: T): int "
        "  var y := x; "                     // unmarked: borrow, does not move x
        "  using var h := owned x; "          // x still owned → marked move OK
        "  return 0; "
        "end Take; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// REJECT: the marker on a non-owned-binding right-hand side (§28.3, v0.180).
// ---------------------------------------------------------------------------

// §28.3: the marker on a BORROW parameter is an error — a borrow is owned
// elsewhere and cannot be transferred.
BEZEL_TEST(owned_binding_marker, marker_on_borrow_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class T end T; "
        "class C public static method M(b: T): int "
        "  using var y := owned b; "
        "  return 0; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow"));
}

// §28.3: the marker on a FRESH PRODUCER (`new`) is an error — no live binding is
// invalidated, so there is nothing to announce (mirrors §22.3 at the call site).
BEZEL_TEST(owned_binding_marker, marker_on_fresh_producer_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class T end T; "
        "class C public static method M(): int "
        "  using var y := owned new T(); "
        "  return 0; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "fresh producer"));
}

// ---------------------------------------------------------------------------
// CODEGEN (AOT + ASan/LSan): the marked transfer disposes EXACTLY ONCE.
// A static live-instance counter; Main returns the residual live count, which is
// 0 iff the single Cell was disposed exactly once. runAotChecked additionally
// fails on any leak / double-free the counter alone would not catch.
// ---------------------------------------------------------------------------

// §28.10: `using var y := owned x;` adopts the owned parameter and disposes it at
// scope end — exactly once. Two transfers (local→param at the call site with the
// §22.3 marker, param→using-var with the §28.10 marker), one Cell, one
// destructor call → Cell.live == 0.
BEZEL_TEST(owned_binding_marker, using_var_adopt_disposes_once_value_28_10) {
    std::string src =
        "module M; "
        "class Cell "
        "  public static live: int := 0; "
        "  public constructor() Cell.live := Cell.live + 1; end constructor; "
        "  destructor Cell.live := Cell.live - 1; end destructor; "
        "end Cell; "
        "class C "
        "  public static method Take(owned x: Cell): int "
        "    using var y := owned x; "        // §28.10 adoption: disposed at scope end
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

// §28.3: the EXCEPTION edge. A plain `var y := owned x;` that is DISCHARGED on
// the normal path (`return y;` through an `owned` return) is not affine
// auto-drop, so its unwind disposal comes from registerUnwindOwnedLocal /
// unwindOwnedLocalClass — which (before this fix) returned null for the bare-id
// owned-marker initializer, leaking `y` on a throw between the binding and the
// return. Boom() throws before the return; the frame must still dispose `y`
// exactly once on the unwind edge → Cell.live == 0 (ASan/LSan catches the leak).
BEZEL_TEST(owned_binding_marker, marked_transfer_disposes_on_unwind_edge_28_3) {
    std::string src =
        "module M; "
        "class AppError: Exception end AppError; "
        "class Cell "
        "  public static live: int := 0; "
        "  public constructor() Cell.live := Cell.live + 1; end constructor; "
        "  destructor Cell.live := Cell.live - 1; end destructor; "
        "end Cell; "
        "class C "
        "  public static method Boom(): int "
        "    throw new AppError(); "
        "    return 0; "
        "  end Boom; "
        "  public static method Inner(owned x: Cell): owned Cell "
        "    var y := owned x; "              // marked transfer; y is the sole owner
        "    var t := C.Boom(); "             // throws before the normal-path discharge
        "    return y; "
        "  end Inner; "
        "  public static method Main(): int "
        "    var w := new Cell(); "
        "    try "
        "      using var got := C.Inner(move w); "
        "    catch AppError => _ := 1; "
        "    end; "
        "    return Cell.live; "               // 0 — disposed exactly once on unwind
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.3: the plain-`var` marked transfer `var y := owned x;` likewise transfers
// the sole ownership to `y`, which the §28.3 affine auto-drop disposes at scope
// end — exactly once (ASan/LSan-clean).
BEZEL_TEST(owned_binding_marker, plain_var_marked_transfer_disposes_once_28_3) {
    std::string src =
        "module M; "
        "class Cell "
        "  public static live: int := 0; "
        "  public constructor() Cell.live := Cell.live + 1; end constructor; "
        "  destructor Cell.live := Cell.live - 1; end destructor; "
        "end Cell; "
        "class C "
        "  public static method Take(owned x: Cell): int "
        "    var y := owned x; "              // §28.3 marked transfer (auto-drop)
        "    return 7; "
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

// ---------------------------------------------------------------------------
// CODEGEN — INTERFACE-typed adoption (unannotated binding). classOfLocal
// recovers only the interface symbol (no static disposer), so the NORMAL-path
// disposal must come from the dynamic-dispatch classifier
// (usingVarOwnsClassObjectDynamically). A bare-id owned-marker initializer with
// an INFERRED interface type matched none of that classifier's branches, so the
// adopted value LEAKED on the normal return (fail-without: 16-byte LSan leak).
// These pin dispose-exactly-once on BOTH paths for the interface source.
// ---------------------------------------------------------------------------

// §28.10: `using var y := owned x;` where the `owned` source is INTERFACE-typed
// and `y` is unannotated (inferred). The dynamic dispose disposes the concrete
// instance exactly once at scope end → Cell.live == 0 (ASan/LSan-clean).
BEZEL_TEST(owned_binding_marker, using_var_adopt_interface_disposes_once_28_10) {
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
        "    using var y := owned x; "         // interface source, inferred slot
        "    var p := y.Ping(); "
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

// §28.3: the plain-`var` interface-typed marked transfer. The §28.3 affine
// auto-drop disposes the concrete instance dynamically at scope end — this is
// the specific NORMAL-path leak the reviewer found (16 bytes); fail-without:
// LSan leak, pass-with: clean.
BEZEL_TEST(owned_binding_marker, plain_var_adopt_interface_disposes_once_28_3) {
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
        "    var y := owned x; "               // interface source, auto-drop
        "    var p := y.Ping(); "
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

// §28.3: the interface-typed marked transfer disposes exactly once on the
// EXCEPTION edge too (Boom() throws before the normal return) — the
// unwindOwnedLocalClass mirror covers the interface case via isClassOrInterfaceRef.
BEZEL_TEST(owned_binding_marker, plain_var_adopt_interface_disposes_on_unwind_28_3) {
    std::string src =
        "module M; "
        "class AppError: Exception end AppError; "
        "interface IThing method Ping(): int; end IThing; "
        "class Cell: IThing "
        "  public static live: int := 0; "
        "  public constructor() Cell.live := Cell.live + 1; end constructor; "
        "  public method Ping(): int return 1; end Ping; "
        "  destructor Cell.live := Cell.live - 1; end destructor; "
        "end Cell; "
        "class C "
        "  public static method Boom(): int throw new AppError(); return 0; end Boom; "
        "  public static method Inner(owned x: IThing): int "
        "    var y := owned x; "
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
