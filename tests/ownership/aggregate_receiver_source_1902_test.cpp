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
//
// TEST-PROGRAM REPAIR (#6347, §5.1 v0.338). These rows used to consume the
// RECEIVER through `Util.Sink(owned c: C)` — passing `arr: Array<int>` to a
// parameter of the unrelated user class `C`. That spelling is not a legal
// Bezel program: §5.1's list of implicit reference conversions is closed, and
// `C` is neither a base of `Array<int>` nor wider than it. Before #6347 the
// compiler accepted it anyway (the miscompile #6347 fixes — the value is the
// same reference re-typed), so the rows ran; rows (a) and (b) happened to assert
// only that SOME error appears and stayed green, but row (c), the ACCEPT
// over-reject guard, asserts NO error and went RED on #6347's correct §5.1
// rejection. The receiver is now consumed through `Util.Drop(owned a:
// Array<int>)`, which is well-typed, so every row's verdict is once again the
// §28.3 S13 freeze's alone.

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

// Common prelude: a class C with Poke, an array element type, a Sink that
// consumes an owned C (the SEED side), and a Drop that consumes an owned
// Array<int> (the RECEIVER side). The two sinks are separate members because
// the receiver is a `Array<int>` and the seed is a `C` — §5.1 admits no
// implicit conversion between them, so one sink cannot serve both (#6347).
// The Aggregate is over Array<int> with a class accumulator C — the case
// where the §28.9 borrowable-result gate lets the Aggregate carve reach the
// receiver pushRootIfBorrowable call (#1902).
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
    "  public static method Drop(owned a: Array<int>): int a.Dispose(); "
    "return 0; "
    "  end Drop; "
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
        "_ := Util.Drop(move arr); "  // consume receiver while `m` live
        "var p := m.Poke(); "              // `m` (result) still live
        "return p; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot consume `arr` here"));
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow held by `m`"));
    // The program is otherwise well-typed — the receiver reaches a sink whose
    // parameter IS an `Array<int>`. Pinning the §5.1 needle's ABSENCE is what
    // keeps this row about the §28.3 S13 freeze alone: it is what #6347's
    // correct rejection of `Util.Sink(move arr)` broke, silently, while this
    // row still passed on its hasErrors() assertion.
    BEZEL_EXPECT_FALSE(errorSays(r, "not implicitly convertible"));
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
// last-use): `m` is no longer live at the Util.Drop site, so the freeze does
// not fire on either source. Same accept-after-last-use property the S13
// seed-side already had.
//
// This row is the one that reads the whole program as legal Bezel: its
// assertion is that NO error is produced at all. It therefore cannot be left
// resting on an ill-typed consume, which is why the receiver goes through
// `Util.Drop(owned a: Array<int>)` and not `Util.Sink(owned c: C)` — see the
// file header.
// ---------------------------------------------------------------------------

BEZEL_TEST(aggregate_receiver_source_1902,
           result_dead_before_receiver_consume_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(prog(
        "var arr := new Array<int>(); "
        "arr.Add(1); arr.Add(2); "
        "var seed := new C(); "
        "var m := arr.Aggregate(seed, lambda(acc: C, x: int) => acc); "
        "var p := m.Poke(); "              // last use of `m`
        "_ := Util.Drop(move arr); "  // consume receiver — `m` dead
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