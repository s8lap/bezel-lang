// Codegen — a RECORD-typed generic field's nested member is read 0 inside a
// generic method/ctor, both standalone and through a derived class (issue #590,
// follow-on to #585; §9 records, §12 inheritance, §14 generics).
//
// BEHAVIOUR FLIP (#5449, §3.6 rule 2 / §14.2, v0.315). Every row below that
// reads `_val.X` through an UNCONSTRAINED bare `T` is now a compile-time
// rejection, not a run. The owner ruling on #5449: "A bare type parameter's
// member surface is what its constraints establish, never the concrete
// substitution's." Per §3.6 rule 2 a generic body type-checks under the
// WEAKEST assumption its constraints establish — an unconstrained `T` could be
// substituted with a type that has no member `X`, so `_val.X` is not decidable
// at the declaration site and is rejected there. Per §14.2 an unconstrained
// parameter contributes only `Object`'s surface. The codegen permissive
// backstop (the `ret 0` fall-through these rows originally pinned) is no longer
// reached for this shape: `fieldAddr`'s #590 substitution-site resolution runs
// only for a sema-RESOLVED member, and sema now rejects the program first.
//
// The original #590 rows were MISCOMPILES when written (every nested read
// evaluated 0): `_val.X` resolved against the monomorphized `T → Pt` at codegen
// while sema had left `.X` unresolved. #5449 fixes that class of silent
// wrong-value code by rejecting the program at the DECLARATION site, which is
// the §3.6 rule-2 answer. Each row below therefore has two halves:
//
//   1. `*_rejected` — the bare-`T` program must NOT run and must carry the
//      declaration-site §3.6 rule 2 / §14.2 diagnostic naming `T` and the
//      missing member. This is the row that fails without #5449 (on pre-#5449
//      code the program compiled and ran, returning 0).
//   2. `*_constrained` — the WORKING TWIN: the same read with the constraint
//      #5449's remedy names (`where T: <interface declaring the member>`) added.
//      This preserves the coverage #590 was written for — that the nested read
//      through a monomorphized generic field GEPs the SPECIALIZED record layout
//      (a second-field offset, a string pointer slot, a multi-field record) and
//      returns the right value — at the shape the spec admits.
//
// Only real stdlib surface is used. Every constrained twin is verified to
// compile WITHOUT any internal diagnostic (#2491/#2547), so no row reads its
// value through a permissive backstop.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// The declaration-site rejection (#5449, §3.6 rule 2 / §14.2, v0.315). The
// program must not run, must carry an Error, and the diagnostic must name BOTH
// the bare type parameter and the member that its constraints fail to provide.
// Asserting on the message (not merely `!ran`) keeps the row load-bearing: a
// program that failed for some UNRELATED reason would not satisfy it.
void expectBareTypeParamRejection(const std::string& prog,
                                  const std::string& member) {
    auto r = runAot(prog);
    BEZEL_EXPECT_FALSE(r.ran);           // the program never ran
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("bare type parameter") != std::string::npos &&
            d.message.find("`T`") != std::string::npos &&
            d.message.find("`" + member + "`") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);            // names T and the missing member
}

}  // namespace

// ── §9/§14: standalone generic class, record T, nested member via method ──

// new Container<Pt>(Pt(9,5)).GX() → 9 was the MINIMAL #590 reproduction: the
// nested `_val.X` on an unconstrained `T` was unresolved in sema and read 0.
// Since #5449 the program is rejected at the declaration site.
BEZEL_TEST(generic_record_field_590, standalone_record_field_nested_member_rejected) {
    expectBareTypeParamRejection(
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
        "end Main; end C;",
        "X");
}

// WORKING TWIN: `where T: IHasXY` — the constraint declares `X`, so §14.2 puts
// it in `T`'s surface and the monomorphized read GEPs the record layout. 9.
BEZEL_TEST(generic_record_field_590, standalone_record_field_nested_member_constrained) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IHasXY "
        "  readonly property X: int; "
        "end IHasXY; "
        "record Pt(X: int, Y: int): IHasXY; "
        "class Container<T> where T: IHasXY "
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
BEZEL_TEST(generic_record_field_590, derived_record_field_via_inherited_method_rejected) {
    expectBareTypeParamRejection(
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
        "end Main; end C;",
        "X");
}

// WORKING TWIN: the constrained form of the inherited-method read → 9.
BEZEL_TEST(generic_record_field_590, derived_record_field_via_inherited_method_constrained) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IHasXY "
        "  readonly property X: int; "
        "end IHasXY; "
        "record Pt(X: int, Y: int): IHasXY; "
        "class Container<T> where T: IHasXY "
        "  protected _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "  public method GX(): int return _val.X; end GX; "
        "  destructor end destructor; "
        "end Container; "
        "class Wrapper<T>: Container<T> where T: IHasXY "
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
BEZEL_TEST(generic_record_field_590, derived_record_field_via_derived_method_rejected) {
    expectBareTypeParamRejection(
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
        "end Main; end C;",
        "X");
}

