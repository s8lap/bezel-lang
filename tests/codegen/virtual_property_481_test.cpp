// Bug #481 — virtual/abstract/override (and interface) property accessors must
// dispatch through the vtable/itable, exactly like virtual methods (§12.1 /
// §12.5 / §23). Before the fix a property accessor was bound STATICALLY: a
// `virtual` property accessed through a base reference returned the base value
// (miscompile), an `abstract` property invoked from a base method crashed (no
// vtable slot), and an interface property accessed through an interface
// reference returned 0 / crashed. A non-virtual property keeps its direct call.
// Observable via the int exit code (string cases compare in-language and return
// 1/0 to prove the right accessor ran).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// 1. virtual property through a Base-typed parameter holding a Derived object →
//    the DERIVED getter runs (§12.1). This is the headline miscompile.
BEZEL_TEST(codegen_bug481, virtual_property_dispatched_on_derived) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base public virtual property Num: int => 1; "
        "  destructor end destructor; end Base; "
        "class Derived : Base public override property Num: int => 2; "
        "  destructor end destructor; end Derived; "
        "class C "
        "  public static method Get(b: Base): int => b.Num; "
        "  public static method Main(): int "
        "    using var d := new Derived(); return Get(d); "
        "  end Main; "
        "end C;"), 2);
}

// 1b. The same Base-typed parameter, given a Base object, runs the BASE getter
//     (no override in play).
BEZEL_TEST(codegen_bug481, virtual_property_base_object_runs_base) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base public virtual property Num: int => 1; "
        "  destructor end destructor; end Base; "
        "class Derived : Base public override property Num: int => 2; "
        "  destructor end destructor; end Derived; "
        "class C "
        "  public static method Get(b: Base): int => b.Num; "
        "  public static method Main(): int "
        "    using var b := new Base(); return Get(b); "
        "  end Main; "
        "end C;"), 1);
}

// 2. abstract property invoked through a base method (implicit `this`) — must
//    dispatch to the override (§12.5). Before the fix this SIGSEGV'd because the
//    abstract property had no vtable slot. String result compared in-language.
BEZEL_TEST(codegen_bug481, abstract_property_via_base_method) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "abstract class Base "
        "  public abstract property Name: string; "
        "  public method GetName(): string => Name; "
        "end Base; "
        "class Child : Base public override property Name: string => \"ChildName\"; "
        "  destructor end destructor; end Child; "
        "class C "
        "  public static method Main(): int "
        "    using var c := new Child(); "
        "    var n := c.GetName(); "
        "    return (1 if n = \"ChildName\" else 0); "
        "  end Main; "
        "end C;"), 1);
}

// 3. abstract/virtual property accessed directly through a base-typed reference
//    (`b.Name`) → the override's value (§12.5). int variant.
BEZEL_TEST(codegen_bug481, abstract_property_via_base_reference_int) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "abstract class Base "
        "  public abstract property Code: int; "
        "end Base; "
        "class Child : Base public override property Code: int => 42; "
        "  destructor end destructor; end Child; "
        "class C "
        "  public static method CodeOf(b: Base): int => b.Code; "
        "  public static method Main(): int "
        "    using var c := new Child(); return CodeOf(c); "
        "  end Main; "
        "end C;"), 42);
}

// 4a. interface property accessed through an interface reference → the
//     implementing class's value (§13 / §23). int — was 0 before the fix.
BEZEL_TEST(codegen_bug481, interface_property_int) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IShape property Size: int; end IShape; "
        "class Square : IShape public property Size: int => 9; "
        "  destructor end destructor; end Square; "
        "class C "
        "  public static method SizeOf(s: IShape): int => s.Size; "
        "  public static method Main(): int "
        "    using var sq := new Square(); return SizeOf(sq); "
        "  end Main; "
        "end C;"), 9);
}

// 4b. interface property returning string accessed through an interface
//     reference — was a SIGSEGV before the fix.
BEZEL_TEST(codegen_bug481, interface_property_string) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface INamed property Name: string; end INamed; "
        "class Widget : INamed public property Name: string => \"W\"; "
        "  destructor end destructor; end Widget; "
        "class C "
        "  public static method NameOf(n: INamed): int "
        "    var s := n.Name; return (1 if s = \"W\" else 0); "
        "  end NameOf; "
        "  public static method Main(): int "
        "    using var w := new Widget(); return NameOf(w); "
        "  end Main; "
        "end C;"), 1);
}

// 5. virtual property with a SETTER overridden in the derived class: setting
//    through a base reference runs the DERIVED setter (§12.1). Base's setter
//    writes _v; Derived's writes _w := value*2. Through a Base ref on a Derived
//    object, the override must run, so _w becomes 10 and _v stays 0 → 100.
BEZEL_TEST(codegen_bug481, virtual_property_setter_override) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base "
        "  protected _v: int := 0; "
        "  public virtual property P: int get => _v; set => do _v := value; end; end P; "
        "  public method ReadV(): int => _v; "
        "end Base; "
        "class Derived : Base "
        "  protected _w: int := 0; "
        "  public override property P: int get => _w; set => do _w := value * 2; end; end P; "
        "  public method ReadW(): int => _w; "
        "end Derived; "
        "class C "
        "  public static method SetIt(b: Base) b.P := 5; end SetIt; "
        "  public static method Main(): int "
        "    using var d := new Derived(); SetIt(d); "
        "    return d.ReadW() * 10 + d.ReadV(); "
        "  end Main; "
        "end C;"), 100);
}

