// Codegen — protected/data field of a GENERIC base class read through a derived
// class (issue #585; §12 inheritance, §13.5 protected, §14 generics).
//
// HIGH miscompile: a `protected` field of a GENERIC base class read as 0 in (and
// through) a derived class. The base ctor wrote the field at the wrong offset
// because the derived class's instance struct embedded the base's generic field
// `_val: T` WITHOUT resolving `T` to the concrete type argument of the base
// sub-object. `Container<int>` standalone laid `_val` out correctly (its own `T`
// was substituted), but as the base of `Wrapper<int>` the inherited `_val` was
// emitted with the unresolved generic placeholder, so the layout (and every
// offset after it) shifted and the read produced garbage / 0.
//
// Per §12: a derived class's instance has prefix layout — the base sub-object's
// data fields come first, at the SAME offsets they have in the base, then the
// derived's own. Per §14: when instantiating `Wrapper<int>`, the base
// `Container<int>` sub-object's `T` is bound to `int`. The fix resolves the base
// field's generic type through the base's concrete type argument when computing
// the derived instantiation's layout.
//
// All tests are load-bearing: BaseGet/DerivedGet read 0 (or garbage) before the
// fix and the correct value after. Non-generic-base regression confirms the
// existing prefix layout still works. Only real stdlib surface is used.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── §14: generic base standalone still works (control) ──

// `new Container<int>(10).BaseGet()` → 10. Container<int> on its own has T=int
// substituted into `_val`, so this already worked; it pins the control case.
BEZEL_TEST(generic_base_field_585, generic_base_standalone_reads_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Container<T> "
        "  protected _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "  public method BaseGet(): T return _val; end BaseGet; "
        "  destructor end destructor; "
        "end Container; "
        "class C public static method Main(): int "
        "  using var c := new Container<int>(10); "
        "  return c.BaseGet(); "
        "end Main; end C;"), 10);
}

// ── §12/§13.5/§14: the issue — generic base field read THROUGH the derived ──

// new Wrapper<int>(42): BaseGet() (the inherited method) must read 42.
// Before the fix: 0 (base ctor wrote _val at the wrong offset).
BEZEL_TEST(generic_base_field_585, generic_base_field_via_inherited_method) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Container<T> "
        "  protected _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "  public method BaseGet(): T return _val; end BaseGet; "
        "  destructor end destructor; "
        "end Container; "
        "class Wrapper<T>: Container<T> "
        "  public constructor(v: T) base(v); end constructor; "
        "  public method DerivedGet(): T return _val; end DerivedGet; "
        "  destructor end destructor; "
        "end Wrapper; "
        "class C public static method Main(): int "
        "  using var w := new Wrapper<int>(42); "
        "  return w.BaseGet(); "
        "end Main; end C;"), 42);
}

// new Wrapper<int>(42): DerivedGet() (a derived method reading the protected
// inherited field) must read 42 too. Before the fix: 0.
BEZEL_TEST(generic_base_field_585, generic_base_field_via_derived_method) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Container<T> "
        "  protected _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "  public method BaseGet(): T return _val; end BaseGet; "
        "  destructor end destructor; "
        "end Container; "
        "class Wrapper<T>: Container<T> "
        "  public constructor(v: T) base(v); end constructor; "
        "  public method DerivedGet(): T return _val; end DerivedGet; "
        "  destructor end destructor; "
        "end Wrapper; "
        "class C public static method Main(): int "
        "  using var w := new Wrapper<int>(42); "
        "  return w.DerivedGet(); "
        "end Main; end C;"), 42);
}

// ── §14: generic base + the derived adds its OWN generic field ──

// The derived's own field must sit AFTER the inherited base field (prefix
// layout). Reading both confirms neither offset is wrong: base _val=7, own
// _extra=5. `T` is unconstrained, so the body uses `=` (an intrinsic on every
// type, §17.3) rather than `+` (undefined on an unconstrained `T`, §25): the
// method returns 12 only when both fields hold their distinct stored values.
BEZEL_TEST(generic_base_field_585, generic_base_plus_own_generic_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Container<T> "
        "  protected _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "  public method BaseGet(): T return _val; end BaseGet; "
        "  destructor end destructor; "
        "end Container; "
        "class Wrapper<T>: Container<T> "
        "  protected _extra: T; "
        "  public constructor(v: T, e: T) base(v); _extra := e; end constructor; "
        "  public method Both(v: T, e: T): int "
        "    if _val = v then if _extra = e then return 12; end; end; "
        "    return 0; "
        "  end Both; "
        "  destructor end destructor; "
        "end Wrapper; "
        "class C public static method Main(): int "
        "  using var w := new Wrapper<int>(7, 5); "
        "  return w.Both(7, 5); "
        "end Main; end C;"), 12);
}

// ── §12/§14: multi-level generic inheritance ──

