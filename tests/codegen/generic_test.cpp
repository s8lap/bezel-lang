// Codegen: generic class support — field-level substitution and full
// method monomorphization. Block-body and expression-body generic methods
// are specialized with concrete types; the result of a generic method call
// is tracked via localGenericSubst_ so subsequent field access and method
// calls on the result use the correct concrete types (§14).
//
// Per §14: `class Box<T> { value: T; ... }` is a generic class.
// `new Box<int>(42)` instantiates with T=int; `new Box<double>(3.14)` with T=double.

#include "support/aot_support.hpp"      // runAot — AOT oracle for disposal-bearing cases
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
// AOT counterpart — used where the program exercises owned-field disposal, so
// the stricter oracle (ownership + DA passes the JIT skips) applies.
int runAotProg(const std::string& prog) {
    auto r = runAot(prog);
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
        // `owned` is REQUIRED here, not decoration (#4758). Per §28.3
        // announcement table row 4 (v0.307), an instance method whose declared
        // return type is a plain concrete class type may return only a
        // receiver-sourced borrow; `new Pair<T>(...)` is a fresh owned value,
        // so the unmarked spelling is rejected by the real compiler. This row
        // runs on `runJit`, which never blocks on the ownership pass, so the
        // violation would surface only on a port to `runAot` (#3439).
        "  public method Swap(): owned Pair<T> return new Pair<T>(_second, _first); end Swap; "
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

// ── Computed property named "Length" ─────────────────────────────────────

BEZEL_TEST(codegen_generic, computed_property_named_length) {
    // §22.4/§33: A user-defined computed property named "Length" must return
    // its own getter expression, not be intercepted by the string Length
    // fallback. The fallback was too broad: it matched any Named type with
    // a "Length" member, treating the class pointer as a bz_string.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class V "
        "  private _n: int := 0; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public property Length: int => _n * 2; "  // user-defined Length
        "  destructor end destructor; "
        "end V; "
        "class C public static method Main(): int "
        "  using var v := new V(5); "
        "  return v.Length; "   // should be 10 (5*2), not garbage
        "end Main; end C;"), 10);
}

// Test: nullable generic field chain (root._left!._val)
// Run under the AOT oracle (§28.3 ownership pass): the `_left` link is `owned`
// so a produced child node can be stored into it (a borrow may not be assigned
// to a field, §28.3), and `left` is a plain `var` — a `using var` is scope-bound
// and cannot be moved out (§28.10), so it could not be transferred into root's
// owned field. The `_left!._val` chain (the feature under test) is unchanged.
BEZEL_TEST(codegen_generic, nullable_generic_field_chain) {
    BEZEL_EXPECT_EQ(runAotProg(
        "module M; "
        "class TreeNode<T> "
        "  public _val: T; "
        "  public owned _left: TreeNode<T>? := null; "
        "  public constructor(v: T) _val := v; end constructor; "
        "  destructor end destructor; "
        "end TreeNode<T>; "
        "class Program "
        "  public static method Main(): int "
        "    using var root := new TreeNode<string>(\"hello\"); "
        "    var left := new TreeNode<string>(\"apple\"); "
        "    root._left := left; "
        "    if root._left is not null then "
        "      var leftVal := root._left!._val; "
        "      if leftVal = \"apple\" then return 1; end; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;"), 1);
}

// Test: block-form property getter (get => do...end;) — per §22.9/A.4
BEZEL_TEST(codegen_generic, block_form_property_getter) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Acc "
        "  private _d: [10]int; private _n: int := 0; "
        "  public constructor() end constructor; "
        "  public method Add(v: int) _d[_n] := v; _n := _n + 1; end Add; "
        "  public property Sum: int "
        "    get => do "
        "      var s := 0; "
        "      for var i in 0.._n-1 do s := s + _d[i]; end; "
        "      return s; "
        "    end; "
        "  end Sum; "
        "  destructor end destructor; "
        "end Acc; "
        "class Program "
        "  public static method Main(): int "
        "    using var a := new Acc(); "
        "    a.Add(10); a.Add(20); a.Add(30); "
        "    return a.Sum; "
        "  end Main; "
        "end Program;"), 60);
}

// Test: string(n) explicit conversion — per §5.2/§9.6
BEZEL_TEST(codegen_generic, string_int_explicit_conv) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    var n := 42; "
        "    if string(n) = \"42\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end Program;"), 1);
}

// Test: static double field initialization — per §22.13
BEZEL_TEST(codegen_generic, static_double_field_init) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Consts public static Pi: double := 3.14159265358979; end Consts; "
        "class Program "
        "  public static method Main(): int "
        "    if Consts.Pi > 3.14 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end Program;"), 1);
}

// Test: int is null returns false — per §17.4 (value types can't be null)
BEZEL_TEST(codegen_generic, int_is_null_is_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    var n := 5; "
        "    if n is null then return 0; end; "
        "    return 1; "
        "  end Main; "
        "end Program;"), 1);
}

// Test: inline array collection literal initializer — §15.1/§16
BEZEL_TEST(codegen_generic, collection_literal_local) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    var arr: [5]int := [10, 20, 30, 40, 50]; "
        "    return arr[2]; "
        "  end Main; "
        "end Program;"), 30);
}

// Test: collection literal in class field — §15.1/§16
BEZEL_TEST(codegen_generic, collection_literal_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class V "
        "  public _d: [4]int := [10, 20, 30, 40]; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end V; "
        "class Program "
        "  public static method Main(): int "
        "    using var v := new V(); "
        "    return v._d[1]; "
        "  end Main; "
        "end Program;"), 20);
}

