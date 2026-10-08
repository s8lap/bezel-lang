// Spec-derived tests for issue #1902 — §28.3 S13 *Aggregate* carve-out
// under-tracks its receiver source for builtin-collection receivers.
//
// §28.3 v0.194 *Bare-type-parameter returns* (S13): "Aggregate<TAccum>(seed,
// fold): TAccum" — the result borrows {receiver, seed}. The caller-side
// source set must contain BOTH the receiver and the seed argument so that
// consuming either while the result is live is frozen by the consume-site
// rule.
//
// The #1801 implementation recognized the call via the §28.9 borrowable-result
// gate and pushed the SEED source position through the argument path
// (pushRootIfBorrowable, which gates on isBorrowableType of the argument). The
// RECEIVER was pushed through the same pushRootIfBorrowable — but the receiver
// of a LINQ sequence operator is a BUILTIN COLLECTION (`Array`/`OwningArray`/
// the lazy `IEnumerable<T>` sentinel) whose resolved TypeRef carries a NULL
// `declaration`. isBorrowableType returns false for a null-declaration Named
// ref (it treats such a ref as a bare type parameter), so the receiver root
// was DROPPED. The element-access branch (First/Min/Single/...) hit the same
// gap and was already fixed (#1876) by pushing the receiver root
// UNCONDITIONALLY; the Aggregate branch was missed.
//
// Consequence: `arr.Aggregate(seed, fold)` over a class `TAccum`
// under-tracked `arr` — consuming `arr` (a Sink(owned arr) transfer) while
// the result is live was NOT frozen, even though the returned borrow aliases
// the receiver's storage. Same UAF shape as the #1876 fix it parallels.
//
// These tests pin the receiver-source freeze for the Aggregate carve and a
// sibling freeze for the seed source (the existing behavior, regression
// guard), plus an over-reject control for a value-instantiation Aggregate
// (the §28.9 borrowable-result gate already rejects this case — no source
// attaches, no freeze, ACCEPT). // per §28.3 v0.194 S13

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// §28.3 v0.194 S13 consume-site-freeze diagnostic — names the consumed
// binding and the borrow holding it.
bool errorSays(const OwnershipResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// Common prelude: a class C with Poke, an array element type, and a Sink
// that consumes an owned C. The Aggregate is over Array<int> with a class
// accumulator C — the case where the §28.9 borrowable-result gate lets the
// Aggregate carve reach the receiver pushRootIfBorrowable call (#1902).
constexpr std::string_view kPrelude =
    "module M; "
    "class C "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "  public method Poke(): int return 1; end Poke; "
    "end C; "
    "class Util "
    "  public static method Sink(owned c: C): int c.Dispose(); return 0; "
    "  end Sink; "
    "end Util; ";

std::string prog(std::string_view body) {
    return std::string(kPrelude) +
           "class Prog "
           "  public static method Main(): int " +
           std::string(body) +
           "  end Main; "
           "end Prog;";
}

}  // namespace

// ---------------------------------------------------------------------------
// (a) THE KEYSTONE REPRO for #1902. Consuming the RECEIVER (`arr`) while the
// Aggregate result `m` (class-typed accumulator C) is live must be frozen —
// the returned borrow aliases `arr`'s storage. Before the fix this compiled
// clean: the receiver source was dropped by pushRootIfBorrowable (null-
// declaration Named ref on the builtin-collection receiver), so no
// constraint attached, and the freeze saw nothing to reject.
// ---------------------------------------------------------------------------

BEZEL_TEST(aggregate_receiver_source_1902,
           consume_receiver_while_aggregate_result_live_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(prog(
        "var arr := new Array<int>(); "
        "arr.Add(1); arr.Add(2); "
        "var seed := new C(); "
        "var m := arr.Aggregate(seed, lambda(acc: C, x: int) => acc); "
        "_ := Util.Sink(move arr); "  // consume receiver while `m` live
        "var p := m.Poke(); "              // `m` (result) still live
        "return p; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot consume `arr` here"));
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow held by `m`"));
}

// ---------------------------------------------------------------------------
// (b) REGRESSION GUARD for the SEED source — the existing behavior. Consuming
// the SEED (`seed`) while the Aggregate result is live is frozen. This was
// already covered (S13's seed-side) by the original #1801 implementation; the
// new fix must not regress it.
// ---------------------------------------------------------------------------

BEZEL_TEST(aggregate_receiver_source_1902,
           consume_seed_while_aggregate_result_live_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(prog(
        "var arr := new Array<int>(); "
        "arr.Add(1); arr.Add(2); "
        "var seed := new C(); "
        "var m := arr.Aggregate(seed, lambda(acc: C, x: int) => acc); "
        "_ := Util.Sink(move seed); "  // consume seed while `m` live
        "var p := m.Poke(); "                // `m` (result) still live
        "return p; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot consume `seed` here"));
}

// ---------------------------------------------------------------------------
// (c) OVER-REJECT GUARD — RESULT DEAD AT THE CONSUME. The same program with
// the result's last use BEFORE the consume must be accepted (accept-after-
// last-use): `m` is no longer live at the Util.Sink site, so the freeze does
// not fire on either source. Same accept-after-last-use property the S13
// seed-side already had.
// ---------------------------------------------------------------------------

BEZEL_TEST(aggregate_receiver_source_1902,
           result_dead_before_receiver_consume_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(prog(
        "var arr := new Array<int>(); "
        "arr.Add(1); arr.Add(2); "
        "var seed := new C(); "
        "var m := arr.Aggregate(seed, lambda(acc: C, x: int) => acc); "
        "var p := m.Poke(); "              // last use of `m`
        "_ := Util.Sink(move arr); "  // consume receiver — `m` dead
        "return p; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// (d) OVER-REJECT GUARD — value-typed accumulator. `arr.Aggregate(0, lambda
// (acc: int, x: int) => acc + x)` over a value-typed element type AND a
// value-typed accumulator attaches no borrow (the result is a copy, not a
// borrow) and is governed by the value-instantiation rule. The §28.9
// borrowable-result gate already drops this case before the carve runs —
// no source attaches, no freeze, ACCEPT.
// ---------------------------------------------------------------------------

BEZEL_TEST(aggregate_receiver_source_1902,
           value_typed_accumulator_attaches_no_source_accepted_spec_28_9) {
    auto r = parseAndCheckOwnership(prog(
        "var arr := new Array<int>(); "
        "arr.Add(1); arr.Add(2); "
        "var s := arr.Aggregate(0, lambda(acc: int, x: int) => acc + x); "
        "return s; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}