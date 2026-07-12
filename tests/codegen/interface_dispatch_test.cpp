// Codegen slice CG35-itable — per-interface itable dispatch (§12 / §13).
// A class's vtable[1] = itmap$C (null-terminated [td$I, itab$C$I, …]).
// Dispatch through an interface-typed value calls bz_itable_lookup to find
// the itable, then GEPs to the method's position within it.
// Observable via the int exit code.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg35, call_via_interface_typed_parameter) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IShape method Area(): int; end IShape; "
        "class Square : IShape public method Area(): int => 16; end Square; "
        "class C "
        "  public static method AreaOf(s: IShape): int => s.Area(); "
        "  public static method Main(): int "
        "    var sq := new Square(); return AreaOf(sq); "
        "  end Main; "
        "end C;"), 16);
}

BEZEL_TEST(codegen_cg35, polymorphism_two_implementers) {
    // Two classes implement IShape differently; calling through the
    // interface dispatches to each one's method.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IShape method Area(): int; end IShape; "
        "class Square : IShape public method Area(): int => 4; end Square; "
        "class Big : IShape public method Area(): int => 100; end Big; "
        "class C "
        "  public static method AreaOf(s: IShape): int => s.Area(); "
        "  public static method Main(): int "
        "    var a := new Square(); var b := new Big(); "
        "    return AreaOf(a) + AreaOf(b); "
        "  end Main; "
        "end C;"), 104);
}

BEZEL_TEST(codegen_cg35, call_via_interface_local) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IShape method Area(): int; end IShape; "
        "class Square : IShape public method Area(): int => 25; end Square; "
        "class C public static method Main(): int "
        "  var s: IShape := new Square(); return s.Area(); "
        "end Main; end C;"), 25);
}

BEZEL_TEST(codegen_cg35, interface_with_argument) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IAdder method Add(n: int): int; end IAdder; "
        "class Plus10 : IAdder "
        "  public method Add(n: int): int => n + 10; end Plus10; "
        "class C "
        "  public static method Use(a: IAdder): int => a.Add(32); "
        "  public static method Main(): int "
        "    var p := new Plus10(); return Use(p); "
        "  end Main; "
        "end C;"), 42);
}

BEZEL_TEST(codegen_cg35, two_interface_methods) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IPair method First(): int; method Second(): int; "
        "end IPair; "
        "class P : IPair "
        "  public method First(): int => 40; "
        "  public method Second(): int => 2; "
        "end P; "
        "class C "
        "  public static method Sum(p: IPair): int => p.First() + p.Second(); "
        "  public static method Main(): int "
        "    var p := new P(); return Sum(p); "
        "  end Main; "
        "end C;"), 42);
}

// CG35-itable new tests: verify per-interface itable dispatch (§12 / §13).

BEZEL_TEST(codegen_cg35_itable, class_two_interfaces_dispatch_each) {
    // §13: a class implementing two unrelated interfaces; dispatch through
    // each interface reaches the correct implementing method. Exercises two
    // separate itabs in itmap$C.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IFoo method FooVal(): int; end IFoo; "
        "interface IBar method BarVal(): int; end IBar; "
        "class Impl : IFoo, IBar "
        "  public method FooVal(): int => 7; "
        "  public method BarVal(): int => 11; "
        "end Impl; "
        "class C "
        "  public static method GetFoo(x: IFoo): int => x.FooVal(); "
        "  public static method GetBar(x: IBar): int => x.BarVal(); "
        "  public static method Main(): int "
        "    var o := new Impl(); return GetFoo(o) + GetBar(o); "
        "  end Main; "
        "end C;"), 18);
}

BEZEL_TEST(codegen_cg35_itable, interface_inheritance_dispatch_both) {
    // §13 / §12: interface I2 extends I1; a class implementing I2 must be
    // dispatchable through both I1 and I2 (separate itabs in itmap).
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IBase method BaseVal(): int; end IBase; "
        "interface IDerived : IBase method DerivedVal(): int; end IDerived; "
        "class Both : IDerived "
        "  public method BaseVal(): int => 3; "
        "  public method DerivedVal(): int => 5; "
        "end Both; "
        "class C "
        "  public static method ViaBase(x: IBase): int => x.BaseVal(); "
        "  public static method ViaDerived(x: IDerived): int => x.DerivedVal(); "
        "  public static method Main(): int "
        "    var o := new Both(); return ViaBase(o) + ViaDerived(o); "
        "  end Main; "
        "end C;"), 8);
}

BEZEL_TEST(codegen_cg35_itable, record_interface_box_itable) {
    // §8.6 / §9.6 / §13: a record boxed to an interface uses the box itable.
    // Dispatch via the interface on the boxed record must reach the record's
    // method (via the box thunk that adjusts `this` past vtable+rc words).
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IArea method Area(): int; end IArea; "
        "record Rect(w: int, h: int): IArea "
        "  public method Area(): int => w * h; "
        "end Rect; "
        "class C "
        "  public static method Measure(s: IArea): int => s.Area(); "
        "  public static method Main(): int "
        "    var r := new Rect(3, 4); return Measure(r); "
        "  end Main; "
        "end C;"), 12);
}

