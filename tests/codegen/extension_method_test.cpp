// Codegen tests for extension methods (§26, issue #433).
//
// §26.1: inside an extension block `this` is the receiver. §26.5: when a
// member access `x.M(...)` finds no instance member, the compiler resolves M
// against an extension declared on x's type; an instance member of the same
// name always wins (no shadow). These tests JIT-run programs that call
// extension methods and assert the integer the program returns.
//
// The §26.1 codegen contract verified here: the receiver `this` is passed by
// VALUE for a primitive/value receiver (e.g. i32 for `extension int` — the
// historical malformed-IR crash, now fixed) and by POINTER for a class
// receiver (the object pointer, exactly as a normal method).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

// Run a whole module source and return Main's exit code, or a sentinel.
int runMod(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// ===========================================================================
// §26.1 / §26.5 — extension method on a CLASS receiver. `this` is the object
// pointer; `this._x` reads a field, exactly as a normal method body would.
// ===========================================================================
BEZEL_TEST(codegen_extension, method_on_class_reads_field_spec_26_1) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "class Foo "
        "  public _x: int := 0; "
        "  public constructor(v: int) _x := v; end constructor; "
        "end Foo; "
        "extension Foo "
        "  public method Bar(): int => this._x + 1; "
        "end; "
        "class P public static method Main(): int "
        "  var f := new Foo(41); "
        "  return f.Bar(); "
        "end Main; end P;"), 42);
}

// §26.5: an extension method that takes an argument in addition to `this`.
BEZEL_TEST(codegen_extension, method_on_class_with_arg_spec_26_5) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "class Foo "
        "  public _x: int := 0; "
        "  public constructor(v: int) _x := v; end constructor; "
        "end Foo; "
        "extension Foo "
        "  public method AddN(n: int): int => this._x + n; "
        "end; "
        "class P public static method Main(): int "
        "  var f := new Foo(40); "
        "  return f.AddN(2); "
        "end Main; end P;"), 42);
}

// ===========================================================================
// §26.1 / §26.2 — extension method on a PRIMITIVE receiver (`extension int`).
// This was the malformed-IR crash: `this` lowered as a pointer for a value
// receiver. `this` is now the i32 VALUE. `(5).Double()` and `x.Double()`.
// ===========================================================================
BEZEL_TEST(codegen_extension, method_on_int_literal_spec_26_2) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "extension int "
        "  public method Double(): int => this * 2; "
        "end; "
        "class P public static method Main(): int "
        "  return (5).Double(); "
        "end Main; end P;"), 10);
}

BEZEL_TEST(codegen_extension, method_on_int_local_spec_26_2) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "extension int "
        "  public method Triple(): int => this * 3; "
        "end; "
        "class P public static method Main(): int "
        "  var x := 7; "
        "  return x.Triple(); "
        "end Main; end P;"), 21);
}

// §26.5: a primitive extension method that takes an argument.
BEZEL_TEST(codegen_extension, method_on_int_with_arg) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "extension int "
        "  public method Plus(n: int): int => this + n; "
        "end; "
        "class P public static method Main(): int "
        "  var x := 40; "
        "  return x.Plus(2); "
        "end Main; end P;"), 42);
}

// ===========================================================================
// §26.2 / §26.1 — extension method on a RECORD receiver (value type). `this`
// is a pointer to a spilled copy of the record value, so `this.field` reuses
// the record field-access machinery.
// ===========================================================================
BEZEL_TEST(codegen_extension, method_on_record_value_receiver_spec_26_2) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "record Point(x: int, y: int); "
        "extension Point "
        "  public method Sum(): int => this.x + this.y; "
        "end; "
        "class P public static method Main(): int "
        "  var pt := new Point(40, 2); "
        "  return pt.Sum(); "
        "end Main; end P;"), 42);
}

// §26.2 — extension method on an ENUM receiver (value type, i32 `this`).
BEZEL_TEST(codegen_extension, method_on_enum_value_receiver_spec_26_2) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "extension Color "
        "  public method Code(): int => 7; "
        "end; "
        "class P public static method Main(): int "
        "  var c := Color.Green; "
        "  return c.Code(); "
        "end Main; end P;"), 7);
}

