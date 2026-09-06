// Bug #510 — virtual/abstract/override and interface property accessors that
// return non-trivial types (enums, nullable value types) were miscompiled by
// the #481 accessor-wrapper path:
//
// Bug2 (HIGH wrong-result): a virtual/abstract/override/interface property
// returning an ENUM returned 0 instead of the accessor body's value.
// Root cause: ensurePropertyAccessorFn is called during declareVtables but
// computeEnumValues runs AFTER declareVtables, so enumValue_ is empty when the
// arrow-body `=> Enum.Member` is lowered; enumConstant() returns nullptr and
// the body emits `ret i32 0`.
//
// Bug3 (CRITICAL IR-fail): a property returning a NULLABLE value type (int?,
// bool?) emitted invalid IR: the accessor body returned the raw non-nullable
// value (i1, or ptr null) into a function declared to return the nullable
// tagged struct { i8, T }, causing LLVM IR verification failure.
// Root cause: lowerExpr(true) = i1, but the function return type is { i8, i1 };
// the ret instruction had a type mismatch. Similarly `null` returned ptr null
// when the function expected { i8, i32 }.
//
// Fix:
//   (1) Move computeEnumValues() before declareVtables() so enumValue_ is
//       populated when accessor bodies are emitted — fixes Bug2.
//   (2) In ensurePropertyAccessorFn, after lowerExpr, if the function return
//       type is a nullable struct and the value is the raw inner type (or a
//       null pointer for the `null` literal), wrap it into the struct —
//       fixes Bug3.
//
// per §12.1 / §12.5 / §13 / §23 / §7 (nullable value types)

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ---------------------------------------------------------------------------
// Bug2: enum-returning virtual/abstract/override/interface properties
// ---------------------------------------------------------------------------

// 1a. virtual property returning an int-backed enum dispatches to the correct
//     member through a base-typed reference (§12.1).
// per §12.1 / §10 / #510 Bug2
BEZEL_TEST(property_accessor_types_510, virtual_enum_property_override_int_backed) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Priority Low, Medium, High; end Priority; "
        "class Task "
        "  public virtual property Urgency: Priority => Priority.Low; "
        "  destructor end destructor; "
        "end Task; "
        "class UrgentTask : Task "
        "  public override property Urgency: Priority => Priority.High; "
        "  destructor end destructor; "
        "end UrgentTask; "
        "class C "
        "  public static method GetUrgency(t: Task): int => int(t.Urgency); "
        "  public static method Main(): int "
        "    using var t := new UrgentTask(); return GetUrgency(t); "
        "  end Main; "
        "end C;"), 2);  // Priority.High == 2
}

// 1b. Base-typed reference on a base object runs the BASE getter (§12.1).
// per §12.1 / §10 / #510 Bug2 control
BEZEL_TEST(property_accessor_types_510, virtual_enum_property_base_object) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Priority Low, Medium, High; end Priority; "
        "class Task "
        "  public virtual property Urgency: Priority => Priority.Medium; "
        "  destructor end destructor; "
        "end Task; "
        "class C "
        "  public static method GetUrgency(t: Task): int => int(t.Urgency); "
        "  public static method Main(): int "
        "    using var t := new Task(); return GetUrgency(t); "
        "  end Main; "
        "end C;"), 1);  // Priority.Medium == 1
}

// 2. abstract property returning an enum dispatched through a base method
//    (implicit `this`) → the override's value (§12.5 / #510 Bug2).
// per §12.5 / §10 / #510 Bug2
BEZEL_TEST(property_accessor_types_510, abstract_enum_property_via_base_method) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "abstract class Shape "
        "  public abstract property Hue: Color; "
        "  public method HueInt(): int => int(Hue); "
        "end Shape; "
        "class Circle : Shape "
        "  public override property Hue: Color => Color.Blue; "
        "  destructor end destructor; "
        "end Circle; "
        "class C "
        "  public static method Main(): int "
        "    using var c := new Circle(); return c.HueInt(); "
        "  end Main; "
        "end C;"), 2);  // Color.Blue == 2
}

// 3. abstract property returning enum accessed directly via a base-typed ref
//    (§12.5 / #510 Bug2).
// per §12.5 / §10 / #510 Bug2
BEZEL_TEST(property_accessor_types_510, abstract_enum_property_via_base_ref) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Status Pending, Active, Closed; end Status; "
        "abstract class Item "
        "  public abstract property State: Status; "
        "end Item; "
        "class ActiveItem : Item "
        "  public override property State: Status => Status.Active; "
        "  destructor end destructor; "
        "end ActiveItem; "
        "class C "
        "  public static method GetState(i: Item): int => int(i.State); "
        "  public static method Main(): int "
        "    using var a := new ActiveItem(); return GetState(a); "
        "  end Main; "
        "end C;"), 1);  // Status.Active == 1
}

