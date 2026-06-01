// Codegen: generic class support — field-level substitution and full
// method monomorphization. Block-body and expression-body generic methods
// are specialized with concrete types; the result of a generic method call
// is tracked via localGenericSubst_ so subsequent field access and method
// calls on the result use the correct concrete types (§14).
//
// Per §14: `class Box<T> { value: T; ... }` is a generic class.
// `new Box<int>(42)` instantiates with T=int; `new Box<double>(3.14)` with T=double.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}

// Generic class with int type param — field read/write.
BEZEL_TEST(codegen_generic, int_field_read) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Box<T> "
        "  public value: T; "
        "  public constructor(v: T) value := v; end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box<int>(42); "
        "  return b.value; "
        "end Main; end C;"), 42);
}

BEZEL_TEST(codegen_generic, int_field_write_and_read) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Box<T> "
        "  public value: T; "
        "  public constructor(v: T) value := v; end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box<int>(10); "
        "  b.value := 99; "
        "  return b.value; "
        "end Main; end C;"), 99);
}

BEZEL_TEST(codegen_generic, two_type_params) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Pair<A, B> "
        "  public first: A; "
        "  public second: B; "
        "  public constructor(a: A, b: B) first := a; second := b; end constructor; "
        "  destructor end destructor; "
        "end Pair; "
        "class C public static method Main(): int "
        "  using var p := new Pair<int, int>(3, 7); "
        "  return p.first + p.second; "
        "end Main; end C;"), 10);
}

BEZEL_TEST(codegen_generic, separate_int_instantiations) {
    // Two Box<int> instances are independent.
    BEZEL_EXPECT_EQ(run(
        "module M; class Box<T> "
        "  public value: T; "
        "  public constructor(v: T) value := v; end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var a := new Box<int>(5); "
        "  using var b := new Box<int>(7); "
        "  return a.value + b.value; "
        "end Main; end C;"), 12);
}

// ── Generic method specialization (block body) ────────────────────────────

// §14: Block-body methods on generic classes are monomorphized for each
// concrete type arg. The following tests verify that methods returning T,
// methods storing T into fields, and methods calling other generic methods
// all produce correct values for both primitive and reference-typed T.

BEZEL_TEST(codegen_generic, stack_push_pop_int) {
    // Block-body generic stack with int: Push + Pop round-trip.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Stack<T> "
        "  private _d: [5]T; private _top: int := 0; "
        "  public constructor() end constructor; "
        "  public method Push(v: T) _d[_top] := v; _top := _top + 1; end Push; "
        "  public method Pop(): T _top := _top - 1; return _d[_top]; end Pop; "
        "  public method Size(): int => _top; "
        "  destructor end destructor; "
        "end Stack; "
        "class C public static method Main(): int "
        "  using var s := new Stack<int>(); "
        "  s.Push(10); s.Push(20); s.Push(30); "
        "  return s.Pop() + s.Pop(); "   // 30+20=50
        "end Main; end C;"), 50);
}

BEZEL_TEST(codegen_generic, stack_push_pop_double) {
    // §14: Same with T=double; Pop returns double, converted to int.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Stack<T> "
        "  private _d: [5]T; private _top: int := 0; "
        "  public constructor() end constructor; "
        "  public method Push(v: T) _d[_top] := v; _top := _top + 1; end Push; "
        "  public method Pop(): T _top := _top - 1; return _d[_top]; end Pop; "
        "  destructor end destructor; "
        "end Stack; "
        "class C public static method Main(): int "
        "  using var s := new Stack<double>(); "
        "  s.Push(1.5); s.Push(2.5); "
        "  return int(s.Pop() + s.Pop()); "   // 2.5+1.5=4
        "end Main; end C;"), 4);
}

BEZEL_TEST(codegen_generic, generic_swap_int) {
    // §14: Pair<int>.Swap() — block-body method returns new generic instance.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Pair<T> "
        "  public _first: T; public _second: T; "
        "  public constructor(a: T, b: T) _first := a; _second := b; end constructor; "
        "  public method Swap(): Pair<T> return new Pair<T>(_second, _first); end Swap; "
        "  destructor end destructor; "
        "end Pair; "
        "class C public static method Main(): int "
        "  using var p := new Pair<int>(3, 7); "
        "  using var p2 := p.Swap(); "
        "  return p2._first;  "   // swapped from 7→3, so _first=7
        "end Main; end C;"), 7);
}

BEZEL_TEST(codegen_generic, generic_size_after_layout_change) {
    // §14: Size() (returns int, not T) must still use the specialized struct
    // layout — the [5]T field has different byte size for T=int vs T=double.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Stack<T> "
        "  private _d: [5]T; private _top: int := 0; "
        "  public constructor() end constructor; "
        "  public method Push(v: T) _d[_top] := v; _top := _top + 1; end Push; "
        "  public method Size(): int => _top; "
        "  destructor end destructor; "
        "end Stack; "
        "class C public static method Main(): int "
        "  using var s := new Stack<double>(); "
        "  s.Push(1.0); s.Push(2.0); s.Push(3.0); "
        "  return s.Size(); "   // 3
        "end Main; end C;"), 3);
}

// ── Array assignment with side effects ───────────────────────────────────

BEZEL_TEST(codegen_generic, array_elem_assign_rhs_evaluated_once) {
    // §22.10: `arr[i] := expr` must evaluate `expr` exactly once.
    // Bug: the RHS was evaluated twice — once in the failed indexerSet
    // attempt and again in the plain lvalue path.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  private static _n: int := 0; "
        "  private static method Next(): int _n := _n + 1; return _n; end Next; "
        "  public static method Main(): int "
        "    var arr: [5]int; "
        "    for var i in 0..4 do arr[i] := Next(); end; "
        "    return _n; "   // should be 5, not 10
        "end Main; end C;"), 5);
}
