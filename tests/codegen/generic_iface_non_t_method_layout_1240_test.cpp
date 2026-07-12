// Codegen — generic class interface dispatch for a method whose signature does
// NOT involve T returns the correct field value when T is a class (issue #1240).
//
// Root cause: specializedVtableFor (#1143 fix) rebuilt itable slots only when
// an interface method's concrete FUNCTION TYPE differed from the template (i.e.
// the method's return or param types involved T). A method like `GetN(): int`
// has the same function type in both the template (T→i32 erasing) and the
// concrete instantiation (T→ptr for a class arg), so no rebuild was triggered.
//
// However, when T→ptr, the concrete struct layout is:
//   {ptr vtable, i8 disposed, ptr _v, i32 _n}  ← offsets 0, 8, 16, 24
// while the template struct is:
//   {ptr vtable, i8 disposed, i32 _v, i32 _n}  ← offsets 0, 8, 12, 16
//
// The template `GetN()` body emits a GEP into the TEMPLATE struct type and
// loads `_n` at byte offset 16. The concrete object has `_n` at byte offset 24.
// Calling the template itable slot through the concrete object reads garbage
// (the upper 32 bits of the `_v` pointer), not the actual `_n` value.
//
// Fix (specializedVtableFor): detect when the concrete struct type (returned by
// classStruct(ts) with genericSubst_ active) differs from the template struct
// (structs_[ts], built without substitution). When they differ, ALL itable
// method slots are re-emitted under the active genericSubst_ — not only those
// whose function signatures involve T — so every body uses GEP offsets from the
// correct concrete struct type. §12/§14.
//
// Spec references:
//   §12  — interface itable dispatch
//   §14  — generic type/method monomorphization: T resolves to the concrete type;
//           every body must use the concrete struct layout.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── (a) primary repro: GetN(): int on Box<Cell> via IVal returns correct _n ──
// §12/§14 (#1240): `GetN(): int` has the same signature in template and
// concrete, but the template GEP reads at the wrong byte offset when T→ptr.
// Before the fix: rc=96 (high word of the _v pointer). After: rc=7.
BEZEL_TEST(generic_iface_non_t_method_layout_1240,
           non_t_method_reads_correct_field_via_interface) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IVal method GetN(): int; end IVal; "
        "class Cell end Cell; "
        "class Box<T>(v: T, n: int): IVal where T: class "
        "  public _v: T := v; "
        "  public _n: int := n; "
        "  public method GetN(): int => _n; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box<Cell>(new Cell(), 7); "
        "  var ib: IVal := b; "
        "  return ib.GetN(); "
        "end Main; end C;"), 7);
}

// ── (b) two int fields after T — method returns _x + _y via interface ─────────
// §12/§14 (#1240): when two int fields follow a class-typed T field, both shift
// by 4 bytes vs the template. The method `Sum(): int => _x + _y` has no T in
// its signature, so only the struct-layout trigger fires.
BEZEL_TEST(generic_iface_non_t_method_layout_1240,
           two_int_fields_after_class_t_sum_via_interface) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface ISum method Sum(): int; end ISum; "
        "class Cell end Cell; "
        "class Triple<T>(a: T, x: int, y: int): ISum where T: class "
        "  public _a: T := a; "
        "  public _x: int := x; "
        "  public _y: int := y; "
        "  public method Sum(): int => _x + _y; "
        "end Triple; "
        "class C public static method Main(): int "
        "  using var t := new Triple<Cell>(new Cell(), 3, 4); "
        "  var ib: ISum := t; "
        "  return ib.Sum(); "
        "end Main; end C;"), 7);
}

// ── (c) interface with BOTH a T-method and a non-T method ────────────────────
// §12/§14 (#1240): when an interface exposes both `GetV(): T` (involves T) and
// `GetN(): int` (does not involve T), both slots must be re-emitted for the
// concrete instantiation. The T-method slot was already rebuilt by the #1143
// fix (function-type trigger); the non-T slot is now rebuilt by the #1240
// layout-differs trigger. Both must return correct values.
BEZEL_TEST(generic_iface_non_t_method_layout_1240,
           interface_with_t_and_non_t_methods_both_correct) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IBoth<T> "
        "  method GetV(): T; "
        "  method GetN(): int; "
        "end IBoth; "
        "class Cell end Cell; "
        "class Box<T>(v: T, n: int): IBoth<T> where T: class "
        "  public _v: T := v; "
        "  public _n: int := n; "
        "  public method GetV(): T => _v; "
        "  public method GetN(): int => _n; "
        "end Box; "
        "class App "
        "  public static method Main(): int "
        "    using var cell := new Cell(); "
        "    using var b := new Box<Cell>(cell, 13); "
        "    var ib: IBoth<Cell> := b; "
        "    var v := ib.GetV(); "
        "    var n := ib.GetN(); "
        "    if v = cell then return n; end; "
        "    return 0; "
        "  end Main; "
        "end App;"), 13);
}

// ── (d) T=int (layout unchanged from template) — no spurious rebuild needed ──
// §12/§14: when T→i32 the concrete struct is identical to the template struct.
// The layout-differs trigger must NOT fire, and the existing template itable
// must still produce the correct result.
BEZEL_TEST(generic_iface_non_t_method_layout_1240,
           value_type_t_unchanged_layout_still_correct) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IVal method GetN(): int; end IVal; "
        "class Box<T>(v: T, n: int): IVal "
        "  public _v: T := v; "
        "  public _n: int := n; "
        "  public method GetN(): int => _n; "
        "end Box; "
        "class App "
        "  public static method Main(): int "
        "    using var b := new Box<int>(100, 99); "
        "    var iv: IVal := b; "
        "    return iv.GetN(); "
        "  end Main; "
        "end App;"), 99);
}

// ── (e) non-generic class implementing interface — unaffected by this change ──
// §12: a plain (non-generic) class's itable is unaffected by specializedVtableFor
// (genericSubst_ is empty, so it returns early). Must still dispatch correctly.
BEZEL_TEST(generic_iface_non_t_method_layout_1240,
           non_generic_class_interface_dispatch_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IVal method GetN(): int; end IVal; "
        "class Plain: IVal "
        "  public _n: int := 0; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public method GetN(): int => _n; "
        "end Plain; "
        "class App "
        "  public static method Main(): int "
        "    using var p := new Plain(55); "
        "    var iv: IVal := p; "
        "    return iv.GetN(); "
        "  end Main; "
        "end App;"), 55);
}