// 4. interface property returning an enum dispatched through an interface ref
//    (§13 / §23 / #510 Bug2).
// per §13 / §10 / #510 Bug2
BEZEL_TEST(property_accessor_types_510, interface_enum_property) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Level Low, Mid, High; end Level; "
        // §11.2/§23.3 (#2413): HighLevel's `L` is getter-only and this test
        // never writes through the interface, so ILeveled's obligation is
        // declared `readonly` — a getter-only implementation of a
        // read-write interface property is now a compile error (#2413).
        "interface ILeveled readonly property L: Level; end ILeveled; "
        "class HighLevel : ILeveled "
        "  public property L: Level => Level.High; "
        "  destructor end destructor; "
        "end HighLevel; "
        "class C "
        "  public static method GetL(x: ILeveled): int => int(x.L); "
        "  public static method Main(): int "
        "    using var h := new HighLevel(); return GetL(h); "
        "  end Main; "
        "end C;"), 2);  // Level.High == 2
}

// 4b. Interface property enum returning Mid (value 1) — not zero — to prove
//     that non-zero enum values are also correct through itable dispatch.
// per §13 / §10 / #510 Bug2
BEZEL_TEST(property_accessor_types_510, interface_enum_property_mid_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Priority Low, Medium, High; end Priority; "
        // §11.2/§23.3 (#2413): MedTask's `Urgency` is getter-only and this
        // test never writes through the interface (see the identical note
        // on interface_enum_property above).
        "interface ITask readonly property Urgency: Priority; end ITask; "
        "class MedTask : ITask "
        "  public property Urgency: Priority => Priority.Medium; "
        "  destructor end destructor; "
        "end MedTask; "
        "class C "
        "  public static method Get(t: ITask): int => int(t.Urgency); "
        "  public static method Main(): int "
        "    using var m := new MedTask(); return Get(m); "
        "  end Main; "
        "end C;"), 1);  // Priority.Medium == 1
}

// ---------------------------------------------------------------------------
// Bug3: nullable value-type returning virtual/interface property accessors
// ---------------------------------------------------------------------------

// 5a. virtual property returning int? with value dispatched through base ref
//     (§12.1 / §7 / #510 Bug3) — before the fix this caused an IR-fail.
// per §12.1 / §7 / #510 Bug3
BEZEL_TEST(property_accessor_types_510, virtual_nullable_int_property_has_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "abstract class Base "
        "  public abstract property MaybeVal: int?; "
        "end Base; "
        "class ValueReturner : Base "
        "  public override property MaybeVal: int? => 42; "
        "  destructor end destructor; "
        "end ValueReturner; "
        "class C "
        "  public static method Get(b: Base): int "
        "    var v := b.MaybeVal; "
        "    return (if v <> null then v! else -1); "
        "  end Get; "
        "  public static method Main(): int "
        "    using var r := new ValueReturner(); return Get(r); "
        "  end Main; "
        "end C;"), 42);
}

// 5b. virtual property returning int? with null dispatched through base ref
//     (§12.1 / §7 / #510 Bug3) — before the fix this caused an IR-fail.
// per §12.1 / §7 / #510 Bug3
BEZEL_TEST(property_accessor_types_510, virtual_nullable_int_property_returns_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "abstract class Base "
        "  public abstract property MaybeVal: int?; "
        "end Base; "
        "class NullReturner : Base "
        "  public override property MaybeVal: int? => null; "
        "  destructor end destructor; "
        "end NullReturner; "
        "class C "
        "  public static method Get(b: Base): int "
        "    var v := b.MaybeVal; "
        "    return (if v = null then 1 else 0); "
        "  end Get; "
        "  public static method Main(): int "
        "    using var r := new NullReturner(); return Get(r); "
        "  end Main; "
        "end C;"), 1);
}