// 6. deep chain (A virtual, B override, D override) dispatched via an A-typed
//    reference → D's value (§12.1).
BEZEL_TEST(codegen_bug481, deep_chain_virtual_property) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class A public virtual property V: int => 1; "
        "  destructor end destructor; end A; "
        "class B : A public override property V: int => 2; "
        "  destructor end destructor; end B; "
        "class D : B public override property V: int => 3; "
        "  destructor end destructor; end D; "
        "class C "
        "  public static method Via(a: A): int => a.V; "
        "  public static method Main(): int "
        "    using var d := new D(); return Via(d); "
        "  end Main; "
        "end C;"), 3);
}

// 6b. middle of the chain not overriding: A virtual, B does NOT override,
//     dispatched via A ref on a B object → A's value (the inherited slot).
BEZEL_TEST(codegen_bug481, chain_non_overriding_middle_inherits) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class A public virtual property V: int => 7; "
        "  destructor end destructor; end A; "
        "class B : A "
        "  public method Tag(): int => 0; "
        "  destructor end destructor; end B; "
        "class C "
        "  public static method Via(a: A): int => a.V; "
        "  public static method Main(): int "
        "    using var b := new B(); return Via(b); "
        "  end Main; "
        "end C;"), 7);
}

// 7a. Regression: a NON-virtual property still works via a direct (static) call.
BEZEL_TEST(codegen_bug481, non_virtual_property_still_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public property Value: int => 5; "
        "  public static method Main(): int "
        "    var c := new C(); return c.Value; "
        "  end Main; "
        "end C;"), 5);
}

// 7b. Regression: a virtual METHOD still dispatches correctly alongside a
//     virtual property on the same class (the layout change must not perturb
//     method slots). Method override → 20, property override → 2; 20*10+2=202.
BEZEL_TEST(codegen_bug481, virtual_method_alongside_virtual_property) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base "
        "  public virtual method M(): int => 10; "
        "  public virtual property P: int => 1; "
        "end Base; "
        "class Derived : Base "
        "  public override method M(): int => 20; "
        "  public override property P: int => 2; "
        "  destructor end destructor; "
        "end Derived; "
        "class C "
        "  public static method Use(b: Base): int => b.M() * 10 + b.P; "
        "  public static method Main(): int "
        "    using var d := new Derived(); return Use(d); "
        "  end Main; "
        "end C;"), 202);
}

// 8. virtual AUTO-property override: both get and set dispatch dynamically and
//    read/write the most-derived backing field (§23.1 / §12.1). Read 9, write
//    50, read 50 → 9*100+50 = 950.
BEZEL_TEST(codegen_bug481, virtual_auto_property_get_set) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class Base "
        "  public virtual property Tag: int := 1; "
        "end Base; "
        "class Derived : Base "
        "  public override property Tag: int := 9; "
        "  destructor end destructor; "
        "end Derived; "
        "class C "
        "  public static method Read(b: Base): int => b.Tag; "
        "  public static method Write(b: Base) b.Tag := 50; end Write; "
        "  public static method Main(): int "
        "    using var d := new Derived(); "
        "    var before := Read(d); Write(d); "
        "    Console.WriteLine(before * 100 + Read(d)); return 0; "
        "  end Main; "
        "end C;"), "950");
}

// 9. interface read-write property: the SETTER also dispatches through the itab
//    (§13 / §23). Bump reads (gets 0) + 100, sets through the interface ref;
//    reading back via the class returns 100.
BEZEL_TEST(codegen_bug481, interface_property_setter) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface ICounter property Count: int; end ICounter; "
        "class Box : ICounter "
        "  protected _c: int := 0; "
        "  public property Count: int get => _c; set => do _c := value; end; end Count; "
        "end Box; "
        "class C "
        "  public static method Bump(c: ICounter) c.Count := c.Count + 100; end Bump; "
        "  public static method Main(): int "
        "    using var b := new Box(); Bump(b); return b.Count; "
        "  end Main; "
        "end C;"), 100);
}

// 10. abstract property invoked through an INTERFACE-typed reference where the
//     interface property is satisfied by an override in the concrete class via a
//     base abstract class — the itab accessor must resolve the most-derived
//     implementation (§12.5 / §13).
BEZEL_TEST(codegen_bug481, interface_property_via_abstract_base) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface ISized property Size: int; end ISized; "
        "abstract class Shape : ISized "
        "  public abstract property Size: int; "
        "end Shape; "
        "class Tile : Shape "
        "  public override property Size: int => 16; "
        "  destructor end destructor; "
        "end Tile; "
        "class C "
        "  public static method SizeOf(s: ISized): int => s.Size; "
        "  public static method Main(): int "
        "    using var t := new Tile(); return SizeOf(t); "
        "  end Main; "
        "end C;"), 16);
}

