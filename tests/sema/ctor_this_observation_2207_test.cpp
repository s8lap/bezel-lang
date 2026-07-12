// Spec-derived tests for §22.7 (v0.184) — construction-time `this`-observation
// rules ("Observing `this` during construction"). Three rules keep §7.9's
// guarantee (a fully-constructed object has every non-nullable field set) true
// while `this` is under construction:
//
//   Rule 1 (observation gate): a synchronous observation of `this` in a
//     constructor body — an instance call on `this`, `this` as an argument,
//     `this` stored, `this` as an is/as operand — is admitted only where every
//     §7.9-strict field is definitely assigned on every reaching path. In the
//     `base(...)`/`this(...)` chaining position no field is assigned yet, so any
//     observation there is a compile error.
//   Rule 2: a dispatching `virtual`/`override`/`abstract` call on `this` from a
//     constructor body is rejected unless the class is `sealed` and not
//     `abstract`; `base.M()` is non-dispatching and exempt.
//   Rule 3: capturing `this` in a closure is NOT a synchronous observation and
//     is not gated (it observes at invocation).
//
// This closes the CRITICAL hole the v0.184 settlement named: before the fix a
// virtual call in a base constructor over an unset derived field compiled and
// segfaulted (issue #1679, probe ctor_virtual.bzl); a plain read-before-assign
// observed an unset non-nullable field.
//
// The DA harness runs collectSymbols → resolveTypes → typecheck →
// checkDefiniteAssignment (where the §22.7 check lives); it does not run the
// ownership pass, so the only diagnostics here are the §22.7 observation errors
// under test — each reject case is clean without the fix and errors with it.

#include <string>

#include "support/da_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckDA;