// 5c. virtual property returning bool? returning true dispatched through base
//     ref (§12.1 / §7 / #510 Bug3) — before the fix: `ret i1 true` into
//     `{ i8, i1 }` → IR verification failure.
// per §12.1 / §7 / #510 Bug3
BEZEL_TEST(property_accessor_types_510, virtual_nullable_bool_property_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "abstract class Base "
        "  public abstract property Flag: bool?; "
        "end Base; "
        "class TrueReturner : Base "
        "  public override property Flag: bool? => true; "
        "  destructor end destructor; "
        "end TrueReturner; "
        "class C "
        "  public static method Get(b: Base): int "
        "    var f := b.Flag; "
        "    return (if (f <> null and f!) then 1 else 0); "
        "  end Get; "
        "  public static method Main(): int "
        "    using var r := new TrueReturner(); return Get(r); "
        "  end Main; "
        "end C;"), 1);
}

// 5d. virtual property returning bool? returning null dispatched through base
//     ref (§12.1 / §7 / #510 Bug3).
// per §12.1 / §7 / #510 Bug3
BEZEL_TEST(property_accessor_types_510, virtual_nullable_bool_property_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "abstract class Base "
        "  public abstract property Flag: bool?; "
        "end Base; "
        "class NullReturner : Base "
        "  public override property Flag: bool? => null; "
        "  destructor end destructor; "
        "end NullReturner; "
        "class C "
        "  public static method Get(b: Base): int "
        "    var f := b.Flag; "
        "    return (if f = null then 1 else 0); "
        "  end Get; "
        "  public static method Main(): int "
        "    using var r := new NullReturner(); return Get(r); "
        "  end Main; "
        "end C;"), 1);
}

// 6. interface property returning int? through interface ref (§13 / §7 / #510 Bug3).
// per §13 / §7 / #510 Bug3
BEZEL_TEST(property_accessor_types_510, interface_nullable_int_property) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        // §11.2/§23.3 (#2413): HasValue's `Val` is getter-only and this test
        // never writes through the interface (see the identical note on
        // interface_enum_property above).
        "interface IMaybeValue readonly property Val: int?; end IMaybeValue; "
        "class HasValue : IMaybeValue "
        "  public property Val: int? => 7; "
        "  destructor end destructor; "
        "end HasValue; "
        "class C "
        "  public static method Get(x: IMaybeValue): int "
        "    var v := x.Val; "
        "    return (if v <> null then v! else -1); "
        "  end Get; "
        "  public static method Main(): int "
        "    using var h := new HasValue(); return Get(h); "
        "  end Main; "
        "end C;"), 7);
}

// ---------------------------------------------------------------------------
// Controls: the #481 cases must still pass (regression guard)
// ---------------------------------------------------------------------------

// C1. virtual int property dispatch still correct (§12.1 / #481 control).
BEZEL_TEST(property_accessor_types_510, control_virtual_int_property) {
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
        "  end Main; end C;"), 2);
}

// C2. abstract property returning string through base ref (§12.5 / #481 control).
BEZEL_TEST(property_accessor_types_510, control_abstract_string_property) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "abstract class Base "
        "  public abstract property Name: string; "
        "end Base; "
        "class Child : Base public override property Name: string => \"ok\"; "
        "  destructor end destructor; end Child; "
        "class C "
        "  public static method Main(): int "
        "    using var c := new Child(); "
        "    var n := (c as Base)!.Name; "
        "    return (if n = \"ok\" then 1 else 0); "
        "  end Main; end C;"), 1);
}

// C3. interface property returning int through interface ref (§13 / #481 control).
BEZEL_TEST(property_accessor_types_510, control_interface_int_property) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        // §11.2/§23.3 (#2413): Square's `Size` is getter-only and this test
        // never writes through the interface (see the identical note on
        // interface_enum_property above).
        "interface IShape readonly property Size: int; end IShape; "
        "class Square : IShape public property Size: int => 9; "
        "  destructor end destructor; end Square; "
        "class C "
        "  public static method SizeOf(s: IShape): int => s.Size; "
        "  public static method Main(): int "
        "    using var sq := new Square(); return SizeOf(sq); "
        "  end Main; end C;"), 9);
}

// ---------------------------------------------------------------------------
// #1168: the NON-virtual, inline expression-bodied property getter path
// (LlvmBackend_Exprs.cpp CG33b) is a sibling of Bug3 above. A `T?`-declared
// expression-bodied property whose body yields the bare inner `T` (a value-type
// field read) was returned UN-wrapped — codegen never promoted the result to
// the declared `{i8 hasValue; T}` optional. Unwrapping the read (`??`, `!`)
// then did CreateExtractValue on a bare `i32`, asserting in LLVM (checkGEPType).
// per §23.2 / §23.3 / §7 (#1168)
// ---------------------------------------------------------------------------