// R36 / §26 / CG30: a `case this` over the enum's own constants inside an enum
// EXTENSION method. The extension body has no parent type, so enumTypeOf(this)
// previously returned null, the enum-constant arms never matched, and codegen
// left `case.end` unterminated → an IR-verify failure. Now resolves the enum
// from the extension's target type. Color.Green => 20.
BEZEL_TEST(codegen_extension, case_this_over_enum_in_extension_method_r36) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "extension Color "
        "  public method ToInt(): int "
        "    return case this "
        "      Color.Red => 10, "
        "      Color.Green => 20, "
        "      Color.Blue => 30, "
        "      _ => -1 "
        "    end; "
        "  end ToInt; "
        "end; "
        "class P public static method Main(): int "
        "  var c := Color.Green; "
        "  return c.ToInt(); "
        "end Main; end P;"), 20);
}

// ===========================================================================
// §26.5 NO-SHADOW — "Adding a real method to a type silently takes priority
// over an extension with the same name — by design." The INSTANCE member must
// win; the extension's same-named method must NOT be selected.
// ===========================================================================
BEZEL_TEST(codegen_extension, instance_member_wins_over_extension_spec_26_5) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "class Foo "
        "  public method Value(): int => 100; "
        "end Foo; "
        "extension Foo "
        "  public method Value(): int => 1; "
        "end; "
        "class P public static method Main(): int "
        "  var f := new Foo(); "
        "  return f.Value(); "   // instance 100 wins, not extension 1
        "end Main; end P;"), 100);
}

// ===========================================================================
// BUG #456 / §26.1 — extension method on a PRIMITIVE calling another extension
// method via `this.OtherMethod()`. Inside an `extension int` body, `this` must
// carry the extended type (int) so that `resolveMemberAccess` can resolve the
// extension member on `this`. Without the fix, `this` has no exprType, so
// lookupExtensionMember is never reached and codegen emits `ret i32 0`.
// ===========================================================================

// §26.1 / #456: `this.OtherMethod()` inside a primitive extension dispatches
// to the other extension method — the callee sees the right `this` value.
BEZEL_TEST(codegen_extension, ext_this_call_primitive_456) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "extension int "
        "  public method Double(): int => this * 2; "
        "  public method Quadruple(): int => this.Double() * 2; "
        "end; "
        "class P public static method Main(): int "
        "  var x := 7; "
        "  return x.Quadruple(); "   // 7 * 2 * 2 = 28
        "end Main; end P;"), 28);
}

// §26.1 / #456: chain of three primitive extension methods, each calling the
// next via `this.M()`. Verifies transitivity and correct `this` threading.
BEZEL_TEST(codegen_extension, ext_this_chain_primitive_456) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "extension int "
        "  public method Inc(): int => this + 1; "
        "  public method Inc2(): int => this.Inc() + 1; "
        "  public method Inc4(): int => this.Inc2() + this.Inc2(); "
        "end; "
        "class P public static method Main(): int "
        "  var x := 10; "
        "  return x.Inc4(); "   // (10+1+1) + (10+1+1) = 24
        "end Main; end P;"), 24);
}

// §26.1 / #456: `extension int` with an extra arg, calling another extension
// method via `this.M()` — verifies `this` threads correctly into the callee.
BEZEL_TEST(codegen_extension, ext_this_call_with_arg_456) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "extension int "
        "  public method AddSelf(): int => this + this; "
        "  public method Scaled(n: int): int => this.AddSelf() * n; "
        "end; "
        "class P public static method Main(): int "
        "  var x := 5; "
        "  return x.Scaled(3); "  // (5+5)*3 = 30
        "end Main; end P;"), 30);
}

// ===========================================================================
// REGRESSION — a program with NO extensions is unaffected (a class method
// call resolves and runs exactly as before).
// ===========================================================================
BEZEL_TEST(codegen_extension, no_extension_program_unaffected) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "class Foo "
        "  public _x: int := 0; "
        "  public constructor(v: int) _x := v; end constructor; "
        "  public method Get(): int => _x; "
        "end Foo; "
        "class P public static method Main(): int "
        "  var f := new Foo(42); "
        "  return f.Get(); "
        "end Main; end P;"), 42);
}

// ===========================================================================
// BUG #702 / §26.5 — user-defined extension method on the `string` primitive.
//
// Before fix #702 the string-member ladder emitted a hard "not yet supported"
// error for every unrecognised string member BEFORE the generic extension-
// lookup block was reached (line ~5043 of ExprTypecheck_Calls.cpp).  A valid
// §26.5 `extension string` method was therefore always rejected.
//
// Fix: attempt lookupExtensionMember first; emit the error only when no user
// extension resolves the member.
// ===========================================================================