namespace {

bool daHasError(const bezel::test::DAResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ===========================================================================
// Rule 1 — the observation gate (same-class field DA). §22.7 R-S1 trace 2.
// ===========================================================================

// REJECT: an instance call on `this` before the declaring class's non-nullable
// field is assigned observes an unset field.
BEZEL_TEST(sema_ctor_obs_2207, instance_call_before_field_assigned_rejected) {
    auto r = parseAndCheckDA(
        "module M; "
        "sealed class W "
        "  private _name: string; "
        "  public constructor(n: string) "
        "    Describe(); "        // observation BEFORE _name assigned
        "    _name := n; "
        "  end constructor; "
        "  public method Describe() end Describe; "
        "end W;");
    BEZEL_EXPECT_TRUE(daHasError(r, "before non-nullable field"));
}

// ACCEPT: the same observation after the field is assigned.
BEZEL_TEST(sema_ctor_obs_2207, instance_call_after_field_assigned_clean) {
    auto r = parseAndCheckDA(
        "module M; "
        "sealed class W "
        "  private _name: string; "
        "  public constructor(n: string) "
        "    _name := n; "
        "    Describe(); "        // observation AFTER _name assigned
        "  end constructor; "
        "  public method Describe() end Describe; "
        "end W;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ACCEPT: a nullable field has a defined default (§7.9), so it is not in the
// gate's needing-set — an observation before it is assigned is fine.
BEZEL_TEST(sema_ctor_obs_2207, nullable_field_not_gated_clean) {
    auto r = parseAndCheckDA(
        "module M; "
        "sealed class W "
        "  private _name: string?; "
        "  public constructor() "
        "    Describe(); "
        "    _name := \"x\"; "
        "  end constructor; "
        "  public method Describe() end Describe; "
        "end W;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ACCEPT: a value-typed field has a defined default and is not §7.9-strict, so
// an observation before it is assigned is fine.
BEZEL_TEST(sema_ctor_obs_2207, value_typed_field_not_gated_clean) {
    auto r = parseAndCheckDA(
        "module M; "
        "sealed class C "
        "  private _n: int; "
        "  public constructor() "
        "    Describe(); "
        "    _n := 5; "
        "  end constructor; "
        "  public method Describe() end Describe; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ACCEPT: a non-nullable auto-property is §7.9-strict backing storage; a store
// to it counts toward the gate, so an observation after it is admitted (§22.7:
// "auto-property assignments … each is the backing field's assignment").
BEZEL_TEST(sema_ctor_obs_2207, autoprop_assigned_before_observe_clean) {
    auto r = parseAndCheckDA(
        "module M; "
        "sealed class Person "
        "  public property Name: string; "
        "  public constructor(n: string) "
        "    Name := n; "         // auto-property store counts toward the gate
        "    Describe(); "
        "  end constructor; "
        "  public method Describe() end Describe; "
        "end Person;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// REJECT: an observation before the non-nullable auto-property is assigned would
// read its unset backing field.
BEZEL_TEST(sema_ctor_obs_2207, autoprop_observe_before_assign_rejected) {
    auto r = parseAndCheckDA(
        "module M; "
        "sealed class Person "
        "  public property Name: string; "
        "  public constructor(n: string) "
        "    Describe(); "        // observation BEFORE Name assigned
        "    Name := n; "
        "  end constructor; "
        "  public method Describe() end Describe; "
        "end Person;");
    BEZEL_EXPECT_TRUE(daHasError(r, "before non-nullable field"));
}

// ===========================================================================
// Rule 1 — flow-sensitivity: "definitely assigned on EVERY reaching path".
// ===========================================================================

// ACCEPT: the field is assigned on both branches before the observation.
BEZEL_TEST(sema_ctor_obs_2207, field_assigned_on_both_branches_clean) {
    auto r = parseAndCheckDA(
        "module M; "
        "sealed class W "
        "  private _name: string; "
        "  public constructor(b: bool) "
        "    if b then _name := \"a\"; else _name := \"b\"; end; "
        "    Describe(); "        // assigned on every path → admitted
        "  end constructor; "
        "  public method Describe() end Describe; "
        "end W;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// REJECT: the field is assigned on only one branch — not on every reaching path.
BEZEL_TEST(sema_ctor_obs_2207, field_assigned_on_one_branch_rejected) {
    auto r = parseAndCheckDA(
        "module M; "
        "sealed class W "
        "  private _name: string; "
        "  public constructor(b: bool) "
        "    if b then _name := \"a\"; end; "
        "    Describe(); "        // NOT assigned on the else path
        "    _name := \"b\"; "
        "  end constructor; "
        "  public method Describe() end Describe; "
        "end W;");
    BEZEL_EXPECT_TRUE(daHasError(r, "before non-nullable field"));
}

// ===========================================================================
// Rule 1 — the chaining position: any observation of `this` is rejected.
// §22.7 R-S1 traces 4 (this(...) entry) and 5 (base-arg escape).
// ===========================================================================

// REJECT: `this` passed in a `this(...)` chaining argument (trace 4).
BEZEL_TEST(sema_ctor_obs_2207, this_arg_in_this_chain_rejected) {
    auto r = parseAndCheckDA(
        "module M; "
        "class Node "
        "  private _v: int; "
        "  public constructor(v: int) "
        "    _v := v; "
        "  end constructor; "
        "  public constructor() "
        "    this(Helper(this)); "   // observation in chaining position
        "  end constructor; "
        "  public static method Helper(n: Node): int return 0; end Helper; "
        "end Node;");
    BEZEL_EXPECT_TRUE(daHasError(r, "chaining position"));
}

// REJECT: `this` escaping in a `base(...)` chaining argument (trace 5).
BEZEL_TEST(sema_ctor_obs_2207, this_arg_in_base_chain_rejected) {
    auto r = parseAndCheckDA(
        "module M; "
        "class Base "
        "  public constructor(o: Object) end constructor; "
        "end Base; "
        "class Derived: Base "
        "  private _name: string; "
        "  public constructor(n: string) "
        "    base(this); "          // observation in chaining position
        "    _name := n; "
        "  end constructor; "
        "end Derived;");
    BEZEL_EXPECT_TRUE(daHasError(r, "chaining position"));
}

// ACCEPT: a `this(...)` chain leaves every §7.9-strict field assigned on body
// entry (the chained-to constructor's obligation), so the body may observe.
BEZEL_TEST(sema_ctor_obs_2207, observe_after_this_chain_clean) {
    auto r = parseAndCheckDA(
        "module M; "
        "sealed class W "
        "  private _name: string; "
        "  public constructor(n: string) "
        "    _name := n; "
        "  end constructor; "
        "  public constructor() "
        "    this(\"d\"); "
        "    Describe(); "          // fields assigned by the chained-to ctor
        "  end constructor; "
        "  public method Describe() end Describe; "
        "end W;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// Rule 2 — no virtual dispatch from a non-sealed constructor body.
// The reported memory-unsafe accept (issue #1679, probe ctor_virtual.bzl):
// before the fix this compiled and segfaulted at runtime.
// ===========================================================================

// REJECT: a virtual call on `this` from a non-sealed base constructor — the
// runtime type may be a derived class whose field initializers have not run.
BEZEL_TEST(sema_ctor_obs_2207, virtual_call_in_nonsealed_ctor_rejected) {
    auto r = parseAndCheckDA(
        "module M; "
        "class Base "
        "  public constructor() "
        "    Describe(); "          // dispatches to a derived override
        "  end constructor; "
        "  public virtual method Describe() end Describe; "
        "end Base; "
        "class Derived: Base "
        "  private _name: string := \"x\"; "
        "  public override method Describe() end Describe; "
        "end Derived;");
    BEZEL_EXPECT_TRUE(daHasError(r, "non-sealed class"));
}

// ACCEPT: in a `sealed` (non-abstract) class the dispatch target is statically
// bounded (rule 2 exempt); the call is admitted under rule 1 once fields are set.
BEZEL_TEST(sema_ctor_obs_2207, override_call_in_sealed_ctor_clean) {
    auto r = parseAndCheckDA(
        "module M; "
        "abstract class Shape "
        "  public abstract method Area(): int; "
        "end Shape; "
        "sealed class Square: Shape "
        "  private _side: int; "
        "  public constructor(s: int) "
        "    _side := s; "
        "    var a := Area(); "     // dispatch statically bounded (sealed)
        "    _ := a; "
        "  end constructor; "
        "  public override method Area(): int return _side * _side; end Area; "
        "end Square;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// Rule 3 — closures observe at invocation, not at capture (NOT gated).
// ===========================================================================

// ACCEPT: a lambda that captures `this` (calls an instance method) BEFORE the
// non-nullable field is assigned is admitted — capture is not a synchronous
// observation (§22.7 rule 3).
BEZEL_TEST(sema_ctor_obs_2207, this_capturing_closure_not_gated_clean) {
    auto r = parseAndCheckDA(
        "module M; "
        "sealed class W "
        "  private _name: string; "
        "  public constructor() "
        "    var g := () => do Helper(); end; "  // captures `this`, not gated
        "    _ := g; "
        "    _name := \"x\"; "
        "  end constructor; "
        "  private method Helper() end Helper; "
        "end W;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ACCEPT: the §24.4 / stdlib §6.5 standard observer pattern (TickObserver). The
// constructor subscribes with a `this`-capturing lambda; the capture is not a
// synchronous observation, and the only field-store (`_sub := …`) is a direct
// store, not an observation. Must stay accepted (the required §22.7 regression).
// The DA harness does not run the ownership pass (§28.14 subscription
// admission), so we assert the §22.7-specific property: the constructor is not
// rejected by construction-time observation checking.
BEZEL_TEST(sema_ctor_obs_2207, tick_observer_subscription_not_gated) {
    auto r = parseAndCheckDA(
        "module M; "
        "record TickEvent(n: int); "
        "class TickObserver "
        "  private owned _sub: Subscription; "
        "  private _count: int := 0; "
        "  public constructor(ticks: IEvent<TickEvent>) "
        "    _sub := ticks.Subscribe((e) => do OnTick(e); end); "
        "  end constructor; "
        "  private method OnTick(e: TickEvent) "
        "    _count := _count + 1; "
        "  end OnTick; "
        "end TickObserver;");
    BEZEL_EXPECT_FALSE(daHasError(r, "22.7"));
}

// ===========================================================================
// fix #1753 — terminated-branch exclusion for FIELD definite-assignment
// (§22.7's observation gate). A branch that provably diverges (throw/return)
// reaches no join, so it must NOT shrink the intersection of assigned fields:
// after `if ok then this.f := … else throw end`, `f` IS assigned on every
// REACHING path and an observation past the `if` must be admitted. Before the
// fix the throwing branch (which assigned no field) was intersected in and the
// observation was over-rejected — the field analog of #1753's local case, and
// one of #1679's two known over-rejections. The exclusion must not
// under-reject: a LIVE branch that assigns no field still shrinks the
// intersection and the observation must still be gated.
// ===========================================================================

// ACCEPT — the else branch `throw`s, so `_name` is assigned on every reaching
// path before `Describe()`. Over-rejected before the fix.
BEZEL_TEST(sema_ctor_obs_2207, field_assigned_else_throws_observation_admitted_1753) {
    auto r = parseAndCheckDA(
        "module M; "
        "sealed class W "
        "  private _name: string; "
        "  public constructor(b: bool) "
        "    if b then _name := \"a\"; else throw new Exception(); end; "
        "    Describe(); "
        "  end constructor; "
        "  public method Describe() end Describe; "
        "end W;");
    BEZEL_EXPECT_FALSE(daHasError(r, "before non-nullable field"));
}

// ACCEPT — the else branch `return`s (a constructor early-return is a normal
// diverging exit); `_name` is assigned on every reaching path before Describe().
BEZEL_TEST(sema_ctor_obs_2207, field_assigned_else_returns_observation_admitted_1753) {
    auto r = parseAndCheckDA(
        "module M; "
        "sealed class W "
        "  private _name: string; "
        "  public constructor(b: bool) "
        "    if b then _name := \"a\"; else return; end; "
        "    Describe(); "
        "  end constructor; "
        "  public method Describe() end Describe; "
        "end W;");
    BEZEL_EXPECT_FALSE(daHasError(r, "before non-nullable field"));
}

// REJECT — under-reject guard: the else branch is LIVE (falls through) and
// assigns no field, so `_name` is NOT assigned on every reaching path; the
// observation must still be gated. The terminated-branch exclusion must not
// swallow this.
BEZEL_TEST(sema_ctor_obs_2207, field_else_live_without_assign_still_rejected_1753) {
    auto r = parseAndCheckDA(
        "module M; "
        "sealed class W "
        "  private _name: string; "
        "  public constructor(b: bool) "
        "    if b then _name := \"a\"; else _ := b; end; "
        "    Describe(); "
        "    _name := \"z\"; "
        "  end constructor; "
        "  public method Describe() end Describe; "
        "end W;");
    BEZEL_EXPECT_TRUE(daHasError(r, "before non-nullable field"));
}

// ACCEPT — the terminated-branch exclusion threaded through a statement-position
// `case` (fix #1681 + #1753 together): the `1` arm `throw`s (excluded), the `_`
// arm assigns `_name`, so on every reaching path `_name` is assigned before the
// observation. Before the fix the case was an inert no-op AND the throwing arm
// would have shrunk the field intersection.
BEZEL_TEST(sema_ctor_obs_2207, field_assigned_case_throwing_arm_excluded_admitted_1753) {
    auto r = parseAndCheckDA(
        "module M; "
        "sealed class W "
        "  private _name: string; "
        "  public constructor(n: int) "
        "    case n of "
        "      1 => do throw new Exception(); end, "
        "      _ => do _name := \"a\"; end "
        "    end; "
        "    Describe(); "
        "  end constructor; "
        "  public method Describe() end Describe; "
        "end W;");
    BEZEL_EXPECT_FALSE(daHasError(r, "before non-nullable field"));
}

// REJECT — under-reject guard for the `case` field join: a LIVE arm assigns no
// field, so the observation past the case must still be gated.
BEZEL_TEST(sema_ctor_obs_2207, field_case_live_arm_without_assign_still_rejected_1753) {
    auto r = parseAndCheckDA(
        "module M; "
        "sealed class W "
        "  private _name: string; "
        "  public constructor(n: int) "
        "    case n of "
        "      1 => do _ := n; end, "
        "      _ => do _name := \"a\"; end "
        "    end; "
        "    Describe(); "
        "    _name := \"z\"; "
        "  end constructor; "
        "  public method Describe() end Describe; "
        "end W;");
    BEZEL_EXPECT_TRUE(daHasError(r, "before non-nullable field"));
}
