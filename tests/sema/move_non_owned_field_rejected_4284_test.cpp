// Spec-derived regression tests for issue #4284 — `move <field>` of a non-`owned`
// (borrow-bearing) field is silently ACCEPTED on base, with a measured
// heap-use-after-free when the moved-from owner is later disposed of the same
// backing storage. The fix (ExprTypecheck_Exprs.cpp, typeUnary's `move` branch)
// rejects it at compile time: a class-typed field with no `owned` annotation is
// a borrow per §28.3's "unmarked read never consumes" rule, so there is no slot
// to transfer ownership of.
//
// §28.3 / §28.8.1: `move` transfers an OWNING slot. The four enumerated operand
// shapes are `move x` (a named local or `owned` parameter — §28.3), `move
// obj.f` (an `owned` field — §28.8.1), `move coll[i]` (an owning slot — §28.11),
// and `move obj.f!` over a nullable owned field (the §28.8.1 detach pattern).
// A non-`owned` class-typed field (`h.item` where `item: Res` carries no `owned`
// annotation) is BORROW storage, not owning storage, so the only legal read is
// the unmarked one — `var y := h.item;` already aliases the original owner's
// object, and `move h.item` would alias it AGAIN under a fresh owner that
// discharges, double-freeing the slot.
//
// Fail-without (base 93e5934, before fix): every REJECT case below compiled
// silently with no diagnostic, and the runtime case aborted under ASan with a
// heap-use-after-free inside `bz$M.C$OneIteration`. Pass-with (post-fix): every
// REJECT case is a compile error carrying the stable substring
// `moveOnNonOwnedFieldBorrow` (the test pins the diagnostic substring rather
// than the full text, so a future message refinement does not invalidate the
// test); the ACCEPT cases compile and dispose exactly once (Cell.live == 0).

#include <string>
#include <string_view>

#include "support/aot_support.hpp"       // runAotChecked — AOT + ASan oracle
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

// Stable substring for the #4284 diagnostic — pinned on the substring
// "non-`owned` field" so future wording refinements do not invalidate the
// regression. The full text cites §28.3 / §28.8.1 and names the field as a
// borrow of the receiver's storage.
constexpr std::string_view kMoveNonOwnedField = "non-`owned` field";

// A destructor-counter Cell: ctor ++, dtor --. A correct program leaves live==0;
// a double-free drives it negative or aborts under ASan.
const char* kCell =
    "module M; "
    "class Cell "
    "  public static live: int := 0; "
    "  public constructor() Cell.live := Cell.live + 1; end constructor; "
    "  destructor Cell.live := Cell.live - 1; end destructor; "
    "end Cell; ";

// A Box holding a NON-`owned` Cell field `n` — exactly the borrow-bearing shape
// the issue's `Holder.item: Res` is an instance of. `n` carries NO `owned`
// modifier, so it is a borrow (§28.3). Constructor takes the Cell as a plain
// (borrowed) parameter, mirroring `Holder(r: Res)` from the issue.
const char* kBox =
    "class Box "
    "  public n: Cell; "
    "  public constructor(c: Cell) this.n := c; end constructor; "
    "  destructor end destructor; ";

// A Box holding an `owned` Cell field `o` — the legal owned-field shape.
// `move` through `o` is the owned-field transfer (§28.8.1), NOT the #4284 rule.
const char* kOwnedBox =
    "class OwnedBox "
    "  public owned o: Cell? := null; "
    "  public constructor() this.o := new Cell(); end constructor; "
    "  destructor end destructor; ";

}  // namespace

// ===========================================================================
// REJECT — `move <field>` on a non-`owned` reference field is rejected at
// compile time. Each shape below was a SILENT ACCEPT on base, dispatching into
// codegen's reclaim-eligibility walk as a fresh owned transfer and double-
// freeing the field's backing storage at runtime.
// ===========================================================================

// §28.3 / §28.8.1: `move h.n` where `n` is a NON-`owned` class-typed field —
// the issue's exact `move h.item` repro shape, generalized to a Cell counter so
// the exact-once oracle pins the runtime UAF pre-fix and the dispose-once is
// the only path post-fix (compile error blocks it).
BEZEL_TEST(move_non_owned_field_rejected_4284,
           move_non_owned_field_disposes_exactly_once_spec_28_3) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) + kBox + "end Box; " +
        "class C "
        "  public static method Use() "
        "    using var box := new Box(); "
        "    var y := move box.n; "
        "  end Use; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kMoveNonOwnedField));
}

// §28.3: `move h.n!` — the bang-detach form over a non-`owned` field. The
// null-detach spelling is exclusively for `owned` nullable fields (§28.8.1);
// a non-`owned` field is borrow storage regardless of nullability — same
// rejection as the bare form.
BEZEL_TEST(move_non_owned_field_rejected_4284,
           move_non_owned_nullable_field_bang_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class Box "
        "  public n: Cell? := null; "
        "  public constructor() this.n := new Cell(); end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Use() "
        "    using var box := new Box(); "
        "    var y := move box.n!; "
        "  end Use; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kMoveNonOwnedField));
}

