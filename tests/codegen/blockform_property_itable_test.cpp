// Block-form property getter dispatched through an interface itable (§23.3 /
// §12.7 / §8.6). Issue: a property accessor declared with the block syntax
//   `property Val: int get => do ... return ...; end; end Val;`
// was not wired into the record's box itable in Pass C of LlvmBackend_Setup.cpp
// (#481 wired properties for CLASSES in Pass D, but Pass C for records only
// handled methods — not events or properties). Accessing a block-form getter
// through an interface-typed variable therefore loaded a null or stale function
// pointer from the itable → SIGSEGV.
//
// The same gap also caused expression-body interface properties on records to
// SIGSEGV when the interface had no methods (only properties), because the
// `if (ifaceMethods.empty()) continue;` guard skipped building an itable for
// the interface entirely.
//
// Tests cover:
//  (a) Repro: block-form getter on a record via interface → returns 9.
//  (b) Block-form getter on a CLASS via interface → returns 7 (classes went
//      through Pass D which was already correct; this is a regression guard).
//  (c) Block-form getter with multiple statements on a record via interface.
//  (d) Block-form setter on a record via interface (read-write property).
//  (e) Control: expression-form property via interface — record + class.
//  (f) Control: block-form property called directly (not via interface).
//  (g) Interface with ONLY properties (no methods) — used to be skipped entirely
//      due to the `ifaceMethods.empty()` guard.
//
// Observable via the int exit code (string comparisons return 1/0).
// All per §23.3 / §13 / §8.6 / §12.7.

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

// (a) Repro (§23.3 / §13 / §8.6): block-form property getter on a RECORD
// dispatched through an interface variable.  `Val` uses `get => do return X; end`
// (block accessor syntax). Before the fix this SIGSEGVd because the record's
// box itable (Pass C) did not include a slot for the property.
BEZEL_TEST(codegen_blockform_property_itable, record_blockgetter_via_interface) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IFoo  property Val: int; end IFoo; "
        "record R(X: int) : IFoo "
        "  public property Val: int "
        "    get => do return X; end; "
        "  end Val; "
        "end R; "
        "class C public static method Main(): int "
        "  var r := new R(9); "
        "  var f: IFoo := r; "
        "  return f.Val; "
        "end Main; end C;"), 9);
}

// (b) Regression guard: block-form getter on a CLASS via interface — was wired
// in Pass D and must remain working after the Pass C fix.
// §23.3 / §13 / §12.1.
BEZEL_TEST(codegen_blockform_property_itable, class_blockgetter_via_interface) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        // §11.2/§23.3 (#2413): Impl's `Val` is a getter-only block-form
        // property (no `set` arm) and this test never writes through the
        // interface, so IBar's obligation is declared `readonly` — a
        // getter-only implementation of a read-write interface property is
        // now a compile error (#2413's own fix), and `readonly` is the
        // accurate declaration of what this test actually exercises.
        "interface IBar  readonly property Val: int; end IBar; "
        "class Impl : IBar "
        "  protected _v: int := 7; "
        "  public property Val: int "
        "    get => do return _v; end; "
        "  end Val; "
        "  destructor end destructor; "
        "end Impl; "
        "class C "
        "  public static method Get(b: IBar): int => b.Val; "
        "  public static method Main(): int "
        "    using var o := new Impl(); return Get(o); "
        "  end Main; "
        "end C;"), 7);
}

// (c) Block-form getter with multiple statements on a record via interface.
// §23.3: the block body is a statement list; `return` carries the value.
// 5 + 3 = 8.
BEZEL_TEST(codegen_blockform_property_itable, record_blockgetter_multi_stmt_via_interface) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface ICalc  property Result: int; end ICalc; "
        "record Pair(A: int, B: int) : ICalc "
        "  public property Result: int "
        "    get => do "
        "      var sum := A + B; "
        "      return sum; "
        "    end; "
        "  end Result; "
        "end Pair; "
        "class C "
        "  public static method Get(c: ICalc): int => c.Result; "
        "  public static method Main(): int "
        "    var p := new Pair(5, 3); "
        "    var c: ICalc := p; "
        "    return Get(c); "
        "  end Main; "
        "end C;"), 8);
}