BEZEL_TEST(codegen_cg35_itable, is_interface_check_still_works) {
    // §19.5 / §12.7: `x is I` must still work after the itable ABI change.
    // `is` uses td$ (vtable[0]) unchanged — this test guards the is/as path.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IShape method Area(): int; end IShape; "
        "class Square : IShape public method Area(): int => 9; end Square; "
        "class C public static method Main(): int "
        "  var s: IShape := new Square(); "
        "  return (1 if s is Square else 0); "
        "end Main; end C;"), 1);
}

// #742 / §11.2 / §13: when the implementing class has MULTIPLE overloads of the
// interface method name, interface dispatch must bind the overload whose
// signature matches the interface member — not the first by name. Previously
// the itable slot was filled by name only, so `i.Run(5)` dispatched the
// arity-0 `Run()` (returning 0, ignoring the argument).
BEZEL_TEST(codegen_cg35_itable, overloaded_impl_binds_matching_arity_742) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IGo method Run(x: int): int; end IGo; "
        "class Impl : IGo "
        "  public method Run(): int return 0; end Run; "
        "  public method Run(x: int): int return x * 2; end Run; "
        "end Impl; "
        "class C public static method Main(): int "
        "  using var im: Impl := new Impl(); var i: IGo := im; "
        "  return i.Run(5); "  // 10, not 0
        "end Main; end C;"), 10);
}

// Order-independent: the matching overload is bound even when it is declared
// before the non-matching one.
BEZEL_TEST(codegen_cg35_itable, overloaded_impl_binds_matching_arity_reversed_742) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IGo method Run(x: int): int; end IGo; "
        "class Impl : IGo "
        "  public method Run(x: int): int return x * 2; end Run; "
        "  public method Run(): int return 99; end Run; "
        "end Impl; "
        "class C public static method Main(): int "
        "  using var im: Impl := new Impl(); var i: IGo := im; "
        "  return i.Run(5); "  // 10, not 99
        "end Main; end C;"), 10);
}

// fix #1139 / §11.2 rule-1: an implementing class may declare a parameter as
// T? where the interface requires T (nullability widening).  Interface dispatch
// must bridge the ABI difference ({i8,T} vs T) via an adapter thunk so the
// value reaches the impl body correctly — without it the body reads the
// hasValue tag (1) as the payload, giving a constant wrong result.
BEZEL_TEST(codegen_cg35_itable, nullable_widening_param_dispatch_1139) {
    // interface declares int, impl declares int? (§11.2 rule-1).
    // ip.Handle(42) must return 42, not 1 (the non-null tag).
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IProc method Handle(x: int): int; end IProc; "
        "class P: IProc "
        "  public method Handle(x: int?): int => x ?? 99; "
        "end P; "
        "class App "
        "  public static method Main(): int "
        "    using var p := new P(); var ip: IProc := p; "
        "    return ip.Handle(42); "
        "  end Main; "
        "end App;"), 42);
}

// Verify that ip.Handle(0) returns 0 (has value = 0, not null) and
// ip.Handle(7) returns 7 — the adapter forwards the actual value, not a tag.
BEZEL_TEST(codegen_cg35_itable, nullable_widening_param_various_values_1139) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IProc method Handle(x: int): int; end IProc; "
        "class P: IProc "
        "  public method Handle(x: int?): int => x ?? 99; "
        "end P; "
        "class App "
        "  public static method Main(): int "
        "    using var p := new P(); var ip: IProc := p; "
        "    var r1 := ip.Handle(7); "    // 7
        "    var r2 := ip.Handle(0); "    // 0  (has value, value=0)
        "    var r3 := ip.Handle(35); "   // 35
        "    return r1 + r2 + r3; "        // 42
        "  end Main; "
        "end App;"), 42);
}

// fix #1613 / §11.2 (interface nullability-widening on parameters, "an
// implementation may declare a parameter as T? where the interface requires
// T"): the same adapter thunk from #1139/#1183 above (ensureNullableWideningThunk)
// must also handle a VOID-returning interface method. The thunk's forwarding
// call was unconditionally named ("nw.ret"), and naming a void-typed Value
// trips the LLVM IR verifier ("Instruction has a name, but provides a void
// value!") — reproduced pre-fix via build/bezelc on a program shaped exactly
// like this one. ip.Handle(42) must land 42 (not the has-value tag 1) into
// the static field via the impl's widened int? parameter.
BEZEL_TEST(codegen_cg35_itable, nullable_widening_param_void_return_1613) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IProc method Handle(x: int); end IProc; "
        "class P: IProc "
        "  public static _last: int := 0; "
        "  public method Handle(x: int?) _last := x ?? -1; end Handle; "
        "end P; "
        "class App "
        "  public static method Main(): int "
        "    using var p := new P(); var ip: IProc := p; "
        "    ip.Handle(42); "
        "    return P._last; "
        "  end Main; "
        "end App;"), 42);
}
