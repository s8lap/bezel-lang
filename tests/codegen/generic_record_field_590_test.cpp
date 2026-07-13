// Codegen — a RECORD-typed generic field's nested member is read 0 inside a
// generic method/ctor, both standalone and through a derived class (issue #590,
// follow-on to #585; §9 records, §12 inheritance, §14 generics).
//
// HIGH miscompile. A field `_val: T` of a generic class, instantiated with a
// RECORD type argument (`T = Pt`), reads its nested members as 0 when accessed
// as `_val.X` inside a method or constructor:
//
//   record Pt(X: int, Y: int);
//   class Container<T> protected _val: T;
//       public method GX(): int return _val.X; end GX; end Container;
//   class Wrapper<T>: Container<T> ... method WX(): int return _val.X; end ...
//
//   new Container<Pt>(Pt(9,0)).GX()  → 0   (expected 9)  ← standalone, ALSO broken
//   new Wrapper<Pt>(Pt(9,0)).GX()    → 0   (expected 9)
//   new Wrapper<Pt>(Pt(9,0)).WX()    → 0   (expected 9)
//
// Root cause: sema types the field `_val` as the bare generic parameter `T`,
// which has no members at the declaration site, so the nested `.X` access is
// left UNRESOLVED (no member symbol). At codegen the monomorphized method body
// is emitted with the active substitution binding `T → Pt`, but fieldAddr had
// no resolved member to GEP, so `_val.X` fell through to a `ret 0` stub. The
// fix resolves the nested member by NAME against the concrete record type that
// `genericSubstSemaTy_` binds `T` to for this instantiation, then GEPs the
// specialized record layout. #585 (which monomorphized the inherited-FIELD
// LAYOUT and ran the ctor chain) is a prerequisite but did not cover the
// nested-member READ on a record-typed `T`.
//
// All tests are load-bearing: each reads 0 before the fix and the correct value
// after. Regressions pin the #585 int/string-`T` cases and the standalone
// Container<Pt> control. Only real stdlib surface is used.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── §9/§14: standalone generic class, record T, nested member via method ──

// new Container<Pt>(Pt(9,5)).GX() → 9. The standalone case is ALSO miscompiled
// (the nested `_val.X` is unresolved regardless of inheritance), so this is the
// minimal reproduction, not merely a control.
BEZEL_TEST(generic_record_field_590, standalone_record_field_nested_member) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class Container<T> "
        "  protected _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "  public method GX(): int return _val.X; end GX; "
        "  destructor end destructor; "
        "end Container; "
        "class C public static method Main(): int "
        "  using var c := new Container<Pt>(new Pt(9, 5)); "
        "  return c.GX(); "
        "end Main; end C;"), 9);
}

// ── §12/§14: the issue — record T field read THROUGH the derived ──

// new Wrapper<Pt>(Pt(9,5)).GX() → 9 (the INHERITED method reads the record's X).
BEZEL_TEST(generic_record_field_590, derived_record_field_via_inherited_method) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class Container<T> "
        "  protected _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "  public method GX(): int return _val.X; end GX; "
        "  destructor end destructor; "
        "end Container; "
        "class Wrapper<T>: Container<T> "
        "  public constructor(v: T) base(v); end constructor; "
        "  public method WX(): int return _val.X; end WX; "
        "  destructor end destructor; "
        "end Wrapper; "
        "class C public static method Main(): int "
        "  using var w := new Wrapper<Pt>(new Pt(9, 5)); "
        "  return w.GX(); "
        "end Main; end C;"), 9);
}

// new Wrapper<Pt>(Pt(9,5)).WX() → 9 (a DERIVED method reads the inherited
// record-typed field's nested member).
BEZEL_TEST(generic_record_field_590, derived_record_field_via_derived_method) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class Container<T> "
        "  protected _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "  public method GX(): int return _val.X; end GX; "
        "  destructor end destructor; "
        "end Container; "
        "class Wrapper<T>: Container<T> "
        "  public constructor(v: T) base(v); end constructor; "
        "  public method WX(): int return _val.X; end WX; "
        "  destructor end destructor; "
        "end Wrapper; "
        "class C public static method Main(): int "
        "  using var w := new Wrapper<Pt>(new Pt(9, 5)); "
        "  return w.WX(); "
        "end Main; end C;"), 9);
}

// ── §9: SECOND record field — offset of the non-first member must be right ──