// WORKING TWIN: the constrained form of the derived-method read → 9.
BEZEL_TEST(generic_record_field_590, derived_record_field_via_derived_method_constrained) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IHasXY "
        "  readonly property X: int; "
        "end IHasXY; "
        "record Pt(X: int, Y: int): IHasXY; "
        "class Container<T> where T: IHasXY "
        "  protected _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "  public method GX(): int return _val.X; end GX; "
        "  destructor end destructor; "
        "end Container; "
        "class Wrapper<T>: Container<T> where T: IHasXY "
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
BEZEL_TEST(generic_record_field_590, record_second_field_offset_rejected) {
    expectBareTypeParamRejection(
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
        "end Main; end C;",
        "Y");
}

// WORKING TWIN: the SECOND-field offset through a constrained generic field is
// 5 — the coverage #590's `record_second_field_offset` existed to pin.
BEZEL_TEST(generic_record_field_590, record_second_field_offset_constrained) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IHasXY "
        "  readonly property X: int; "
        "  readonly property Y: int; "
        "end IHasXY; "
        "record Pt(X: int, Y: int): IHasXY; "
        "class Wrapper<T> where T: IHasXY "
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
BEZEL_TEST(generic_record_field_590, record_with_string_field_int_member_rejected) {
    expectBareTypeParamRejection(
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
        "end Main; end C;",
        "Age");
}

// WORKING TWIN: the record carries a string pointer slot, so `Age` sits after
// it — the constrained read must still land on 30 + 42 == 72.
BEZEL_TEST(generic_record_field_590, record_with_string_field_int_member_constrained) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IHasNameAge "
        "  readonly property Name: string; "
        "  readonly property Age: int; "
        "end IHasNameAge; "
        "record Person(Name: string, Age: int): IHasNameAge; "
        "class Container<T> where T: IHasNameAge "
        "  protected _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "  public method GA(): int return _val.Age; end GA; "
        "  destructor end destructor; "
        "end Container; "
        "class Wrapper<T>: Container<T> where T: IHasNameAge "
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
BEZEL_TEST(generic_record_field_590, record_string_member_length_rejected) {
    expectBareTypeParamRejection(
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
        "end Main; end C;",
        "Name");
}

// WORKING TWIN: the nested chain reaches the string pointer slot and reads its
// length — "Hello".Length == 5. The heap-string pointer makes this the
// ASan-relevant row.
BEZEL_TEST(generic_record_field_590, record_string_member_length_constrained) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IHasName "
        "  readonly property Name: string; "
        "end IHasName; "
        "record Person(Name: string, Age: int): IHasName; "
        "class Wrapper<T> where T: IHasName "
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
BEZEL_TEST(generic_record_field_590, two_level_inheritance_record_field_rejected) {
    expectBareTypeParamRejection(
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
        "end Main; end C;",
        "X");
}

// WORKING TWIN: the constrained read at all three inheritance levels is 18 —
// the coverage #590's two-level row existed to pin.
BEZEL_TEST(generic_record_field_590, two_level_inheritance_record_field_constrained) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IHasXY "
        "  readonly property X: int; "
        "  readonly property Y: int; "
        "end IHasXY; "
        "record Pt(X: int, Y: int): IHasXY; "
        "class Base<T> where T: IHasXY "
        "  protected _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "  public method BX(): int return _val.X; end BX; "
        "  destructor end destructor; "
        "end Base; "
        "class Mid<T>: Base<T> where T: IHasXY "
        "  public constructor(v: T) base(v); end constructor; "
        "  public method MX(): int return _val.X; end MX; "
        "  destructor end destructor; "
        "end Mid; "
        "class Leaf<T>: Mid<T> where T: IHasXY "
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
BEZEL_TEST(generic_record_field_590, multi_field_record_all_members_rejected) {
    auto r = runAot(
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
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // Each of the three members is named — the record's whole surface, not one.
    for (const char* m : {"A", "B", "C"}) {
        bool found = false;
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.message.find("bare type parameter") != std::string::npos &&
                d.message.find("`T`") != std::string::npos &&
                d.message.find(std::string("`") + m + "`") != std::string::npos)
                found = true;
        BEZEL_EXPECT_TRUE(found);
    }
}

// WORKING TWIN: every offset of the three-member record is correct — 432 does
// not fit an 8-bit exit code, so it is observed on stdout.
BEZEL_TEST(generic_record_field_590, multi_field_record_all_members_constrained) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "interface IHasABC "
        "  readonly property A: int; "
        "  readonly property B: int; "
        "  readonly property C: int; "
        "end IHasABC; "
        "record Triple(A: int, B: int, C: int): IHasABC; "
        "class Box<T> where T: IHasABC "
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

// T = int via inherited method: still 42. These read `_val` ITSELF (not a
// nested member of it), so they were never in #590's broken set and are
// unaffected by both the #590 fix and the #5449 rejection.
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
// case stays correct). The `.Length` here is read OUTSIDE the generic body —
// on a concrete `Wrapper<string>` — so it is an ordinary member read.
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