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

// §17.8 / §28.3 / §28.5 (v0.309, #5144, #4993): `this.f := x!` — force-unwrap
// of a nullable OWNED LOCAL stored into an owned field. §17.8: "Over a named
// owned binding, `x!` is therefore classified by its position exactly as the
// bare read `x` is. At a marker-free position of the consume table — for
// example a store into an `owned` field — `sink._f := x!;` consumes `x`
// exactly as `sink._f := x;` does." The owned-field store IS the announcement
// (§28.3 K3.1); `x!` consumes x and the field disposes it exactly once →
// Cell.live == 0. The pre-fix #1983 behaviour rejected this with "borrow
// cannot be stored" — the §17.8 consume at a marker-free position was
// mis-classified as a borrow. Fail-without (revert #5144): rc != 0 with the
// borrow-store diagnostic.
BEZEL_TEST(null_forgiving_owned_sink_1983,
           local_bang_into_owned_field_consumes_spec_17_8) {
    std::string src =
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell? := null; "
        "  public method Set() var x: Cell? := new Cell(); this.f := x!; end Set; "
        "end Box; "
        "class C public static method Main(): int "
        "  do using var b := new Box(); b.Set(); end; "
        "  return Cell.live; "
        "end Main; end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
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

// §17.8 / §22.3 / §28.8 (v0.309, #5144, #4993): `C.Sink(x!)` — a null-forgiving
// `E!` over a named OWNED BINDING passed to an `owned` PARAMETER. §17.8: "At a
// marked position — an `owned` argument, a local binding or assignment — the
// transfer is spelled `move x!`, the `!` being transparent to the marker."
// The argument position is a MARKED position (K3.3), so the user must write
// `C.Sink(move x!)`. The bare `C.Sink(x!)` is rejected with the marker-required
// diagnostic — steering the user to the legal spelling (NOT the §28.8 "borrow
// cannot be passed" diagnostic, which is for non-binding borrows like
// `C.Sink(this.f)`). // per §17.8, §22.3, §28.8
BEZEL_TEST(null_forgiving_owned_sink_1983,
           local_bang_into_owned_param_missing_marker_spec_22_3) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class C "
        "  public static method Sink(owned c: Cell) using var k := move c; end Sink; "
        "  public static method Use() var x: Cell? := new Cell(); C.Sink(x!); end Use; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "this call consumes `x!`"));
    BEZEL_EXPECT_TRUE(errorSays(r, "M(move x!)"));
}

// §17.8 / §22.3 / §28.8 (v0.309, #5144, #4993): `new Holder(x!)` — same as the
// param twin above at a CONSTRUCTOR ARGUMENT position (also a MARKED position
// per §17.8 / K3.3). Rejected with the marker-required diagnostic, NOT the
// §28.8 borrow diagnostic. // per §17.8, §22.3, §28.8
BEZEL_TEST(null_forgiving_owned_sink_1983,
           local_bang_into_owned_ctor_arg_missing_marker_spec_22_3) {
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
    BEZEL_EXPECT_TRUE(errorSays(r, "this call consumes `x!`"));
    BEZEL_EXPECT_TRUE(errorSays(r, "M(move x!)"));
}

// ---------------------------------------------------------------------------
// ACCEPT + dispose-exactly-once: the legal owned forms are NOT over-rejected.
// Fail-without (had the fix over-reached): compile error; with the fix: exit 0.
// ---------------------------------------------------------------------------

// §17.8 / §28.3 / §28.8.1 (v0.309, #5144, #4993): `this.f := move x!` is the
// REDUNDANT MARKER. §17.8: "`x!` is classified by its position exactly as the
// bare read `x` is. At a marker-free position of the consume table — for
// example a store into an `owned` field — `sink._f := x!;` consumes `x`
// exactly as `sink._f := x;` does." A `move` marker at a marker-free position
// (a store into an `owned` field, K3.1) is §28.3 K3.13's "redundant marker"
// — `move` is admitted only at the marker-required consuming positions
// (`owned` argument, local binding / assignment, K3.3/K3.4). The §28.3 K3.13
// / K3.1 redundant-marker diagnostic reports it. // per §17.8, §28.3 K3.13
BEZEL_TEST(null_forgiving_owned_sink_1983,
           move_bang_into_owned_field_rejected_redundant_marker_spec_28_3) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class Box "
        "  public owned f: Cell? := null; "
        "  public method Set() var x: Cell? := new Cell(); this.f := move x!; end Set; "
        "end Box;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "the `move` marker is redundant"));
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
        "  public static method Sink(owned c: Cell) using var k := move c; end Sink; "
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