// Leaf<T> : Mid<T> : Base<T> — the deepest inherited generic field must still
// land at the right offset two levels down. Base._val=99 read through the leaf.
BEZEL_TEST(generic_base_field_585, multi_level_generic_inheritance) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base<T> "
        "  protected _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "  public method BaseGet(): T return _val; end BaseGet; "
        "  destructor end destructor; "
        "end Base; "
        "class Mid<T>: Base<T> "
        "  public constructor(v: T) base(v); end constructor; "
        "  destructor end destructor; "
        "end Mid; "
        "class Leaf<T>: Mid<T> "
        "  public constructor(v: T) base(v); end constructor; "
        "  public method LeafGet(): T return _val; end LeafGet; "
        "  destructor end destructor; "
        "end Leaf; "
        "class C public static method Main(): int "
        "  using var l := new Leaf<int>(99); "
        "  return l.LeafGet(); "
        "end Main; end C;"), 99);
}

// ── §14: generic base field of a non-int type (string) ──

// T = string: the base field is a pointer-sized slot. The wrong-offset bug also
// mislaid pointer fields; here a derived instance carries an inherited string
// and reads its length back. "hello".Length == 5.
BEZEL_TEST(generic_base_field_585, generic_base_string_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Container<T> "
        "  protected _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "  public method BaseGet(): T return _val; end BaseGet; "
        "  destructor end destructor; "
        "end Container; "
        "class Wrapper<T>: Container<T> "
        "  public constructor(v: T) base(v); end constructor; "
        "  destructor end destructor; "
        "end Wrapper; "
        "class C public static method Main(): int "
        "  using var w := new Wrapper<string>(\"hello\"); "
        "  return w.BaseGet().Length; "
        "end Main; end C;"), 5);
}

// ── §12/§14: DIRECT access of an inherited generic-base field (read side) ──

// `w._val` accessed directly off a `Wrapper<int>` (not via an inherited method)
// reads the base field through the derived layout. `_val`'s parent is the base
// `Container`, so the field-access substitution must walk the base chain to bind
// `Container.T → int`. Before the fix the direct access used the T→i32 template
// layout; for int it read 0 once the offset shifted under a wider derived field.
BEZEL_TEST(generic_base_field_585, inherited_field_direct_access_int) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Container<T> "
        "  public _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "  destructor end destructor; "
        "end Container; "
        "class Wrapper<T>: Container<T> "
        "  public constructor(v: T) base(v); end constructor; "
        "  destructor end destructor; "
        "end Wrapper; "
        "class C public static method Main(): int "
        "  using var w := new Wrapper<int>(55); "
        "  return w._val; "
        "end Main; end C;"), 55);
}

// Same, T = string: direct `w._val.Length` through a derived `Wrapper<string>`.
// The base field is a pointer slot; the field-access substitution must resolve
// `Container.T → string` so the GEP loads a string ptr, not a truncated i32.
BEZEL_TEST(generic_base_field_585, inherited_field_direct_access_string) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Container<T> "
        "  public _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "  destructor end destructor; "
        "end Container; "
        "class Wrapper<T>: Container<T> "
        "  public constructor(v: T) base(v); end constructor; "
        "  destructor end destructor; "
        "end Wrapper; "
        "class C public static method Main(): int "
        "  using var w := new Wrapper<string>(\"world!\"); "
        "  return w._val.Length; "  // 6
        "end Main; end C;"), 6);
}

// ── §12: non-generic base regression ──

// A non-generic base read through a derived class must keep working (this was
// always correct; guards against a regression from the fix).
BEZEL_TEST(generic_base_field_585, non_generic_base_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Animal "
        "  protected _legs: int; "
        "  public constructor(n: int) _legs := n; end constructor; "
        "  public method Legs(): int return _legs; end Legs; "
        "  destructor end destructor; "
        "end Animal; "
        "class Dog: Animal "
        "  public constructor() base(4); end constructor; "
        "  public method DogLegs(): int return _legs; end DogLegs; "
        "  destructor end destructor; "
        "end Dog; "
        "class C public static method Main(): int "
        "  using var d := new Dog(); "
        "  return d.DogLegs() + d.Legs(); "  // 4 + 4
        "end Main; end C;"), 8);
}

// ── §14: derived field AFTER base value confirms base offset independence ──

// A generic base field plus a non-generic derived field: base _val=3 (T), the
// derived adds its own `_tag: int` = 100. `_tag` is a plain int so `_tag + ...`
// is valid; the base `_val` is compared with `=` (intrinsic, §17.3) and gates
// the return. 103 only when both the inherited `_val` and the derived `_tag`
// hold their stored values — a swapped/overlapping offset gives a different
// result.
BEZEL_TEST(generic_base_field_585, generic_base_with_nongeneric_derived_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Container<T> "
        "  protected _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "  public method BaseGet(): T return _val; end BaseGet; "
        "  destructor end destructor; "
        "end Container; "
        "class Wrapper<T>: Container<T> "
        "  protected _tag: int; "
        "  public constructor(v: T) base(v); _tag := 100; end constructor; "
        "  public method Tagged(v: T): int "
        "    if _val = v then return _tag + 3; end; "
        "    return _tag; "  // 100 (wrong) if _val mislaid
        "  end Tagged; "
        "  destructor end destructor; "
        "end Wrapper; "
        "class C public static method Main(): int "
        "  using var w := new Wrapper<int>(3); "
        "  return w.Tagged(3); "  // 103 when _val=3 and _tag=100
        "end Main; end C;"), 103);
}
