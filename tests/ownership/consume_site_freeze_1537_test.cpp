// Spec-derived tests for the v0.180 consume-site freeze (issue #1537, closes
// #1465).
//
// §28.3 "The consume-site freeze (v0.180)": "Consuming an owned **binding** — a
// local or an `owned` parameter — while a **live local borrow of its current
// value** exists is a compile error. The consume sites are the owned-consuming
// positions ... an `owned` argument under the call-site marker (including
// `Domain.Spawn` arguments and channel `Send`), a marked binding or assignment
// transfer, a store into an `owned` field or owning-collection slot, a `return`
// under an `owned` return type, and **reassignment** of the binding itself
// (§28.7 — drop-on-overwrite is a consume of the displaced value)."
//
// §28.7 "Overwrite while borrowed (v0.180)": "Reassigning a plain-`var` owned
// **local** while a live local borrow of its current value exists is rejected by
// the consume-site freeze ... the implicit disposal would silently invalidate
// the borrow."  The UNBORROWED reassignment stays legal — §28.7 drop-on-
// overwrite — which is the #1465 fix (the compiler previously blanket-rejected
// owned-local reassignment with the misleading "would be dropped here without
// disposal").
//
// §30.14: a value moved across a domain boundary "loses access statically at the
// binding level ... the consume-site freeze rejects the transfer while any local
// borrow of that binding is live" — the cross-domain data race the freeze closes.
//
// Liveness is of the binding's CURRENT value: a borrow taken of a PRIOR value
// that has already been replaced does NOT freeze the replacement, and a borrow
// whose only uses precede the consume is not live (the transfer-then-reuse
// idiom is expressible by finishing with the borrow first).

#include <string>
#include <string_view>

#include "support/aot_support.hpp"       // runAotCheckedDisposal — AOT oracle
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasAutoDropWarning;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotCheckedDisposal;

namespace {

// A disposable `Res` with a readable method, plus a `Take(owned)` owned sink.
std::string mod(const std::string& body) {
    return std::string(
        "module M; "
        "class Res "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public method N(): int return _n; end N; "
        "  destructor _ := _n; end destructor; "
        "end Res; "
        "class C "
        "  public static method Take(owned r: Res) r.Dispose(); end Take; "
        "  public static method Main(): int ") +
        body +
        "    return 0; "
        "  end Main; "
        "end C;";
}

// True iff some error diagnostic contains `needle`.
bool errorSays(const bezel::test::OwnershipResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// (a) move / owned-argument transfer while a live borrow exists — REJECTED.
// ---------------------------------------------------------------------------

// §28.3: `var b := a; Take(owned a); ... b.N()` — `a` is consumed by the owned-
// argument transfer while `b` (a live local borrow of `a`'s current value) is
// still used afterward. Reading `b` after the transfer would dangle.
BEZEL_TEST(consume_site_freeze, move_while_borrowed_spec_28_3) {
    auto r = parseAndCheckOwnership(mod(
        "    var a := new Res(1); "
        "    var b := a; "
        "    Take(move a); "
        "    var x := b.N(); "
        "    _ := x; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot consume `a` here"));
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow held by `b`"));
}

// §28.3: the same freeze fires when the borrow is used inside a `$"..."` hole
// (the surviving use is an interpolation) — the probe shape p16.
BEZEL_TEST(consume_site_freeze, move_while_borrowed_interpolated_use_spec_28_3) {
    auto r = parseAndCheckOwnership(mod(
        "    var a := new Res(1); "
        "    var b := a; "
        "    Take(move a); "
        "    Console.WriteLine($\"b={b.N()}\"); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot consume `a` here"));
}

// ---------------------------------------------------------------------------
// (b) cross-domain `Domain.Spawn(entry, owned c)` with a live alias — REJECTED.
// This is the §30.14 data race: the pre-transfer borrow and the receiving
// domain would reach one mutable object concurrently.
// ---------------------------------------------------------------------------

BEZEL_TEST(consume_site_freeze, cross_domain_spawn_live_alias_spec_30_14) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "public sealed class Counter "
        "  public N: int := 0; "
        "  public constructor() end constructor; "
        "end Counter; "
        "public static class Work "
        "  public static method Bump(owned c: Counter) "
        "    using var mine := c; "
        "    mine.N := mine.N + 1; "
        "  end Bump; "
        "end Work; "
        "public static class Program "
        "  public static async method Main(): Task<int> "
        "    var c := new Counter(); "
        "    var alias := c; "
        "    using var worker := Domain.Spawn(Work.Bump, move c); "
        "    alias.N := alias.N + 1; "
        "    await worker; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot consume `c` here"));
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow held by `alias`"));
}