// 7a. THE REPRO: `obj.P ?? d` on a value-type field-backed `int?` getter wraps
//     the bare field value and coalesces to the present value.
// per §23.2 / §23.3 / §7 (#1168)
BEZEL_TEST(property_accessor_types_510, nullable_property_getter_wraps_value_1168) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Holder "
        "  private _x: int; "
        "  public constructor(x: int) _x := x; end constructor; "
        "  public property Maybe: int? => _x; "
        "  destructor end destructor; "
        "end Holder; "
        "class P public static method Main(): int "
        "  using var h := new Holder(7); "
        "  return h.Maybe ?? 0; "
        "end Main; end P;"), 7);
}

// 7b. force-unwrap (`!`) of the same getter — the wrapped value carries
//     hasValue=1 so the unwrap yields the inner value (§7.5).
// per §23.2 / §23.3 / §7.5 (#1168)
BEZEL_TEST(property_accessor_types_510, nullable_property_getter_force_unwrap_1168) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Holder "
        "  private _x: int; "
        "  public constructor(x: int) _x := x; end constructor; "
        "  public property Maybe: int? => _x; "
        "  destructor end destructor; "
        "end Holder; "
        "class P public static method Main(): int "
        "  using var h := new Holder(11); "
        "  return h.Maybe!; "
        "end Main; end P;"), 11);
}

// 7c. accessor-block expression-body getter (`get => _x;`) goes through the
//     same inline CG33b path and must wrap identically.
// per §23.2 / §23.3 / §7 (#1168)
BEZEL_TEST(property_accessor_types_510, nullable_accessor_expr_getter_wraps_1168) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Holder "
        "  private _x: int; "
        "  public constructor(x: int) _x := x; end constructor; "
        "  public property Maybe: int? get => _x; end; "
        "  destructor end destructor; "
        "end Holder; "
        "class P public static method Main(): int "
        "  using var h := new Holder(5); "
        "  return h.Maybe ?? 0; "
        "end Main; end P;"), 5);
}

// 7d. bare implicit-`this` read of the getter from another method (the
//     IdentifierExpr CG33b path) must wrap the same way.
// per §23.2 / §23.3 / §7 (#1168)
BEZEL_TEST(property_accessor_types_510, nullable_property_getter_bare_read_1168) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Holder "
        "  private _x: int; "
        "  public constructor(x: int) _x := x; end constructor; "
        "  public property Maybe: int? => _x; "
        "  public method Get(): int => Maybe ?? 0; "
        "  destructor end destructor; "
        "end Holder; "
        "class P public static method Main(): int "
        "  using var h := new Holder(9); "
        "  return h.Get(); "
        "end Main; end P;"), 9);
}

// 7e. generic class `T?` getter (T = int): the wrap runs in the receiver's
//     generic context so the payload lowers to the concrete type.
// per §23.2 / §23.3 / §7 / §14.1 (#1168)
BEZEL_TEST(property_accessor_types_510, nullable_generic_property_getter_1168) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public property Maybe: T? => _v; "
        "  destructor end destructor; "
        "end Box; "
        "class P public static method Main(): int "
        "  using var b := new Box<int>(42); "
        "  return b.Maybe ?? 0; "
        "end Main; end P;"), 42);
}

// 7g. STATIC `T?`-declared expression-bodied property read via the type name
//     (the static CG33b branch) must wrap the same way.
// per §23.2 / §23.3 / §7 / §22.13 (#1168)
BEZEL_TEST(property_accessor_types_510, nullable_static_property_getter_1168) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Holder "
        "  private static _x: int := 8; "
        "  public static property Maybe: int? => _x; "
        "end Holder; "
        "class P public static method Main(): int "
        "  return Holder.Maybe ?? 0; "
        "end Main; end P;"), 8);
}

// 7f. control: a body that is ALREADY `T?` (a nullable field) must NOT be
//     double-wrapped — the read passes the optional through unchanged.
// per §23.2 / §23.3 / §7 (#1168 control)
BEZEL_TEST(property_accessor_types_510, nullable_property_getter_passthrough_1168) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Holder "
        "  private _y: int?; "
        "  public constructor(y: int?) _y := y; end constructor; "
        "  public property Maybe: int? => _y; "
        "  destructor end destructor; "
        "end Holder; "
        "class P public static method Main(): int "
        "  using var h := new Holder(7); "
        "  return h.Maybe ?? 0; "
        "end Main; end P;"), 7);
}
