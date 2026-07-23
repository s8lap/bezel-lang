// fix #2146 / §28.10, §28.11 (spec v0.220), stdlib §4.5 (v0.195): binding an
// owning-collection `recv.Drain()` result to a `using var` (or any owning
// obligation) when `recv` is a NAMED binding double-disposes the collection.
//
// Stdlib §4.5 (v0.195): "`Drain()` on `OwningArray<T>` returns the array
// itself at that static type — no separate enumerable object". So the drain
// view is an ALIAS of the receiver, not a fresh iterator frame; the receiver's
// owner keeps the ONE container obligation (§28.11 "ownership out" row moves
// ELEMENTS, never the container). Spec §28.10 "`using var` initialized from a
// borrow": "When the right-hand side of a `using var` declaration is a borrow
// rather than an owned value (a method call without an `owned` return — unless
// its declared return type is a `Task`-family type […]), the `using var` does
// **not** assume a disposal obligation […] at scope end, no `Dispose()` is
// called, because the value is not owned by this scope." `Drain()` has no
// `owned` return and is not `Task`-family; with the alias fact there is no
// fresh frame for the §28.3 iterator-factory convention to hand over, so the
// binding is the §28.10 borrow-pin, and the receiver's owner is the sole
// disposer.
//
// Pre-fix, producerState classified the call Owned (the IOwningEnumerable
// name-head fallback), so BOTH binding forms registered a second disposal —
// and worse, through the iterator-format slot-1 vtable path
// (registerUsingDisposal's isSyncIterEnum branch / the plain-var
// registerIterableSourceDisposal), which on a bz_array is type confusion:
// SEGV under ASan (the #2146 repro), with the program accepted clean.
//
// Each test drives the AOT + generated-code-ASan/LSan oracle (runAotChecked):
// without the fix the SEGV makes r.ran false; with it the run is clean and the
// live-counter returns to 0 (disposed exactly once, by the receiver's owner).
// Controls pin the shapes that must NOT change: direct Drain iteration
// (§29.2), a user yield-iterator IOwningEnumerable factory (§28.3 iterator
// convention — must KEEP adoption, else its frame leaks), and the §21.1
// `const` rejection of a Drain initializer. // per §28.10, §28.11, #2146

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
    "end R; ";

}  // namespace