// §26.5 / #702: basic repro — extension on string compiles and returns correctly.
// IsBlank() returns 1 (true → int via conditional) when the string is empty.
BEZEL_TEST(codegen_extension, method_on_string_isblank_spec_26_5_702) {
    // IsBlank: returns 1 when Length = 0, 0 otherwise.
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "extension string "
        "  public method IsBlank(): bool => this.Length = 0; "
        "end; "
        "class Program "
        "  public static method Main(): int "
        "    var s := \"hello\"; "
        "    var b := s.IsBlank(); "
        "    if b then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end Program;"), 0);
}

// §26.5 / #702: empty string → IsBlank() returns true.
BEZEL_TEST(codegen_extension, method_on_string_isblank_empty_spec_26_5_702) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "extension string "
        "  public method IsBlank(): bool => this.Length = 0; "
        "end; "
        "class Program "
        "  public static method Main(): int "
        "    var s := \"\"; "
        "    var b := s.IsBlank(); "
        "    if b then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end Program;"), 1);
}

// §26.5 / #702: extension method on string that takes an argument and uses
// the result of a built-in string method (Length) inside the body.
BEZEL_TEST(codegen_extension, method_on_string_longer_than_spec_26_5_702) {
    // LongerThan(n): returns 1 if Length > n, else 0 — used as return value.
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "extension string "
        "  public method LongerThan(n: int): bool => this.Length > n; "
        "end; "
        "class Program "
        "  public static method Main(): int "
        "    var s := \"hello\"; "
        "    if s.LongerThan(3) then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end Program;"), 1);
}

// ===========================================================================
// §26.5 (#927) — an extension on a BASE class (or implemented INTERFACE) must
// be found via a receiver whose static type is a SUBTYPE of the extension's
// target. Before the fix, extension lookup matched only the receiver's exact
// type and rejected `d.GetV()` with "no member GetV on type Derived".
// ===========================================================================

// Extension on a direct base class, called through a derived receiver.
BEZEL_TEST(codegen_extension, method_on_base_via_derived_receiver_spec_26_5_927) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "class Base "
        "  public v: int; "
        "  public constructor(n: int) this.v := n; end constructor; "
        "  destructor end destructor; "
        "end Base; "
        "class Derived : Base "
        "  public constructor() base(7); end constructor; "
        "end Derived; "
        "extension Base "
        "  public method GetV(): int => this.v; "
        "end; "
        "class C public static method Main(): int "
        "  using var d := new Derived(); "
        "  return d.GetV(); "  // 7
        "end Main; end C;"), 7);
}

// Two levels of inheritance (Leaf : Mid : Base); the extension lives on Base.
BEZEL_TEST(codegen_extension, method_on_base_via_grandchild_spec_26_5_927) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "class Base public v: int; "
        "  public constructor(n: int) this.v := n; end constructor; "
        "  destructor end destructor; end Base; "
        "class Mid : Base public constructor() base(5); end constructor; end Mid; "
        "class Leaf : Mid public constructor() base(); end constructor; end Leaf; "
        "extension Base public method GetV(): int => this.v; end; "
        "class C public static method Main(): int "
        "  using var d := new Leaf(); "
        "  return d.GetV(); "  // 5
        "end Main; end C;"), 5);
}

// Extension on an INTERFACE, reached through an implementing class receiver.
BEZEL_TEST(codegen_extension, method_on_interface_via_impl_spec_26_5_927) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "interface IShape method Area(): int; end; "
        "class Square : IShape "
        "  public side: int; "
        "  public constructor(s: int) this.side := s; end constructor; "
        "  public method Area(): int => this.side * this.side; "
        "  destructor end destructor; "
        "end Square; "
        "extension IShape public method Describe(): int => this.Area() + 1; end; "
        "class C public static method Main(): int "
        "  using var s := new Square(4); "
        "  return s.Describe(); "  // 4*4 + 1 = 17
        "end Main; end C;"), 17);
}

// ===========================================================================
// §26.5 / #1237 — Facet A: extension on an INTERFACE called with a RECORD
// receiver.  The record implements the interface; calling the extension via
// the record value requires boxing the record into a proper interface
// reference (vtable + itable) so `this.M()` inside the extension dispatches
// through the itable.  Before the fix this SIGSEGV'd because the receiver was
// spilled to a raw stack pointer with no vtable, which itable_lookup derefs.
// ===========================================================================

