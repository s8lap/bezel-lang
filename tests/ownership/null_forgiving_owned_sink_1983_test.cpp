// Spec-derived tests for issue #1983 — a postfix null-forgiving `E!` on a PLACE
// (a named local / parameter / field / index slot), un-`move`d, flowing into an
// OWNED SINK is a BORROW and must be rejected (it was silently accepted, causing
// a use-after-free / double-free).
//
// §17.4 (v0.184): a null-aware operator "neither transfers ownership out of `x`
// nor disposes `x`'s value"; its result is borrowed even when the operand is
// owned — "Code that needs the result to be owned must move from `x` explicitly."
// Stated for `as`, the principle applies to every null-aware operator (`!`, `??`,
// `?.`, `?(`). §28.8.1: `move E!` is the only way to read a place's value as an
// owned value. So a bare `E!` handed to an owned sink — an `owned` field store
// (§28.3/§28.5), an `owned` parameter / constructor argument (§28.8), an `owned`
// return, or a `throw` operand (§31.1) — is a double-dispose: the sink disposes
// the value AND the place's real owner disposes it too.
//
// Before this fix `E!` had no borrow classification in `ownedArgSourceIsBorrow`,
// and the owned-sink handlers apply move-on-read only to a bare identifier — so a
// place `E!` was neither moved nor rejected: it silently aliased the value into
// the sink, which double-freed it (ASan: heap-use-after-free in the sink; the
// value's destructor runs twice). Discovered while investigating #1975 (whose
// reported nullable-owned-field "leak" is a `using var` disposal-ordering
// measurement artifact, not a real leak — the field is disposed exactly once).
//
// These tests pin: (1) a place `E!` into an owned field / owned param / owned
// ctor-arg is REJECTED (fail-without: accepted → the runtime double-free);
// (2) the legal owned forms still work and dispose exactly once — `move E!`, a
// fresh producer `producer()!` / `new T()!`, and the bare `this.f := x`
// move-on-read (no `!` needed for a nullable-into-nullable field). v0.184.

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

// A destructor-counter Cell: ctor ++, dtor --. A correct program leaves live==0;
// a leak leaves it >0; a double-free/double-dispose drives it negative or aborts
// under ASan (heap-use-after-free) — the exact-once oracle.
const char* kCell =
    "module M; "
    "class Cell "
    "  public static live: int := 0; "
    "  public constructor() Cell.live := Cell.live + 1; end constructor; "
    "  destructor Cell.live := Cell.live - 1; end destructor; "
    "end Cell; ";

}  // namespace

// ---------------------------------------------------------------------------
// REJECT: a place `E!` into an owned SINK is a borrow (§17.4 / §28.8.1).
// Fail-without: the store/call is silently accepted and the value is disposed
// twice (a use-after-free at the owning object's / callee's disposal).
// ---------------------------------------------------------------------------

// §28.3 / §28.5 / §17.4: `this.f := x!` — force-unwrap of a nullable OWNED LOCAL
// stored into an owned field. `x!` borrows x's value; x still auto-drops it and
// the field disposes it too. Must be rejected (write `this.f := move x!` or, for
// a nullable field, the bare `this.f := x`).
BEZEL_TEST(null_forgiving_owned_sink_1983,
           local_bang_into_owned_field_rejected_spec_17_4) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell? := null; "
        "  public method Set() var x: Cell? := new Cell(); this.f := x!; end Set; "
        "end Box;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow cannot be stored into an `owned` field"));
}

// §28.3 / §28.8.1: `this.f := this.g!` — force-unwrap of a nullable OWNED FIELD
// read (a place) stored into another owned field. A bare owned-field read is a
// borrow (§28.8.1: only `move obj.f` is owned); the `!` does not change that.
// Must be rejected (write `this.f := move this.g`).
BEZEL_TEST(null_forgiving_owned_sink_1983,
           field_bang_into_owned_field_rejected_spec_28_8_1) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell? := null; "
        "  public owned g: Cell? := null; "
        "  public method Init(owned x: Cell) this.g := x; end Init; "
        "  public method Copy() this.f := this.g!; end Copy; "
        "end Box;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow cannot be stored into an `owned` field"));
}