// ---------------------------------------------------------------------------
// (c) UNBORROWED reassignment — ACCEPTED as §28.7 drop-on-overwrite (#1465).
// ---------------------------------------------------------------------------

// §28.7: `var r := new Res(0); r := new Res(1);` — no live borrow, so the
// overwrite disposes the displaced Res(0) and the final Res(1) auto-drops at
// scope end. It compiles with only the §28.3 must-use warning (a lint over a
// sound program), NOT the historical "would be dropped here without disposal"
// error (#1465).
BEZEL_TEST(consume_site_freeze,
           unborrowed_reassign_accepted_drop_on_overwrite_1465_spec_28_7) {
    auto r = parseAndCheckOwnership(mod(
        "    var r := new Res(0); "
        "    r := new Res(1); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "r"));
}

// The accepted program runs to completion under checked disposal — the displaced
// value and the final value are each disposed exactly once (no double-free, no
// leak, no use-after-dispose fault).
BEZEL_TEST(consume_site_freeze,
           unborrowed_reassign_runs_disposes_once_1465_spec_28_7) {
    auto r = runAotCheckedDisposal(mod(
        "    var r := new Res(0); "
        "    r := new Res(1); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// A borrow whose only use PRECEDES the consume is not live at the consume, so
// the transfer/reassignment after it is accepted (the transfer-then-reuse idiom
// with reuse omitted).
BEZEL_TEST(consume_site_freeze, transfer_after_dead_borrow_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(mod(
        "    var a := new Res(1); "
        "    var b := a; "
        "    var seen := b.N(); "
        "    _ := seen; "
        "    Take(move a); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// (d) closure-held borrow counts as LIVE — REJECTED.
// ---------------------------------------------------------------------------

// §28.3: a lambda capturing `s` keeps the borrow live across the consume when
// the closure value is itself still live (invoked after the transfer).
BEZEL_TEST(consume_site_freeze, closure_held_borrow_live_spec_28_3) {
    auto r = parseAndCheckOwnership(mod(
        "    var s := new Res(5); "
        "    var f := lambda() => s.N(); "
        "    Take(move s); "
        "    var y := f(); "
        "    _ := y; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot consume `s` here"));
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow held by `f`"));
}

// ---------------------------------------------------------------------------
// (e) reassignment WHILE borrowed — REJECTED (the §28.7 borrowed case); and a
// borrow of an already-replaced PRIOR value does NOT freeze the replacement.
// ---------------------------------------------------------------------------

// §28.7: `var b := a; a := new Res(2); ... b.N()` — the drop-on-overwrite would
// silently dispose the value `b` still reads. This is the probe b2, which the
// #1537 split rejects with the freeze diagnostic (previously it was rejected
// with the misleading undischarged-drop message; #1465).
BEZEL_TEST(consume_site_freeze, reassign_while_borrowed_spec_28_7) {
    auto r = parseAndCheckOwnership(mod(
        "    var a := new Res(1); "
        "    var b := a; "
        "    a := new Res(2); "
        "    var x := b.N(); "
        "    _ := x; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot consume `a` here"));
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow held by `b`"));
}

// §28.3 point 4: liveness is of the binding's CURRENT value. Here `b` borrows
// `a`'s value and is finished with (used only before the reassignment); the
// reassignment is therefore accepted, and the later `Take(owned a)` consumes the
// NEW value while `b` — a stale borrow of the replaced prior value — does not
// freeze it.
BEZEL_TEST(consume_site_freeze,
           borrow_of_prior_value_does_not_freeze_replacement_spec_28_3) {
    auto r = parseAndCheckOwnership(mod(
        "    var a := new Res(1); "
        "    var b := a; "
        "    var seen := b.N(); "
        "    _ := seen; "
        "    a := new Res(2); "
        "    Take(move a); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The stale-prior-value program runs cleanly under checked disposal: the
// displaced value is disposed at the overwrite and the replacement is disposed
// by `Take`, each exactly once, and `b`'s pre-reassignment read is valid.
BEZEL_TEST(consume_site_freeze,
           borrow_of_prior_value_runs_clean_spec_28_7) {
    auto r = runAotCheckedDisposal(mod(
        "    var a := new Res(1); "
        "    var b := a; "
        "    var seen := b.N(); "
        "    _ := seen; "
        "    a := new Res(2); "
        "    Take(move a); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
