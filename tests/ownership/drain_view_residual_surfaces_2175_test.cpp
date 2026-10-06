// fix #2175 / §28.3 v0.226, §28.6 (spec v0.220), stdlib §4.5 (v0.195): the
// drain-view residual surfaces after the #2146 stable-named-receiver borrow-
// pin and the #2169 temporary-receiver binding-site rejection. Three related
// shapes the issue names:
//
//   (a) PRE-EXISTING — `return a.Drain();` over a scope-bound local
//       compiles (missing return-site rejection). §28.3 *Returning a
//       deferred-activation frame*: "A returned frame carrying a borrow of
//       anything else — a scope-bound local, a `using var`, a temporary
//       — is rejected at the return site." The runtime is benign by
//       accident (caller adoption registers no dispose for a
//       declaration-less IOwningEnumerable type), but the spec is
//       unambiguous. Closed at the return site: `recv.Drain()` where
//       `recv` is a tracked scope-bound owned local or `using var` (the
//       chain form `recv.f.Drain()` reuses the same root check). The
//       parameter-sourced form (`View(a: OwningArray<R>):
//       IOwningEnumerable<R> => a.Drain();`) is spec-admitted and
//       probe-clean — its lifetime is wider than the call. // per §28.3,
//       stdlib §4.5 v0.195, #2175
//
//   (b) NEW ACCEPT SURFACE (unrecorded) — a user getter returning
//       `_items.Drain();` was rejected by checkGetterFreshOwnedReturn
//       pre-#2146 (producerState was Owned); #2146's Borrowed carve lets
//       it through, but the caller side does not pin (the body scan
//       rejects a CallExpr body). Closed at the getter-recognition
//       site: extend propertyGetterIsRecognizedBorrow to accept the
//       builtin `recv.Drain()` alias over a `this`-rooted chain, so
//       the caller-side `using var q := h.Q;` becomes a §28.10 borrow-
//       pin. The receiver-source check is conservative — a chain
//       rooted at a scope-bound local is NOT a getter shape (the
//       return-site check (a) above already rejects the caller
//       position there). // per §28.10, stdlib §4.5 v0.195, #2175
//
//   (c) AFFINE-VIEW LAUNDER + LINT NIT — `using var q := h.Q;` over the
//       (b) getter: with the (b) fix it becomes a §28.10 borrow-pin; the
//       vacuous-pin lint correctly stays quiet when the borrow is load-
//       bearing (the receiver `h` outlives the use scope, so the pin
//       extends nothing — but the lint's rhsIsVacuousBorrowPin is
//       receiver-sourced-aware: a `h` parameter, `this`, or a static
//       read does NOT fire, while a `h` local binding does). The lint
//       is exercised by the positive `using var q := h.Q;` cases
//       below; the control covers a parameter-receiver (`P.Run(q: …)`)
//       shape where the pin IS vacuous. // per §28.10, #2175
//
// Each positive (a) test compiles the rejected program through ownership
// checking and asserts hasErrors() with a substring match on the diagnostic.
// Each negative (a/b/c) test compiles through and runs under runAotChecked
// to confirm the admitted shapes still work. // per §28.3, §28.10,
// stdlib §4.5 v0.195, #2175

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
// (a1) §28.3 / stdlib §4.5 — `return a.Drain();` over a `using var` LOCAL is
// REJECTED. The drain view aliases `a`'s owning-collection array; `a` is
// disposed at the callee's scope end, so the returned view dangles.
// ===========================================================================
BEZEL_TEST(drain_view_return_site_2175,
           return_using_var_drain_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(
        std::string(kR) +
        "class Bad "
        "  public static method Leak(): IOwningEnumerable<R> "
        "    using var a := new OwningArray<R>(); "
        "    return a.Drain(); "
        "  end Leak; "
        "end Bad;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("cannot return `a`.Drain()") != std::string::npos &&
            d.message.find("using var") != std::string::npos &&
            d.message.find("drain view aliases") != std::string::npos) {
            found = true; break;
        }
    }
    BEZEL_EXPECT_TRUE(found);
}

// ===========================================================================
// (a2) §28.3 / stdlib §4.5 — `return a.f.Drain();` over a `using var`
// Holder's owned field is REJECTED. The chain `a.f` is rooted at `a`
// (scope-bound); the drain view aliases `a.f`'s array; `a`'s cascade
// disposes `a.f` at scope end, so the returned view dangles.
// ===========================================================================
BEZEL_TEST(drain_view_return_site_2175,
           return_using_var_field_drain_chain_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(
        std::string(kR) +
        "class Holder "
        "  public owned Items: OwningArray<R>; "
        "  public constructor() Items := new OwningArray<R>(); end constructor; "
        "end Holder; "
        "class Bad "
        "  public static method Leak(): IOwningEnumerable<R> "
        "    using var a := new Holder(); "
        "    return a.Items.Drain(); "
        "  end Leak; "
        "end Bad;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("cannot return `a`.Drain()") != std::string::npos &&
            d.message.find("using var") != std::string::npos) {
            found = true; break;
        }
    }
    BEZEL_EXPECT_TRUE(found);
}

