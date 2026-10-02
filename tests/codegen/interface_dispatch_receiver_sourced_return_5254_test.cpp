// Regression tests for issue #5254 - §28.3 (spec v0.313): interface-dispatched
// receiver-sourced return over an owned param escapes the field-store check.
//
// The #5225 (d172667f) fix added a borrowSourceForExpr arm for concrete method
// calls whose every return is receiver-sourced.  For interface-dispatched calls,
// `calleeMethodDecl` yields the interface's abstract MethodDecl (no body), so
// `methodReturnsOnlyReceiverSourcedBorrows` returns false and the new arm does not
// fire.  The interface signature does not expose what an implementation returns,
// so conservatively treat a class/interface-typed non-owned return from a body-less
// callee as MAYBE receiver-sourced (source 1, `this`): apply the new arm when
// the callee is abstract and the return type is borrowable.
//
// Probes: /var/tmp/bezel-scratch/r25/rv5225/pe.bzl  (interface form, bug)
//         /var/tmp/bezel-scratch/r25/rv5225/pe2.bzl (class form, fixed by #5225)

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

const char* kInterfaceModule = R"BZL(
module M;
public interface IThing
  method Self(): IThing;
  method Tag(): int;
end IThing;
public class Impl: IThing
  public tag: int;
  public constructor(t: int) tag := t; end constructor;
  public method Self(): IThing return this; end Self;
  public method Tag(): int return tag; end Tag;
end Impl;
)BZL";

const char* kClassModule = R"BZL(
module M;
public class Base
  public tag: int;
  public constructor(t: int) tag := t; end constructor;
  public virtual method Self(): Base return this; end Self;
end Base;
public class Der: Base
  public constructor(t: int) base(t); end constructor;
  public override method Self(): Base return this; end Self;
end Der;
)BZL";

const char* kMain = R"BZL(
public class Program
  public static method Main(): int return 0; end Main;
end Program;
)BZL";

void expectReject(const std::string& fixture, const char* reason) {
    auto r = runAot(fixture + kMain);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, reason));
}

void expectAccept(const std::string& fixture) {
    auto r = runAot(fixture + kMain);
    BEZEL_EXPECT_TRUE(r.exitCode == 0);
}

}  // namespace

// ---- Interface form (bug #5254) ----

// Issue repro: interface-dispatched Self() returning this, stored to a field.
BEZEL_TEST(interface_dispatch_receiver_sourced_return_5254,
           interface_self_returns_this_to_owned_param_field_store_5254_spec_28_3) {
    expectReject(std::string(kInterfaceModule) + R"BZL(
public class B
  private _x: IThing?;
  public method SetV(owned value: IThing) _x := value.Self(); end SetV;
end B;
)BZL",
        "borrow of `value` stored into a field would outlive its source");
}

// Regression control: interface method returning owned IThing (non-borrow) → allowed.
// An `owned` return transfers ownership, not a borrow; the §28.3 field-store
// check does not apply.  This requires an interface method declared `owned Self()`
// — implemented by a concrete method that clones/duplicates.
BEZEL_TEST(interface_dispatch_receiver_sourced_return_5254,
           interface_owned_return_to_owned_param_field_allowed) {
    expectAccept(std::string(kInterfaceModule) + R"BZL(
public interface IOwnable
  method Clone(): owned IOwnable;
end IOwnable;
public class OwnableImpl: IOwnable
  public method Clone(): owned IOwnable return new OwnableImpl(0); end Clone;
end OwnableImpl;
public class B
  private _x: IOwnable?;
  public method Store(owned value: IOwnable) _x := value.Clone(); end Store;
end B;
)BZL");
}

// Control: interface Getter() returning a class type that is NOT receiver-sourced
// (returns a different object) → allowed.
BEZEL_TEST(interface_dispatch_receiver_sourced_return_5254,
           interface_getter_returns_class_type_not_receiver_sourced_allowed) {
    expectAccept(std::string(kInterfaceModule) + R"BZL(
public class Other
  private _x: IThing?;
  public method MakeOther(): IThing return new Impl(0); end MakeOther;
  public method Use() _x := MakeOther(); end Use;
end Other;
)BZL");
}

// Control: interface method returning a primitive (not borrowable) → allowed.
BEZEL_TEST(interface_dispatch_receiver_sourced_return_5254,
           interface_method_returns_primitive_allowed) {
    expectAccept(std::string(kInterfaceModule) + R"BZL(
public class B
  private _n: int;
  public method Take(owned value: IThing) _n := value.Tag(); end Take;
end B;
)BZL");
}

// Chain through interface: value.Self().Self() → still rejected.
BEZEL_TEST(interface_dispatch_receiver_sourced_return_5254,
           interface_chained_self_rejected) {
    expectReject(std::string(kInterfaceModule) + R"BZL(
public class B
  private _x: IThing?;
  public method SetV(owned value: IThing) _x := value.Self().Self(); end SetV;
end B;
)BZL",
        "borrow of `value` stored into a field would outlive its source");
}

// Control: owned local, not owned parameter (receiver-sourced only applies to
// parameter/local scope-boundness).
BEZEL_TEST(interface_dispatch_receiver_sourced_return_5254,
           interface_self_on_owned_local_allowed) {
    expectAccept(std::string(kInterfaceModule) + R"BZL(
public class B
  private _x: IThing?;
  public method Make(): IThing
    using var impl := new Impl(1);
    return impl.Self();
  end Make;
end B;
)BZL");
}

// Control: plain borrow parameter (not owned) → allowed (borrow of a borrow param
// is not scope-bound).
BEZEL_TEST(interface_dispatch_receiver_sourced_return_5254,
           interface_self_on_borrow_param_allowed) {
    expectAccept(std::string(kInterfaceModule) + R"BZL(
public class B
  private _x: IThing?;
  public method SetV(value: IThing) _x := value.Self(); end SetV;
end B;
)BZL");
}

// ---- Class form (control: #5225 must still work) ----

// Class virtual Self() form — already fixed by #5225; verify no regression.
BEZEL_TEST(interface_dispatch_receiver_sourced_return_5254,
           class_self_returns_this_to_owned_param_field_store_5254_spec_28_3) {
    expectReject(std::string(kClassModule) + R"BZL(
public class B
  private _x: Base?;
  public method SetV(owned value: Base) _x := value.Self(); end SetV;
end B;
)BZL",
        "borrow of `value` stored into a field would outlive its source");
}

// Class Self() on a plain borrow param — control.
BEZEL_TEST(interface_dispatch_receiver_sourced_return_5254,
           class_self_on_borrow_param_allowed) {
    expectAccept(std::string(kClassModule) + R"BZL(
public class B
  private _x: Base?;
  public method SetV(value: Base) _x := value.Self(); end SetV;
end B;
)BZL");
}