// ===========================================================================
// (1) §28.10 / stdlib §4.5 PIN — the exact #2146 repro: `using var drained :=
// a.Drain();` where `a` is a using-var-owned OwningArray. The drain view
// aliases `a`; the pin registers no disposal, `a`'s scope-end cascade is the
// single disposal. Pre-fix: SEGV (slot-1 iterator dispose on a bz_array).
// ===========================================================================
BEZEL_TEST(using_var_drain_alias_2146,
           using_var_over_named_owning_array_drain_pins_spec_28_10) {
    JitResult r = runAotChecked(
        std::string(kR) +
        "class P "
        "  public static method Main(): int "
        "    do "
        "      using var a := new OwningArray<R>(); "
        "      a.Add(new R()); "
        "      a.Add(new R()); "
        "      using var drained := a.Drain(); "
        "    end; "
        "    return R.live; "  // 0 — both elements disposed once, by `a`
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);   // fails without the fix: ASan SEGV
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (2) §28.3 producers / stdlib §4.5 — the PLAIN-var binding form of the same
// alias: `var drained := a.Drain();`. Pre-fix producerState called it Owned
// (auto-drop registered the same slot-1 iterator dispose → SEGV) and warned
// §28.3 auto-drop. Post-fix it is a borrow (source: `a`) — no disposal, no
// auto-drop obligation.
// ===========================================================================
BEZEL_TEST(using_var_drain_alias_2146,
           plain_var_over_named_owning_array_drain_no_second_disposal_spec_28_3) {
    JitResult r = runAotChecked(
        std::string(kR) +
        "class P "
        "  public static method Main(): int "
        "    do "
        "      using var a := new OwningArray<R>(); "
        "      a.Add(new R()); "
        "      a.Add(new R()); "
        "      var drained := a.Drain(); "
        "    end; "
        "    return R.live; "  // 0 — disposed once, by `a`
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);   // fails without the fix: ASan SEGV
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (3) §28.10 / stdlib §4.9 PIN — the OwningDictionary variant: `using var
// drained := d.Drain();` (IOwningEnumerable<DrainedEntry<K,V>>). Same alias
// shape, same single-owner rule: `d`'s cascade is the one disposal.
// ===========================================================================
BEZEL_TEST(using_var_drain_alias_2146,
           using_var_over_named_owning_dictionary_drain_pins_spec_28_10) {
    JitResult r = runAotChecked(
        std::string(kR) +
        "class P "
        "  public static method Main(): int "
        "    do "
        "      using var d := new OwningDictionary<string, R>(); "
        "      d.Add(\"a\", new R()); "
        "      d.Add(\"b\", new R()); "
        "      using var drained := d.Drain(); "
        "    end; "
        "    return R.live; "  // 0 — both values disposed once, by `d`
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (4) §28.10 PIN — the receiver is an owned FIELD read (`_items.Drain()`
// inside the declaring class). The alias fact is receiver-form-independent:
// the container obligation stays with the field's holder, whose cascade is
// the single disposal.
// ===========================================================================
BEZEL_TEST(using_var_drain_alias_2146,
           using_var_over_owned_field_drain_pins_spec_28_10) {
    JitResult r = runAotChecked(
        std::string(kR) +
        "class Pool "
        "  private owned _items: OwningArray<R>; "
        "  public constructor() "
        "    _items := new OwningArray<R>(); "
        "    _items.Add(new R()); "
        "    _items.Add(new R()); "
        "  end constructor; "
        "  public method Bind(): int "
        "    using var drained := _items.Drain(); "
        "    return 0; "
        "  end Bind; "
        "end Pool; "
        "class P "
        "  public static method Main(): int "
        "    do "
        "      using var p := new Pool(); "
        "      var i := p.Bind(); "
        "    end; "
        "    return R.live; "  // 0 — elements disposed once, by the pool field
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (4b) §28.10 / §28.6 PIN — the receiver is a borrowed PARAMETER (the §28.6
// pass-and-fill shape: "Bezel already admits [Drain] on a borrowed collection").
// The caller keeps the container obligation; the callee's pin disposes nothing.
// ===========================================================================
BEZEL_TEST(using_var_drain_alias_2146,
           using_var_over_borrowed_param_drain_pins_spec_28_6) {
    JitResult r = runAotChecked(
        std::string(kR) +
        "class Util "
        "  public static method Bind(a: OwningArray<R>): int "
        "    using var drained := a.Drain(); "
        "    return 0; "
        "  end Bind; "
        "end Util; "
        "class P "
        "  public static method Main(): int "
        "    do "
        "      using var a := new OwningArray<R>(); "
        "      a.Add(new R()); "
        "      var i := Util.Bind(a); "
        "    end; "
        "    return R.live; "  // 0 — disposed once, by the caller's `a`
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (4c) §28.10 / stdlib §4.10 PIN — the OwningSet member of the owning family
// (guards the isOwningCollectionHead wiring across the five sequence heads).
// ===========================================================================
BEZEL_TEST(using_var_drain_alias_2146,
           using_var_over_named_owning_set_drain_pins_spec_28_10) {
    JitResult r = runAotChecked(
        std::string(kR) +
        "class P "
        "  public static method Main(): int "
        "    do "
        "      using var s := new OwningSet<R>(); "
        "      s.Add(new R()); "
        "      using var drained := s.Drain(); "
        "    end; "
        "    return R.live; "  // 0 — disposed once, by `s`
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (5) CONTROL (§29.2 / stdlib §4.5) — direct `for var x in a.Drain()`
// iteration, the blessed idiom, is untouched by the pin recognition: elements
// transfer to the loop variable (owned, discharged by the K3.4 marker), the
// emptied array disposes at scope end. Was clean before the fix; must stay so.
// ===========================================================================
BEZEL_TEST(using_var_drain_alias_2146,
           direct_drain_iteration_control_unaffected_spec_29_2) {
    JitResult r = runAotChecked(
        std::string(kR) +
        "class P "
        "  public static method Main(): int "
        "    do "
        "      using var a := new OwningArray<R>(); "
        "      a.Add(new R()); "
        "      a.Add(new R()); "
        "      for var x in a.Drain() do "
        "        using var u := owned x; "
        "      end; "
        "      if R.live <> 0 then return 2; end; "  // drained + disposed in-loop
        "    end; "
        "    return R.live; "
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (6) CONTROL (§28.3 iterator convention, over-pin guard) — a USER
// yield-iterator method returning IOwningEnumerable<R> is a FACTORY: its call
// mints a fresh caller-owned frame that MUST keep its adoption (the §28.3
// "callers must discharge the ownership obligation" convention). The #2146
// recognition is confined to the builtin owning-collection `Drain()` shape —
// a user call (resolved MethodSymbol) is excluded, so `seq` still owns and
// disposes the frame (a pin here would leak it; LSan would flag).
// ===========================================================================
BEZEL_TEST(using_var_drain_alias_2146,
           user_owning_iterator_factory_still_adopts_control_spec_28_3) {
    JitResult r = runAotChecked(
        std::string(kR) +
        "class P "
        "  public static method Gen(): IOwningEnumerable<R> "
        "    yield return new R(); "
        "    yield return new R(); "
        "  end Gen; "
        "  public static method Main(): int "
        "    do "
        "      using var seq := P.Gen(); "
        "      for var x in seq do "
        "        using var u := owned x; "
        "      end; "
        "    end; "
        "    return R.live; "  // 0 — yielded elements disposed; frame freed
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (7) CONTROL (§21.1 v0.201) — `const q := a.Drain();` stays REJECTED: §21.1
// "Rejected, with a diagnostic naming this rule and the `using var` remedy:
// […] a deferred-activation producer (a lazy query, `Drain()`, `ReceiveAll()`
// — affine values, §29.11)". The #2146 reclassification of the alias call to
// Borrowed must not silently admit the const form.
// ===========================================================================
BEZEL_TEST(using_var_drain_alias_2146,
           const_over_drain_still_rejected_spec_21_1) {
    auto r = parseAndCheckOwnership(
        std::string(kR) +
        "class P "
        "  public static method Main(): int "
        "    using var a := new OwningArray<R>(); "
        "    a.Add(new R()); "
        "    const q := a.Drain(); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
