// Spec-derived tests for the §28.3 *Bare-type-parameter returns* keystone
// (v0.194, #1801 sub-task S1) — the caller-side union-source attach + the
// consume-site freeze that closes the use-after-free.
//
// §28.3 "Bare-type-parameter returns" (v0.194), producers row (2nd exception):
// "a call whose declared return type is a **bare type parameter** ... is never
// this row's case: at class or interface instantiations it is a **borrow** of
// the call's receiver and its T-reaching plain arguments."
//
// §28.3 caller side (v0.194): "With a **non-empty** potential source set ... the
// call produces a **borrow** carrying the **union of the source-liveness
// constraints** of the receiver and of every argument bound to a source
// parameter ... the result is usable only where every contributing source is
// still live. ... consuming a contributing source while the result borrow is
// live is rejected by the consume-site freeze below; an explicit `Dispose()` of
// a source remains the announced kill with the checked-mode backstop (§28.2)."
//
// The potential source set (§28.3, v0.194): "the call's receiver ... together
// with every **plain** (non-`owned`) parameter whose declared type mentions the
// returned type parameter; `owned` parameters are never sources." A null-literal
// argument, and "any argument or receiver whose substituted type is a value or
// runtime-managed type, contributes no constraint" — so `Pick(1, 2)` (value
// instantiation) attaches no source and never freezes.
//
// S1 SCOPE: the NON-empty-set caller side only. The result stays classified
// Borrowed (producerState is unchanged — no owned classification is introduced);
// this suite pins that consuming a contributing source (via a `move` / owned-
// argument transfer — the frozen consume forms) while the result is live is
// rejected, and that an explicit `Dispose()` is the announced kill (accepted at
// compile time, backstopped at runtime by checked disposal, §28.2) — NOT a
// compile freeze. The empty-set factory fallback is a sibling sub-task.

#include <string>
#include <string_view>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

bool errorSays(const OwnershipResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// (a) THE KEYSTONE UAF. A static `Pick<T>(a: T, b: T): T` yields a borrow with
// sources {a, b}. Consuming `a` (an owned-argument transfer into `Sink(owned
// c)`) while the result `m` has a reachable later use (`m.Poke()`) must be
// frozen — the returned value *is* one of the caller's arguments, so freeing it
// leaves `m` dangling. Before S1 the static call attached NO source (a static
// call has no receiver root), so this compiled clean — the use-after-free the
// keystone closes.
// ---------------------------------------------------------------------------

BEZEL_TEST(bare_type_param_returns,
           static_pick_consume_arg_while_result_live_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "  public method Poke(): int return 1; end Poke; "
        "end C; "
        "class Util "
        "  public static method Pick<T>(a: T, b: T): T return a; end Pick; "
        "  public static method Sink(owned c: C): int c.Dispose(); return 0; "
        "  end Sink; "
        "end Util; "
        "class Prog "
        "  public static method Main(): int "
        "    var a := new C(); "
        "    var b := new C(); "
        "    var m := Util.Pick(a, b); "
        "    _ := Util.Sink(owned a); "  // consume `a` while `m` live
        "    var p := m.Poke(); "            // `m`'s surviving use
        "    b.Dispose(); "
        "    return p; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot consume `a` here"));
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow held by `m`"));
}

// ---------------------------------------------------------------------------
// (b) OVER-REJECT GUARD — result dead at the consume. The same program with
// `m.Poke()` BEFORE the consume: `m`'s only use precedes it, so the borrow is
// not live at the consume and the freeze must NOT fire (accept-after-last-use).
// ---------------------------------------------------------------------------