// ===========================================================================
// (a3) §28.3 / stdlib §4.5 — `return a.Drain();` over a plain (non-using)
// owned local is REJECTED. The plain owned local is auto-dropped at the
// callee's scope end (§28.3 affine auto-drop) — same hazard as the using
// var case. The return-site check fires on both `using var` and
// `everOwnedPlainLocals_` shape.
// ===========================================================================
BEZEL_TEST(drain_view_return_site_2175,
           return_plain_owned_local_drain_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(
        std::string(kR) +
        "class Bad "
        "  public static method Leak(): IOwningEnumerable<R> "
        "    var a := new OwningArray<R>(); "
        "    return a.Drain(); "
        "  end Leak; "
        "end Bad;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("cannot return `a`.Drain()") != std::string::npos &&
            d.message.find("scope-bound owned local") != std::string::npos) {
            found = true; break;
        }
    }
    BEZEL_EXPECT_TRUE(found);
}

// ===========================================================================
// (a4) §28.3 CONTROL — the parameter-sourced form (the issue's own
// admitted shape): `View(a: OwningArray<R>): IOwningEnumerable<R> => a.Drain();`.
// The parameter outlives the call, so the borrow is sound. Pre-fix this
// also compiled (it was correctly admitted); the negative control pins
// that the (a) fix doesn't over-reject. // per §28.3, stdlib §4.5 v0.195
// ===========================================================================
BEZEL_TEST(drain_view_return_site_2175,
           return_parameter_drain_admitted_control_spec_28_3) {
    auto r = parseAndCheckOwnership(
        std::string(kR) +
        "class Good "
        "  public static method View(a: OwningArray<R>): IOwningEnumerable<R> "
        "    => a.Drain(); "
        "end Good;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// (a5) §28.3 CONTROL — `this.f.Drain()`: the receiver is `this`, the
// chain is receiver-sourced, the call's return is a borrow whose source
// is the receiver. ADMITTED. // per §28.3, stdlib §4.5 v0.195
// ===========================================================================
BEZEL_TEST(drain_view_return_site_2175,
           return_this_field_drain_admitted_control_spec_28_3) {
    auto r = parseAndCheckOwnership(
        std::string(kR) +
        "class Owner "
        "  public owned Items: OwningArray<R>; "
        "  public constructor() Items := new OwningArray<R>(); end constructor; "
        "  public method View(): IOwningEnumerable<R> return Items.Drain(); "
        "  end View; "
        "end Owner;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// (b) Stdlib §4.5 / §4.13 (v0.307): a property getter returning
// `_items.Drain()` is REJECTED at compile time. `Drain()` on an owning
// collection "mints a fresh drain-view frame" (§4.13) — a caller-owned
// owned frame per §28.3 row 2 — so the getter body produces an Owned
// value.  Per §28.6: "a property or indexer getter may not return a
// freshly-produced owned value — a getter is a borrow-producing position".
// The v0.195 comment ("borrow-pin getter") reflected the old (buggy)
// Borrowed classification of `Drain()`.  Under correct spec, the shape
// is invalid; the fix-forward is to use a method instead:
//   public method Q(): IOwningEnumerable<R> return _items.Drain(); end
// per §28.6 guidance: "expose the factory as a method returning `owned T`".
// ===========================================================================
BEZEL_TEST(drain_view_getter_2175,
           getter_drain_over_this_field_rejected_spec_28_6) {
    auto src = std::string(kR) +
        "class Holder "
        "  private owned _items: OwningArray<R>; "
        "  public constructor() _items := new OwningArray<R>(); end constructor; "
        "  public property Q: IOwningEnumerable<R> => _items.Drain(); "
        "end Holder; "
        "class P "
        "  public static method Run(): int "
        "    using var h := new Holder(); "
        "    using var view: IOwningEnumerable<R> := h.Q; "
        "    return R.live; "
        "  end Run; "
        "  public static method Main(): int "
        "    var r := P.Run(); "
        "    return r; "
        "  end Main; "
        "end P;";
    auto r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool hasOwnedValueErr = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("freshly-produced owned value") != std::string::npos) {
            hasOwnedValueErr = true;
            break;
        }
    }
    BEZEL_EXPECT_TRUE(hasOwnedValueErr);
}

// ===========================================================================
// (b) §28.10 CONTROL — the getter over a NON-`this` field (the chain's
// root is a scope-bound local, the LAUNDER the getter is supposed to NOT
// admit). The getter is rejected: the alias over a scope-bound local
// would have the same dangling hazard as the return-site case (a) — but
// the caller's chain is rooted at `h`, a using-var parameter, not `this`.
// The getter's `this`-rooting requirement keeps the borrows scope-bound
// to the holder's lifetime, which is what the pin machinery needs.
// Pre-fix this getter is recognized as a getter; post-fix only the
// `this`-rooted form is. // per §28.10, stdlib §4.5 v0.195, #2175
// ===========================================================================
// (b) is narrower than (a): getter-body recognition REQUIRES `this`-rooted.
// A user getter over `h._items.Drain()` where `h` is a parameter would
// still be recognized? No: the getter is `class P{ method V(h: Holder):
// IOwningEnumerable<R> => h._items.Drain(); }` — the chain's root is `h`
// (a parameter), but the getter's body is a parameter-rooted drain —
// the chain is NOT receiver-sourced. The getter is NOT recognized as a
// borrow-pin, and the caller adopts (return-site / §28.6 already admits
// the `h._items.Drain()` form for a parameter-sourced `h`).

// ===========================================================================