// (d) Block-form SETTER on a record via interface.  §23 / §13: read-write
// property with both a block-form getter and a block-form setter. The record
// is a body record (not positional) so the field initializer does not
// reference another instance member (§22.8 forbids that). The setter mutates
// the boxed copy of the record (the interface variable holds the box).
// Set to 42, read back via interface → 42.
BEZEL_TEST(codegen_blockform_property_itable, record_block_setter_via_interface) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IRW  property Count: int; end IRW; "
        "record Counter : IRW "
        "  protected _n: int := 0; "
        "  public property Count: int "
        "    get => do return _n; end; "
        "    set => do _n := value; end; "
        "  end Count; "
        "end Counter; "
        "class C "
        "  public static method Main(): int "
        "    var c := new Counter(); "
        "    var r: IRW := c; "
        "    r.Count := 42; "
        "    return r.Count; "
        "  end Main; "
        "end C;"), 42);
}

// (e1) Control: expression-form property on a RECORD via interface still works
// after the fix.  §23.2 / §13 / §8.6.
BEZEL_TEST(codegen_blockform_property_itable, record_arrow_getter_via_interface_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface INum  property Num: int; end INum; "
        "record Wrap(V: int) : INum "
        "  public property Num: int => V; "
        "end Wrap; "
        "class C "
        "  public static method Get(n: INum): int => n.Num; "
        "  public static method Main(): int "
        "    var w := new Wrap(21); "
        "    var n: INum := w; "
        "    return Get(n); "
        "  end Main; "
        "end C;"), 21);
}

// (e2) Control: expression-form property on a CLASS via interface still works.
BEZEL_TEST(codegen_blockform_property_itable, class_arrow_getter_via_interface_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        // §11.2/§23.3 (#2413): Tagged's `Tag` is getter-only (expression-
        // bodied) and this test never writes through the interface, so
        // ITag's obligation is declared `readonly` (see the identical note
        // on class_blockgetter_via_interface above).
        "interface ITag  readonly property Tag: int; end ITag; "
        "class Tagged : ITag "
        "  public property Tag: int => 55; "
        "  destructor end destructor; "
        "end Tagged; "
        "class C "
        "  public static method Get(t: ITag): int => t.Tag; "
        "  public static method Main(): int "
        "    using var x := new Tagged(); return Get(x); "
        "  end Main; "
        "end C;"), 55);
}

// (f1) Control: block-form property on a record called DIRECTLY (not via
// interface) still works.  §23.3.
BEZEL_TEST(codegen_blockform_property_itable, record_blockgetter_direct_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Val(X: int) "
        "  public property Double: int "
        "    get => do return X * 2; end; "
        "  end Double; "
        "end Val; "
        "class C public static method Main(): int "
        "  var v := new Val(6); "
        "  return v.Double; "
        "end Main; end C;"), 12);
}

// (f2) Control: block-form property on a CLASS called directly still works.
// §23.3.
BEZEL_TEST(codegen_blockform_property_itable, class_blockgetter_direct_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class MyClass "
        "  protected _x: int := 13; "
        "  public property Triple: int "
        "    get => do return _x * 3; end; "
        "  end Triple; "
        "  destructor end destructor; "
        "end MyClass; "
        "class C public static method Main(): int "
        "  using var o := new MyClass(); "
        "  return o.Triple; "
        "end Main; end C;"), 39);
}

// (g) Interface with ONLY properties (no methods) — was previously skipped by
// the `if (ifaceMethods.empty()) continue;` guard in Pass C, producing an
// empty itable and a SIGSEGV on first dispatch. §13 / §23.
// The record uses differently-named fields (Width/Height) to avoid duplicating
// the interface property names W/H (§11.3 forbids duplicate member names).
BEZEL_TEST(codegen_blockform_property_itable, interface_only_property_no_methods) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface ISize  property W: int; property H: int; end ISize; "
        "record Rect(Width: int, Height: int) : ISize "
        "  public property W: int => Width; "
        "  public property H: int => Height; "
        "end Rect; "
        "class C "
        "  public static method Area(s: ISize): int => s.W * s.H; "
        "  public static method Main(): int "
        "    var r := new Rect(4, 5); "
        "    var s: ISize := r; "
        "    return Area(s); "
        "  end Main; "
        "end C;"), 20);
}
