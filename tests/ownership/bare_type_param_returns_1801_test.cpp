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
        "    var _ := Util.Sink(owned a); "  // consume `a` while `m` live
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
        "    var _ := Util.Sink(owned a); "  // now safe — `m` is dead
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
        "    var _ := Box<C>.Sink(owned a); "
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
        "    var _ := Util.Sink(owned recv); "  // consume the receiver
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