// §28.3 / §28.8.1: the BARE-IDENTIFIER `move _n` form (implicit `this.n`).
// Same rejection — `this` owns its instance fields only when the field carries
// the `owned` modifier; an unmarked field is a borrow even on `this`.
BEZEL_TEST(move_non_owned_field_rejected_4284,
           move_bare_non_owned_this_field_rejected_spec_28_8_1) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class Box "
        "  public n: Cell; "
        "  public constructor() this.n := new Cell(); end constructor; "
        "  public method Take() var y := move n; end Take; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Use() using var box := new Box(); box.Take(); end Use; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kMoveNonOwnedField));
}

// §28.3: a nested-field shape `move box.inner.n` where the OUTER `inner` is a
// non-`owned` reference field — same rejection. (The receiver `box.inner` is
// itself a borrow, and the field `n` of that borrow is a borrow of a borrow —
// still no owning slot to transfer.)
BEZEL_TEST(move_non_owned_field_rejected_4284,
           move_nested_non_owned_field_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class Inner "
        "  public n: Cell; "
        "  public constructor() this.n := new Cell(); end constructor; "
        "  destructor end destructor; "
        "end Inner; "
        "class Outer "
        "  public inner: Inner; "
        "  public constructor() this.inner := new Inner(); end constructor; "
        "  destructor end destructor; "
        "end Outer; "
        "class C "
        "  public static method Use(o: Outer) "
        "    var y := move o.inner.n; "
        "  end Use; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kMoveNonOwnedField));
}

// ===========================================================================
// RUNTIME UAF — exactly the issue's repro shape, with a destructor counter to
// make the exact-once oracle observable. Pre-fix (base 93e5934): the program
// compiled silently and aborted under ASan with a heap-use-after-free. Post-
// fix: the program is REJECTED at compile time, so the runtime path is
// unreachable — assert the compile error rather than the runtime exit code.
// ===========================================================================

// §28.3: the issue's exact `move h.item` repro, scaled to a 300-iteration
// loop (per `lsan-one-shot-leak-tests-pass-vacuously.md`'s amplification note
// for leak-style defects — a one-shot dispose may pass vacuously, but a
// double-free inside a loop reliably aborts under ASan on iteration 2+).
BEZEL_TEST(move_non_owned_field_rejected_4284,
           runtime_uaf_repro_rejected_at_compile_time_spec_28_3) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) + kBox + "end Box; " +
        "class C "
        "  public static method OneIteration(): int "
        "    var x := new Cell(); "
        "    var h := new Box(x); "
        "    var y := move h.n; "
        "    y.Dispose(); "
        "    x.Dispose(); "
        "    return 1; "
        "  end OneIteration; "
        "  public static method Main(): int "
        "    var i := 0; var acc := 0; "
        "    while i < 300 do acc := acc + C.OneIteration(); i := i + 1; end; "
        "    return acc - 300; "
        "  end Main; "
        "end C;");
    // Post-fix: compile error wins, the runtime path is unreachable.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kMoveNonOwnedField));
}

// ===========================================================================
// ACCEPT — the fix must NOT over-reject. Each case below compiles and runs
// the exact-once dispose under the AOT+ASan oracle (Cell.live == 0).
// ===========================================================================

// §28.8.1: `move h.o` where `o` is an `owned` field — the LEGAL owned-field
// transfer. NOT a #4284 case (the field carries `owned`); guards against the
// fix mis-classifying an `owned` field as a borrow. Uses the take-and-return
// idiom (`return move box.o!`) — directly disposes the field's null slot
// afterwards via OwnedBox's destructor skipping a null owned field.
BEZEL_TEST(move_non_owned_field_rejected_4284,
           move_owned_field_legal_disposes_once_spec_28_8_1) {
    std::string src =
        std::string(kCell) + kOwnedBox + "end OwnedBox; " +
        "class C "
        "  public static method Make(): owned Cell "
        "    using var box := new OwnedBox(); "
        "    return move box.o!; "
        "  end Make; "
        "  public static method Main(): int "
        "    do using var c := C.Make(); end; "
        "    return Cell.live; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.3: `move box.n` of a VALUE field (`int`) — no borrow/own distinction
// for value types (§3: value types carry no storage obligation; reads copy),
// so the rejection must NOT fire. The value is read through and the program
// returns the field value (42). Guards the fix's value-typed scoping.
BEZEL_TEST(move_non_owned_field_rejected_4284,
           move_value_field_unchanged_spec_28_3) {
    std::string src =
        "module M; "
        "class Box "
        "  public n: int := 42; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Use(): int "
        "    using var box := new Box(); "
        "    var q: int := move box.n; "
        "    return q; "
        "  end Use; "
        "  public static method Main(): int return C.Use(); end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// §28.3: a plain (non-`move`) read of a non-`owned` class field — the
// canonical use site. The unmarked read stays a borrow; the program compiles
// and the field aliases the source object as expected. (Box wraps a Cell in a
// non-`owned` field; Box owns the wrapping slot but not the Cell — Cell is
// discharged by its owning `using var x`.)
BEZEL_TEST(move_non_owned_field_rejected_4284,
           plain_read_non_owned_field_aliases_spec_28_3) {
    std::string src =
        std::string(kCell) + kBox + "end Box; " +
        "class C "
        "  public static method Use(): int "
        "    using var x := new Cell(); "
        "    using var h := new Box(x); "
        "    var y := h.n; "
        "    return 0; "
        "  end Use; "
        "  public static method Main(): int C.Use(); return Cell.live; end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