// §26.5 / #1237 Facet A – basic repro: record receiver → extension boxing →
// `this.Get()` itable dispatch.  r is record R(7); r.Wrap() must return 7.
BEZEL_TEST(codegen_extension,
           iface_extension_record_receiver_boxes_for_this_dispatch_1237_A) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "interface IFoo method Get(): int; end IFoo; "
        "record R(X: int): IFoo "
        "  public method Get(): int => X; "
        "end R; "
        "extension IFoo "
        "  public method Wrap(): int => this.Get(); "
        "end; "
        "class C public static method Main(): int "
        "  var r := new R(7); "
        "  return r.Wrap(); "   // 7
        "end Main; end C;"), 7);
}

// §26.5 / #1237 Facet A – variant: extension takes an arg AND calls two
// interface methods via `this`, both dispatched through the itable.
BEZEL_TEST(codegen_extension,
           iface_extension_record_receiver_two_this_calls_1237_A) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "interface IFoo method Get(): int; method Add(n: int): int; end IFoo; "
        "record R(X: int): IFoo "
        "  public method Get(): int => X; "
        "  public method Add(n: int): int => X + n; "
        "end R; "
        "extension IFoo "
        "  public method WrapTwo(): int => this.Get() + this.Add(10); "
        "end; "
        "class C public static method Main(): int "
        "  var r := new R(5); "
        "  return r.WrapTwo(); "   // 5 + (5+10) = 20
        "end Main; end C;"), 20);
}

// §26.5 / #1237 Facet A – regression: CLASS receiver for extension on the
// same interface must still work (class is already a pointer, no boxing).
BEZEL_TEST(codegen_extension,
           iface_extension_class_receiver_regression_1237_A) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "interface IFoo method Get(): int; end IFoo; "
        "class Impl: IFoo "
        "  private _v: int := 0; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method Get(): int => _v; "
        "  destructor end destructor; "
        "end Impl; "
        "extension IFoo "
        "  public method Wrap(): int => this.Get(); "
        "end; "
        "class C public static method Main(): int "
        "  using var c := new Impl(99); "
        "  return c.Wrap(); "   // 99
        "end Main; end C;"), 99);
}

// §26.5 / #1237 Facet A – regression: interface-VARIABLE receiver must still
// work (the interface variable already holds a properly boxed/classed pointer).
BEZEL_TEST(codegen_extension,
           iface_extension_iface_var_receiver_regression_1237_A) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "interface IFoo method Get(): int; end IFoo; "
        "record R(X: int): IFoo "
        "  public method Get(): int => X; "
        "end R; "
        "extension IFoo "
        "  public method Wrap(): int => this.Get(); "
        "end; "
        "class C public static method Main(): int "
        "  var r := new R(55); "
        "  var ri: IFoo := r; "
        "  return ri.Wrap(); "   // 55
        "end Main; end C;"), 55);
}

// ===========================================================================
// §26.5 / #1237 — Facet B: `this[i]` indexer dispatch inside an extension
// body on an INDEXER INTERFACE.  Before the fix, indexReceiverDecl returned
// null for `this` inside an extension method (extension methods have no
// parent TypeSymbol), so lowerIndexerGet returned null → caller got 0.
// ===========================================================================

// §26.5 / #1237 Facet B – basic repro: `this[0]` inside an extension on an
// indexer interface, called through an interface-variable receiver → 42.
BEZEL_TEST(codegen_extension,
           iface_extension_this_indexer_dispatch_1237_B) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "interface IIdx property [i: int]: int; end IIdx; "
        "class Arr: IIdx "
        "  private _v: int := 0; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public property [i: int]: int => _v + i; "
        "  destructor end destructor; "
        "end Arr; "
        "extension IIdx "
        "  public method First(): int => this[0]; "
        "end; "
        "class C public static method Main(): int "
        "  using var a := new Arr(42); "
        "  var ip: IIdx := a; "
        "  return ip.First(); "   // 42 + 0 = 42
        "end Main; end C;"), 42);
}

// §26.5 / #1237 Facet B – regression: direct `ip[i]` from a regular (non-
// extension) method must still dispatch correctly through the itable.
BEZEL_TEST(codegen_extension,
           iface_indexer_direct_dispatch_regression_1237_B) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "interface IIdx property [i: int]: int; end IIdx; "
        "class Arr: IIdx "
        "  private _v: int := 0; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public property [i: int]: int => _v + i; "
        "  destructor end destructor; "
        "end Arr; "
        "class C "
        "  private static method GetAt(arr: IIdx, i: int): int => arr[i]; "
        "  public static method Main(): int "
        "    using var a := new Arr(40); "
        "    return GetAt(a, 2); "   // 42
        "  end Main; "
        "end C;"), 42);
}