BEZEL_TEST(bare_type_param_returns,
           static_pick_result_dead_before_consume_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "  public method Poke(): int return 1; end Poke; "
        "end C; "
        "class Util "
        "  public static method Pick<T>(a: T, b: T): T return a; end Pick; "
        "  public static method Sink(owned c: C): int c.Dispose(); return 0; "
        "  end Sink; "
        "end Util; "
        "class Prog "
        "  public static method Main(): int "
        "    var a := new C(); "
        "    var b := new C(); "
        "    var m := Util.Pick(a, b); "
        "    var p := m.Poke(); "            // `m`'s last use, before the consume
        "    _ := Util.Sink(move a); "  // now safe — `m` is dead
        "    b.Dispose(); "
        "    return p; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// (c) OVER-REJECT GUARD — value instantiation attaches no source. `Pick(1, 2)`
// at T=int: the arguments' substituted type is a value type, which contributes
// no constraint (§28.3, v0.194), so the potential source set is empty and the
// result never freezes anything. The recognizer fires (the return is a bare type
// parameter) but the source set is empty — nothing is attached.
// ---------------------------------------------------------------------------

BEZEL_TEST(bare_type_param_returns,
           value_instantiation_attaches_no_source_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Util "
        "  public static method Pick<T>(a: T, b: T): T return a; end Pick; "
        "end Util; "
        "class Prog "
        "  public static method Main(): int "
        "    var x := Util.Pick(1, 2); "
        "    var y := 5; "
        "    return x + y; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// (d) The classification is inherited from the ENCLOSING TYPE's parameter, not
// only a method-level one: `class Box<T> ... static method GetIt(a: T, b: T): T`
// returns the class's own `T`. Consuming `a` while the result is live freezes
// exactly as the method-level case — the recognizer matches the callee's parent
// type's generic parameters. (Distinguishing a bare parameter from a 0-arity
// builtin, which shares the null-declaration Named shape, requires this name-set
// membership check.)
// ---------------------------------------------------------------------------

BEZEL_TEST(bare_type_param_returns,
           enclosing_type_param_static_consume_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "  public method Poke(): int return 1; end Poke; "
        "end C; "
        "class Box<T> "
        "  public static method GetIt(a: T, b: T): T return a; end GetIt; "
        "  public static method Sink(owned c: C): int c.Dispose(); return 0; "
        "  end Sink; "
        "end Box; "
        "class Prog "
        "  public static method Main(): int "
        "    var a := new C(); "
        "    var b := new C(); "
        "    var m := Box<C>.GetIt(a, b); "
        "    _ := Box<C>.Sink(owned a); "
        "    var p := m.Poke(); "
        "    b.Dispose(); "
        "    return p; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot consume `a` here"));
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow held by `m`"));
}

// ---------------------------------------------------------------------------
// (e) The RECEIVER is a source too (§28.3: "the call's receiver ... together
// with every plain parameter ..."). An instance `Choose<T>(a: T, b: T): T`
// carried by receiver `recv`: consuming `recv` while the result is live freezes,
// because the returned borrow could alias the receiver.
// ---------------------------------------------------------------------------

BEZEL_TEST(bare_type_param_returns,
           instance_receiver_source_consume_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "  public method Poke(): int return 1; end Poke; "
        "  public method Choose<T>(a: T, b: T): T return a; end Choose; "
        "end C; "
        "class Util "
        "  public static method Sink(owned c: C): int c.Dispose(); return 0; "
        "  end Sink; "
        "end Util; "
        "class Prog "
        "  public static method Main(): int "
        "    var recv := new C(); "
        "    var a := new C(); "
        "    var b := new C(); "
        "    var m := recv.Choose(a, b); "
        "    _ := Util.Sink(owned recv); "  // consume the receiver
        "    var p := m.Poke(); "
        "    a.Dispose(); b.Dispose(); "
        "    return p; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot consume `recv` here"));
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow held by `m`"));
}

// ---------------------------------------------------------------------------
// (f) BOUNDARY — an explicit `Dispose()` of a source is the ANNOUNCED KILL, not
// a compile freeze (§28.3, v0.194: "an explicit `Dispose()` of a source remains
// the announced kill with the checked-mode backstop (§28.2)"). Disposing `a`
// while `m` is live is accepted at compile time; the resulting use-after-dispose
// is caught at runtime by checked disposal, exactly as elsewhere in §28. This
// guards against over-implementing the freeze onto the dispose path (which would
// contradict the spec and diverge from the existing consume-site-freeze
// machinery, whose frozen forms are `move` / owned-argument transfer / drop-on-
// overwrite — see consume_site_freeze_span_1726_test.cpp).
// ---------------------------------------------------------------------------

BEZEL_TEST(bare_type_param_returns,
           explicit_dispose_is_announced_kill_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "  public method Poke(): int return 1; end Poke; "
        "end C; "
        "class Util "
        "  public static method Pick<T>(a: T, b: T): T return a; end Pick; "
        "end Util; "
        "class Prog "
        "  public static method Main(): int "
        "    var a := new C(); "
        "    var b := new C(); "
        "    var m := Util.Pick(a, b); "
        "    a.Dispose(); "                   // announced kill — NOT a freeze
        "    var p := m.Poke(); "
        "    b.Dispose(); "
        "    return p; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// S2 (#1801) — the ESCAPE side. S1 attaches the union source set and freezes a
// consume while the result is live; S2 closes the complementary hole where the
// bare-T borrow ESCAPES its sources: returning the holder, or storing it into
// an owned field / longer-lived sink, when a carried source is a live owned
// scope-bound local. §28.3 / §9.7 borrow-lifetime rule, adapted to the union
// source set (§28.3 *Bare-type-parameter returns*, v0.194).
// ===========================================================================

// ---------------------------------------------------------------------------
// (g) THE ESCAPE-VIA-RETURN HOLE. An INSTANCE method binds `m := Pick(a, b)`
// over owned scope-bound locals `a`/`b`, then `return m;`. `m` borrows the
// union {a, b}; both are disposed at the method's scope end, so the returned
// borrow dangles in the caller — a use-after-dispose. Before S2 the holder
// carried its source set only in recordBorrowLocalSource_ (not st->source), so
// the indirect-return escape check missed it and this compiled CLEAN. (A STATIC
// method would already be rejected by the broader static-borrow-return rule
// §28.3 #1547; the hole is specifically the instance-method form.) // per §28.9
// ---------------------------------------------------------------------------

BEZEL_TEST(bare_type_param_returns,
           return_holder_escapes_owned_local_source_rejected_spec_28_9) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end C; "
        "class Util "
        "  public static method Pick<T>(a: T, b: T): T return a; end Pick; "
        "end Util; "
        "class Prog "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "  public method Make(): C "
        "    var a := new C(); "
        "    var b := new C(); "
        "    var m := Util.Pick(a, b); "
        "    return m; "                       // escapes owned locals a/b
        "  end Make; "
        "  public static method Main(): int return 0; end Main; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "returned `m` carries a borrow of `a`"));
    BEZEL_EXPECT_TRUE(errorSays(r, "bare-type-parameter"));
}

