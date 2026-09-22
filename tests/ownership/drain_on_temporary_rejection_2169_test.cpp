// Ownership sema — #2169 / §28.3 v0.226 / stdlib §4.5 v0.195: a `Drain()`
// over a TEMPORARY receiver escapes the temporary's K8 full-expression
// boundary. The drain view aliases the receiver's array (§4.5 v0.195: "`Drain()`
// on `OwningArray<T>` returns the array itself at that static type — no
// separate enumerable object"; the alias's container obligation stays with
// the receiver's owner — the temporary — which dies at the binding's full-
// expression boundary). Per §28.3 "a borrow or frame that would escape [the
// temporary's boundary] is rejected — the language has no general lifetime-
// extension mechanism".
//
// The #2146 stable-named-receiver case is recognized as a §28.10 borrow-pin
// (the producer is Borrowed, no scope-end dispose registered); this is its
// temporary-receiver twin, currently REJECTED. The runtime counterpart
// (codegen's iterator-format dispose-slot-1 on what is actually a bz_array,
// ASan SEGV) lives in locked iterator-emission paths and is out of scope;
// the rejection closes the SEGV at compile time and steers the user to the
// idiom that DOES work today: bind the temporary to a name first, then
// drain the named binding (a stable-named-receiver, the #2146 borrow-pin).
//
// Each positive test compiles the rejected program through ownership
// checking and asserts hasErrors() with a substring match on the diagnostic.
// Each negative (compatible) test compiles through and runs under
// runAotChecked to confirm the stable-named-receiver case still works
// (regression guard for #2146). // per §28.3, stdlib §4.5 v0.195, #2169

#include <string>

#include "support/aot_support.hpp"        // runAotChecked — AOT + ASan/LSan
#include "support/ownership_support.hpp"  // parseAndCheckOwnership — diag pins
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// ctor +1 / dtor -1 net-zero dispose-once oracle. A double-free additionally
// fires ASan under runAotChecked (r.ran == false).
const char* kR =
    "module M; "
    "class R "
    "  public static live: int := 0; "
    "  public constructor() R.live := R.live + 1; end constructor; "
    "  destructor R.live := R.live - 1; end destructor; "
    "end R; "
    "class F "
    "  public static method Make(): owned OwningArray<R> "
    "    var a := new OwningArray<R>(); "
    "    a.Add(new R()); "
    "    return a; "
    "  end Make; "
    "end F; ";

}  // namespace

// #2169: the issue's exact repro — `using var d := F.Make().Drain();` —
// REJECTED. The drain view's array borrow outlives only the temporary's
// full-expression boundary; binding it past is a §28.3 temporary escape.
BEZEL_TEST(drain_on_temporary_rejection_2169, using_var_temp_drain_rejected) {
    auto r = parseAndCheckOwnership(
        std::string(kR) +
        "class P public static method Main(): int "
        "  do "
        "    using var drained := F.Make().Drain(); "
        "  end; "
        "  return R.live; "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The error names both the spec section and the binding-temporary-first
    // remedy.
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("`Drain()` over a temporary receiver")
            != std::string::npos) { found = true; break; }
    }
    BEZEL_EXPECT_TRUE(found);
}

// #2169 / variant: plain `var d := F.Make().Drain();` — same §28.3 escape
// (the binding's lifetime is not the temporary's K8 boundary). The
// auto-drop warning fires here too, but the §28.3 escape is the primary
// reject (the temp's array-borrow would outlive the temp).
BEZEL_TEST(drain_on_temporary_rejection_2169, plain_var_temp_drain_rejected) {
    auto r = parseAndCheckOwnership(
        std::string(kR) +
        "class P public static method Main(): int "
        "  do "
        "    var drained := F.Make().Drain(); "
        "  end; "
        "  return R.live; "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("`Drain()` over a temporary receiver")
            != std::string::npos) { found = true; break; }
    }
    BEZEL_EXPECT_TRUE(found);
}

// #2169 / chained-temp variant: `c.GetItems().Drain()` where the inner call
// returns a non-`owned` view of a field (per §28.6 getter rules). Here the
// outer receiver `c.GetItems()` is a CallExpr (a temporary method call),
// not a stable-named place — same §28.3 escape, same rejection.
BEZEL_TEST(drain_on_temporary_rejection_2169, chained_call_temp_drain_rejected) {
    auto r = parseAndCheckOwnership(
        std::string(kR) +
        "class Holder "
        "  private Items: OwningArray<R>; "
        "  public constructor() Items := new OwningArray<R>(); end constructor; "
        "  public method GetItems(): OwningArray<R> return Items; end GetItems; "
        "end Holder; "
        "class P public static method Main(): int "
        "  do "
        "    var c := new Holder(); "
        "    using var drained := c.GetItems().Drain(); "
        "  end; "
        "  return R.live; "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("`Drain()` over a temporary receiver")
            != std::string::npos) { found = true; break; }
    }
    BEZEL_EXPECT_TRUE(found);
}

// #2169 / REGRESSION GUARD for the #2146 stable-named-receiver borrow-pin:
// `tmp.Drain()` after `using var tmp := F.Make();` — the id-receiver IS the
// named binding, NOT a temporary, so the rejection does NOT fire and the
// drain is the §28.10 borrow-pin. Compiles clean, runs under ASan with the
// live-counter back to 0.
BEZEL_TEST(drain_on_temporary_rejection_2169,
           named_receiver_still_pin_spec_28_10) {
    JitResult r = runAotChecked(
        std::string(kR) +
        "class P public static method Main(): int "
        "  do "
        "    using var tmp := F.Make(); "
        "    using var drained := tmp.Drain(); "
        "  end; "
        "  return R.live; "
        "end Main; end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// #2169 / CONTROL: a non-`Drain()` call over a temporary receiver with the
// SAME owning-collection head type stays UNREJECTED here. (It may be
// admitted or rejected by other rules — this check is scoped to `Drain()`,
// which has the alias-on-receiver fact unique to it; the head check picks up
// the owning collection specifically when the call IS `Drain()`.)
BEZEL_TEST(drain_on_temporary_rejection_2169, non_drain_unchanged) {
    // A non-Drain read of `F.Make()` — the `Make()` call itself is on a
    // non-owning-collection head (F is a class, not an owning collection),
    // so this check does not engage with it.
    auto r = parseAndCheckOwnership(
        std::string(kR) +
        "class P public static method Main(): int "
        "  do "
        "    var v := F.Make(); "
        "  end; "
        "  return R.live; "
        "end Main; end P;");
    // The Drain-on-temporary rejection does not fire here (F is not an
    // owning collection head). Other diagnostics may fire on this program
    // (e.g. the auto-drop warning) but they're NOT the #2169 reject, so
    // the test asserts only that OUR specific message is absent.
    bool foundDrainReject = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("`Drain()` over a temporary receiver")
            != std::string::npos) { foundDrainReject = true; break; }
    }
    BEZEL_EXPECT_FALSE(foundDrainReject);
}