// Test: generic Contains on string stack uses content equality — §14/§4.6
BEZEL_TEST(codegen_generic, string_generic_contains) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Stk<T> "
        "  private _d: [10]T; private _n: int := 0; "
        "  public constructor() end constructor; "
        "  public method Push(v: T) _d[_n] := v; _n := _n + 1; end Push; "
        "  public method Contains(item: T): bool "
        "    for var i in 0.._n-1 do if _d[i] = item then return true; end; end; "
        "    return false; "
        "  end Contains; "
        "  destructor end destructor; "
        "end Stk; "
        "class Program "
        "  public static method Main(): int "
        "    using var s := new Stk<string>(); "
        "    s.Push(\"hello\"); s.Push(\"world\"); "
        "    if s.Contains(\"hello\") then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end Program;"), 1);
}

// Test: two-param generic map with string keys — §14
BEZEL_TEST(codegen_generic, two_param_string_key_map) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Map<K, V> "
        "  private _k: [10]K; private _v: [10]V; private _n: int := 0; "
        "  public constructor() end constructor; "
        "  private method Idx(key: K): int "
        "    for var i in 0.._n-1 do if _k[i] = key then return i; end; end; "
        "    return -1; "
        "  end Idx; "
        "  public method Set(key: K, val: V) "
        "    var i := Idx(key); "
        "    if i >= 0 then _v[i] := val; return; end; "
        "    _k[_n] := key; _v[_n] := val; _n := _n + 1; "
        "  end Set; "
        "  public method Get(key: K): V var i := Idx(key); return _v[i]; end Get; "
        "  public method Has(key: K): bool => Idx(key) >= 0; "
        "  destructor end destructor; "
        "end Map; "
        "class Program "
        "  public static method Main(): int "
        "    using var m := new Map<string, int>(); "
        "    m.Set(\"a\", 10); m.Set(\"b\", 20); m.Set(\"a\", 30); "
        "    if m.Get(\"a\") = 30 and m.Has(\"b\") and not m.Has(\"c\") then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end Program;"), 1);
}

// Test: ref params in generic method specialization (§14/CG50)
BEZEL_TEST(codegen_generic, ref_param_in_spec_inorder) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Coll<K> "
        "  private _d: [10]K; private _n: int := 0; "
        "  public constructor() end constructor; "
        "  public method Add(v: K) _d[_n] := v; _n := _n + 1; end Add; "
        "  public method Copy(ref r: [20]K, ref n: int) "
        "    for var i in 0.._n-1 do r[n] := _d[i]; n := n + 1; end; "
        "  end Copy; "
        "  destructor end destructor; "
        "end Coll; "
        "class Program "
        "  public static method Main(): int "
        "    using var c := new Coll<int>(); "
        "    c.Add(10); c.Add(20); c.Add(30); "
        "    var arr: [20]int; var n := 0; "
        "    c.Copy(ref arr, ref n); "
        "    if n = 3 and arr[0] = 10 and arr[2] = 30 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end Program;"), 1);
}

// Test: closures capture 'this' for instance field access — §21.5/CG60
BEZEL_TEST(codegen_generic, closure_captures_this_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Acc "
        "  public Total: int := 0; "
        "  public constructor() end constructor; "
        "  public method MakeAdder(w: int): (int) -> () "
        "    return lambda(x: int) => do Total := Total + x * w; end; "
        "  end MakeAdder; "
        "  destructor end destructor; "
        "end Acc; "
        "class Program "
        "  public static method Main(): int "
        "    using var a := new Acc(); "
        "    var add1 := a.MakeAdder(1); "
        "    var add2 := a.MakeAdder(2); "
        "    add1(10); add2(5); add1(3); "
        "    return a.Total; "  // 10*1 + 5*2 + 3*1 = 23
        "  end Main; "
        "end Program;"), 23);
}

// Test: user-defined operator= on class types — §25
BEZEL_TEST(codegen_generic, user_defined_operator_eq_record) {
    // §25.2: `=`/`<>` may be overloaded on records (not classes — classes use
    // `Equals`). A user `operator =` is dispatched for `a = b`.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record V(X: int, Y: int) "
        "  public static method operator =(a: V, b: V): bool "
        "    return a.X = b.X and a.Y = b.Y; "
        "  end operator =; "
        "  public static method operator <>(a: V, b: V): bool "
        "    return a.X <> b.X or a.Y <> b.Y; "
        "  end operator <>; "
        // §25.2 v0.158 co-declaration: an overloaded `=`/`<>` (homogeneous
        // operands) requires a consistent `Equals(T)` + `override GetHashCode`.
        "  public method Equals(other: V): bool => X = other.X and Y = other.Y; "
        "  public override method GetHashCode(): int => X * 31 + Y; "
        "end V; "
        "class Program "
        "  public static method Main(): int "
        "    var a := new V(3, 4); "
        "    var b := new V(3, 4); "
        "    var c := new V(1, 2); "
        "    if a = b and not (a = c) then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end Program;"), 1);
}

// Test: enum instance method with case this dispatch — §10.1/§30
BEZEL_TEST(codegen_generic, enum_instance_method_case_this) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Color Red, Green, Blue; "
        "  public method Name(): string "
        "    return (case this of "
        "      Color.Red => \"red\", "
        "      Color.Green => \"green\", "
        "      _ => \"blue\"); "
        "  end Name; "
        "end Color; "
        "class Program "
        "  public static method Main(): int "
        "    if Color.Red.Name() = \"red\" and Color.Blue.Name() = \"blue\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end Program;"), 1);
}