// _val.Y reads the second record member (Pt(9,5).Y == 5). A wrong record-struct
// identity would mislay every offset after the first; reading Y pins the layout.
BEZEL_TEST(generic_record_field_590, record_second_field_offset) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class Wrapper<T> "
        "  protected _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "  public method WY(): int return _val.Y; end WY; "
        "  destructor end destructor; "
        "end Wrapper; "
        "class C public static method Main(): int "
        "  using var w := new Wrapper<Pt>(new Pt(9, 5)); "
        "  return w.WY(); "
        "end Main; end C;"), 5);
}

// ── §9/§14: record T carrying a STRING field (pointer slot) ──

// T = Person(Name: string, Age: int): the nested `_val.Age` is read both
// standalone (Container<Person>) and through the derived (Wrapper<Person>).
// 30 + 42 == 72. The record carries a string so the int field sits AFTER a
// pointer slot — the offset must account for it.
BEZEL_TEST(generic_record_field_590, record_with_string_field_int_member) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Person(Name: string, Age: int); "
        "class Container<T> "
        "  protected _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "  public method GA(): int return _val.Age; end GA; "
        "  destructor end destructor; "
        "end Container; "
        "class Wrapper<T>: Container<T> "
        "  public constructor(v: T) base(v); end constructor; "
        "  public method WA(): int return _val.Age; end WA; "
        "  destructor end destructor; "
        "end Wrapper; "
        "class C public static method Main(): int "
        "  using var c := new Container<Person>(new Person(\"Ann\", 30)); "
        "  using var w := new Wrapper<Person>(new Person(\"Bob\", 42)); "
        "  return c.GA() + w.WA(); "
        "end Main; end C;"), 72);
}

// The string member ITSELF: read `_val.Name.Length` through the derived. The
// record's string field is a pointer slot; the nested chain must reach it and
// then read the string length. "Hello".Length == 5. (ASan-relevant: the record
// carries a heap-string pointer.)
BEZEL_TEST(generic_record_field_590, record_string_member_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Person(Name: string, Age: int); "
        "class Wrapper<T> "
        "  protected _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "  public method NameLen(): int return _val.Name.Length; end NameLen; "
        "  destructor end destructor; "
        "end Wrapper; "
        "class C public static method Main(): int "
        "  using var w := new Wrapper<Person>(new Person(\"Hello\", 1)); "
        "  return w.NameLen(); "
        "end Main; end C;"), 5);
}

// ── §12/§14: two-level generic inheritance with a record T ──

// Leaf<Pt> : Mid<Pt> : Base<Pt> — the record's nested member must read the same
// value at all three levels. Base.BX + Mid.MX + Leaf.LX, each reading _val.X == 6
// → 18. Pins that the inherited record-typed field lands correctly two levels
// down (the #585 prefix-layout fix) AND each level's nested read resolves.
BEZEL_TEST(generic_record_field_590, two_level_inheritance_record_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class Base<T> "
        "  protected _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "  public method BX(): int return _val.X; end BX; "
        "  destructor end destructor; "
        "end Base; "
        "class Mid<T>: Base<T> "
        "  public constructor(v: T) base(v); end constructor; "
        "  public method MX(): int return _val.X; end MX; "
        "  destructor end destructor; "
        "end Mid; "
        "class Leaf<T>: Mid<T> "
        "  public constructor(v: T) base(v); end constructor; "
        "  public method LX(): int return _val.X; end LX; "
        "  destructor end destructor; "
        "end Leaf; "
        "class C public static method Main(): int "
        "  using var l := new Leaf<Pt>(new Pt(6, 0)); "
        "  return l.BX() + l.MX() + l.LX(); "
        "end Main; end C;"), 18);
}

// ── §9: multi-field record (three members), read each ──

// A record with three int members; read all three through the derived to pin
// that every offset is correct, not only the first. Triple(2,3,4): 2+3*10+4*100
// = 432.
BEZEL_TEST(generic_record_field_590, multi_field_record_all_members) {
    // 432 does not fit an 8-bit exit code — observe on stdout.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "record Triple(A: int, B: int, C: int); "
        "class Box<T> "
        "  protected _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "  public method Sum(): int return _val.A + _val.B * 10 + _val.C * 100; "
        "  end Sum; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box<Triple>(new Triple(2, 3, 4)); "
        "  Console.WriteLine(b.Sum()); return 0; "
        "end Main; end C;"), "432");
}

// ── §14 regression: the #585 int/string-T cases stay correct ──

// T = int via inherited method: still 42 (not re-broken by the #590 fix, which
// only adds a fallback when sema left the member unresolved — a primitive `T`
// has no nested member, so this path is unaffected).
BEZEL_TEST(generic_record_field_590, regression_585_int_T) {
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

// T = string via inherited method: "hello".Length == 5 (the #585 pointer-field
// case stays correct).
BEZEL_TEST(generic_record_field_590, regression_585_string_T) {
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