// ---------------------------------------------------------------------------
// (h) OVER-REJECT GUARD — PARAM-sourced result outlives the return. The same
// shape but the contributing sources are PARAMETERS (`p`/`q`), whose lifetime
// is the caller's frame and so outlives the return. §28.3 "the result is usable
// only where every contributing source is still live" — a parameter is still
// live at (and past) the return, so it is not an escape and the return must be
// ACCEPTED. This is the mirror of the single-source guard: only a still-live
// owned scope-bound obligation is flagged, never a source that outlives it.
// ---------------------------------------------------------------------------

BEZEL_TEST(bare_type_param_returns,
           return_holder_param_sourced_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end C; "
        "class Util "
        "  public static method Pick<T>(a: T, b: T): T return a; end Pick; "
        "end Util; "
        "class Prog "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "  public method FromParams(p: C, q: C): C "
        "    var m := Util.Pick(p, q); "
        "    return m; "                        // p/q outlive the return — OK
        "  end FromParams; "
        "  public static method Main(): int return 0; end Main; "
        "end Prog;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// (i) OVER-REJECT GUARD — a freshly produced owned value carries no borrow.
// `return new C();` (via a local bound to a fresh `new C()`) is an owned-value
// transfer, not a borrow: no bare-T holder, no carried source, nothing to
// escape. Must be ACCEPTED. Guards the return-escape scan against firing on
// ordinary owned returns.
//
// The return type is `owned C`, not `C`. That is REQUIRED as of v0.307 and is
// not incidental to this row: §28.3's announcement table row 4 admits only a
// receiver-sourced borrow through a PLAIN concrete class return, and §28.6's
// fresh-value rule rejects a freshly-produced owned value there. `owned` is
// how a transfer is spelled.
//
// The row originally declared `: C` and asserted `!hasErrors()`. `fe95b173`
// implemented row 4 and it went red — but the row's AXIS never changed: it
// guards the BORROW-ESCAPE SCAN, a different rule, and `!hasErrors()` is an
// over-broad oracle that catches every rule including ones the row is not
// about. Spelling the transfer correctly keeps this a genuine over-reject
// guard for the scan it exists to guard, instead of turning it into a row
// about row 4. // per §28.3 row 4, §28.6, v0.307
// ---------------------------------------------------------------------------

BEZEL_TEST(bare_type_param_returns,
           return_fresh_owned_value_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end C; "
        "class Prog "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "  public method MakeFresh(): owned C "
        "    var c := new C(); "
        "    return c; "                        // fresh owned transfer — OK
        "  end MakeFresh; "
        "  public static method Main(): int return 0; end Main; "
        "end Prog;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// (j) ESCAPE-VIA-OWNED-STORE. Storing the bare-T holder into an OWNED FIELD of
// `this` (which outlives the method) lets the borrow of owned local `a` outlive
// its source — the same §28.9/§9.7 escape as the return, through the store path
// (checkBorrowStoreEscape, fed by the extended ownedLocalBorrowSource). Before
// S2 the holder's source set was invisible to the store check and this compiled
// clean. // per §28.9, §9.7
// ---------------------------------------------------------------------------

BEZEL_TEST(bare_type_param_returns,
           store_holder_into_owned_field_rejected_spec_28_9) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end C; "
        "class Util "
        "  public static method Pick<T>(a: T, b: T): T return a; end Pick; "
        "end Util; "
        "class Prog "
        "  public owned _held: C; "
        "  public constructor() _held := new C(); end constructor; "
        "  destructor end destructor; "
        "  public method Stash(): int "
        "    var a := new C(); "
        "    var b := new C(); "
        "    var m := Util.Pick(a, b); "
        "    _held := m; "                      // borrow stored into owned field
        "    a.Dispose(); "
        "    b.Dispose(); "
        "    return 0; "
        "  end Stash; "
        "  public static method Main(): int return 0; end Main; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow of `a` stored into a field"));
}