// §28.8 / §17.4: `C.Sink(x!)` — a place `E!` passed to an `owned` PARAMETER. The
// owned sink takes disposal responsibility; the borrow's real owner (x) disposes
// it too. Must be rejected (write `C.Sink(move x!)` or pass a fresh producer).
BEZEL_TEST(null_forgiving_owned_sink_1983,
           local_bang_into_owned_param_rejected_spec_28_8) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class C "
        "  public static method Sink(owned c: Cell) using var k := c; end Sink; "
        "  public static method Use() var x: Cell? := new Cell(); C.Sink(x!); end Use; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        errorSays(r, "borrow cannot be passed where an `owned` value is required"));
}

// §28.8: `new Holder(x!)` — a place `E!` passed to an `owned` CONSTRUCTOR
// parameter (the owned-sink classification covers ctor args). Must be rejected.
BEZEL_TEST(null_forgiving_owned_sink_1983,
           local_bang_into_owned_ctor_arg_rejected_spec_28_8) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class Holder "
        "  public owned c: Cell; "
        "  public constructor(owned c: Cell) this.c := c; end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "class C "
        "  public static method Use() "
        "    var x: Cell? := new Cell(); using var h := new Holder(x!); "
        "  end Use; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        errorSays(r, "borrow cannot be passed where an `owned` value is required"));
}

// ---------------------------------------------------------------------------
// ACCEPT + dispose-exactly-once: the legal owned forms are NOT over-rejected.
// Fail-without (had the fix over-reached): compile error; with the fix: exit 0.
// ---------------------------------------------------------------------------

// §28.8.1: `this.f := move x!` — the EXPLICIT owned transfer. `move` reads the
// place as an owned value; x is left moved-from (no auto-drop), the field owns
// and disposes the value exactly once → Cell.live == 0.
BEZEL_TEST(null_forgiving_owned_sink_1983,
           move_bang_into_owned_field_disposes_once_28_8_1) {
    std::string src =
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell? := null; "
        "  public method Set() var x: Cell? := new Cell(); this.f := move x!; end Set; "
        "end Box; "
        "class C public static method Main(): int "
        "  do using var b := new Box(); b.Set(); end; "   // Box disposes f exactly once
        "  return Cell.live; "                             // 0
        "end Main; end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §17.4: `this.f := producer()!` — a FRESH PRODUCER unwrapped. The call result is
// the sole owner; the `!` on a temporary (not a place) carries that ownership
// through, so the field adopts it and disposes it exactly once → Cell.live == 0.
BEZEL_TEST(null_forgiving_owned_sink_1983,
           producer_bang_into_owned_field_disposes_once_17_4) {
    std::string src =
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell? := null; "
        "  public static method Mk(): owned Cell? return new Cell(); end Mk; "
        "  public method Set() this.f := Box.Mk()!; end Set; "
        "end Box; "
        "class C public static method Main(): int "
        "  do using var b := new Box(); b.Set(); end; "
        "  return Cell.live; "                             // 0
        "end Main; end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.8: `C.Sink(new Cell())` — a fresh producer into an owned param (no `!`, no
// place). Accepted; the sink disposes it exactly once → Cell.live == 0. Guards
// that the owned-arg `PostfixBangExpr` routing does not disturb the ordinary
// fresh-producer argument path.
BEZEL_TEST(null_forgiving_owned_sink_1983,
           producer_into_owned_param_disposes_once_28_8) {
    std::string src =
        std::string(kCell) +
        "class C "
        "  public static method Sink(owned c: Cell) using var k := c; end Sink; "
        "  public static method Use() C.Sink(new Cell()); end Use; "
        "  public static method Main(): int C.Use(); return Cell.live; end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.3: `this.f := x;` — the bare move-on-read (no `!`). A nullable owned local
// stored into a nullable owned field: the owned-field store IS the announcement
// (§28.3, v0.180), so x is moved into the field with no marker and disposed
// exactly once → Cell.live == 0. The `!`-free form the rejections above steer to.
BEZEL_TEST(null_forgiving_owned_sink_1983,
           bare_move_on_read_into_owned_field_disposes_once_28_3) {
    std::string src =
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell? := null; "
        "  public method Set() var x: Cell? := new Cell(); this.f := x; end Set; "
        "end Box; "
        "class C public static method Main(): int "
        "  do using var b := new Box(); b.Set(); end; "
        "  return Cell.live; "                             // 0
        "end Main; end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
