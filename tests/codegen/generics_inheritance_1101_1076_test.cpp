// Regression tests for #1101 (generics × inheritance, §14 / §14.1) and #1076
// (generic method returning a type-param bound to a generic-record spec, §14.1).
//
// #1101 — three #428-family (type-erased generics × inheritance) defects that
// only manifested through a TWO-level inheritance chain (one level already
// worked):
//   A — BAD_REJECT (sema): arithmetic on an inherited generic-T field in a
//       grandchild (`Child: Mid: Box<int>`, `return _v + 1`) reported
//       "operator + is not defined for type `?`" — substituteMemberType only
//       walked ONE base level, so the inherited T stayed unsubstituted in the
//       binary-operand typing.
//   B — WRONG_VALUE (codegen): a grandchild reading an inherited reference-typed
//       generic field (`Box<string>` `_v`) with an extra own field `_w` read the
//       WRONG GEP slot (got `_w`) — classStruct only specialized a DIRECT generic
//       base, so the grandchild kept the i32 placeholder layout and the field
//       offsets diverged from the specialized read.
//   C — IR CRASH (codegen): calling an inherited generic method via a BARE
//       identifier (`Make()` returning `T?`, implicit `this`) returned the
//       type-erased `{i8,i32}` template into a ptr-returning caller — the
//       call-site substitution setup only ran for a member-access receiver.
//
// #1076 — a generic method `Select<A,B>(a:A,b:B): A` instantiated with
//   A = Pair<int,int> built its DECLARED return type via recordStruct(Pair)
//   under the METHOD's substitution {A→Pair<int,int>, B→int}; because Pair<A,B>
//   reuses the names A/B, that re-specialized Pair's fields into
//   Pair<Pair<int,int>,int> ($spec1), mismatching the body's `ret $spec0`
//   (IR_FAIL). Fixed by lowering the resolved concrete record under its OWN args.
//
// All found by adversarial discovery sweep #8. runJit returns the FULL int from
// Main() (not 8-bit wrapped); values ≤ 255.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #1101-A: arithmetic on an inherited generic-T field through a 2-level chain
// (`Child: Mid: Box<int>`) typechecks and computes the value (was BAD_REJECT).
BEZEL_TEST(generics_inheritance_1101, grandchild_inherited_t_field_arith) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box<T> public _v: T; public constructor(v: T) _v := v; end constructor; end Box; "
        "class Mid: Box<int> public constructor(v: int) base(v); end constructor; end Mid; "
        "class Child: Mid "
        "  public constructor(v: int) base(v); end constructor; "
        "  public method Get(): int return _v + 1; end Get; "
        "end Child; "
        "class C public static method Main(): int "
        "  using var c := new Child(41); return c.Get(); "      // 41 + 1
        "end Main; end C;"), 42);
}

// #1101-A (deeper): a 3-level chain still substitutes the inherited T.
BEZEL_TEST(generics_inheritance_1101, three_level_inherited_t_field_arith) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box<T> public _v: T; public constructor(v: T) _v := v; end constructor; end Box; "
        "class A1: Box<int> public constructor(v: int) base(v); end constructor; end A1; "
        "class A2: A1 public constructor(v: int) base(v); end constructor; end A2; "
        "class A3: A2 "
        "  public constructor(v: int) base(v); end constructor; "
        "  public method Get(): int return _v + 1; end Get; "
        "end A3; "
        "class C public static method Main(): int "
        "  using var a := new A3(41); return a.Get(); "
        "end Main; end C;"), 42);
}

// #1101-A (composition): an intermediate generic link `Mid<U>: Box<U>` composes
// the substitution so the grandchild still resolves T → int.
BEZEL_TEST(generics_inheritance_1101, composed_generic_link_inherited_t_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box<T> public _v: T; public constructor(v: T) _v := v; end constructor; end Box; "
        "class Mid<U>: Box<U> public constructor(v: U) base(v); end constructor; end Mid; "
        "class Child: Mid<int> "
        "  public constructor(v: int) base(v); end constructor; "
        "  public method Get(): int return _v + 1; end Get; "
        "end Child; "
        "class C public static method Main(): int "
        "  using var c := new Child(41); return c.Get(); "
        "end Main; end C;"), 42);
}

// #1101-B: a grandchild reading an inherited REFERENCE-typed generic field
// (`Box<string>` `_v`) with its OWN extra field `_w` reads the correct slot
// (was off-by-one → returned `_w`).
BEZEL_TEST(generics_inheritance_1101, grandchild_inherited_ref_field_slot) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box<T> public _v: T; public constructor(v: T) _v := v; end constructor; end Box; "
        "class Mid: Box<string> public constructor(v: string) base(v); end constructor; end Mid; "
        "class Child: Mid "
        "  public _w: string := \"WWW\"; "
        "  public constructor(v: string) base(v); end constructor; "
        "  public method V(): string return _v; end V; "
        "  public method W(): string return _w; end W; "
        "end Child; "
        "class C public static method Main(): int "
        "  using var c := new Child(\"VV\"); "
        "  return c.V().Length * 10 + c.W().Length; "          // 2*10 + 3
        "end Main; end C;"), 23);
}

// #1101-C: an inherited generic method returning `T?` called via a BARE
// identifier (implicit `this`) returns the substituted reference (here null).
BEZEL_TEST(generics_inheritance_1101, inherited_generic_method_bare_call_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Widget public _id: int := 0; public constructor(i: int) _id := i; end constructor; end Widget; "
        "class Factory<T> public method Make(): T? return null; end Make; end Factory; "
        "class WidgetFactory: Factory<Widget> "
        "  public method Create(): Widget? return Make(); end Create; "  // bare Make()
        "end WidgetFactory; "
        "class C public static method Main(): int "
        "  using var f := new WidgetFactory(); var w := f.Create(); "
        "  return (1 if w = null else 0); "
        "end Main; end C;"), 1);
}

// #1101-C (real value): the inherited generic method returns a stored T? and the
// derived caller reads through it.
BEZEL_TEST(generics_inheritance_1101, inherited_generic_method_bare_call_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Widget public _id: int := 0; public constructor(i: int) _id := i; end constructor; end Widget; "
        "class Factory<T> "
        "  public _proto: T?; "
        "  public constructor(p: T?) _proto := p; end constructor; "
        "  public method Make(): T? return _proto; end Make; "
        "end Factory; "
        "class WidgetFactory: Factory<Widget> "
        "  public constructor(p: Widget?) base(p); end constructor; "
        "  public method Create(): Widget? return Make(); end Create; "
        "end WidgetFactory; "
        "class C public static method Main(): int "
        "  using var w := new Widget(7); "
        "  var f := new WidgetFactory(w); "
        "  var got := f.Create(); "
        "  var r := (got!._id if got <> null else -1); "       // 7
        "  f.Dispose(); return r; "
        "end Main; end C;"), 7);
}

// Regression guard (#1101-C): the explicit `this.Make()` form is unchanged.
BEZEL_TEST(generics_inheritance_1101, inherited_generic_method_explicit_this) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Widget public _id: int := 0; public constructor(i: int) _id := i; end constructor; end Widget; "
        "class Factory<T> public method Make(): T? return null; end Make; end Factory; "
        "class WidgetFactory: Factory<Widget> "
        "  public method Create(): Widget? return this.Make(); end Create; "
        "end WidgetFactory; "
        "class C public static method Main(): int "
        "  using var f := new WidgetFactory(); var w := f.Create(); "
        "  return (1 if w = null else 0); "
        "end Main; end C;"), 1);
}

// #1076: a generic method `Select<A,B>: A` instantiated with A = Pair<int,int>
// returns the canonical Pair<int,int> specialization (was IR_FAIL: $spec0 body
// vs $spec1 declared return).
BEZEL_TEST(generics_inheritance_1076, generic_method_returns_record_spec_first) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pair<A, B>(first: A, second: B); "
        "static class Utils "
        "  public static method Select<A, B>(a: A, b: B): A return a; end Select; "
        "end Utils; "
        "class C public static method Main(): int "
        "  var inner := new Pair<int, int>(3, 7); "
        "  return Utils.Select(inner, 0).first; "              // 3
        "end Main; end C;"), 3);
}

// #1076 (read both fields of the returned record spec).
BEZEL_TEST(generics_inheritance_1076, generic_method_returns_record_both_fields) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pair<A, B>(first: A, second: B); "
        "static class Utils "
        "  public static method First<A, B>(a: A, b: B): A return a; end First; "
        "end Utils; "
        "class C public static method Main(): int "
        "  var inner := new Pair<int, int>(30, 12); "
        "  var p := Utils.First(inner, 0); "
        "  return p.first + p.second; "                        // 30 + 12
        "end Main; end C;"), 42);
}

// #1076 (the SECOND type param `B` as the return — a distinct specialization).
BEZEL_TEST(generics_inheritance_1076, generic_method_returns_second_param) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pair<A, B>(first: A, second: B); "
        "static class Utils "
        "  public static method Snd<A, B>(a: A, b: B): B return b; end Snd; "
        "end Utils; "
        "class C public static method Main(): int "
        "  var inner := new Pair<int, int>(3, 7); "
        "  return Utils.Snd(inner, 42); "                      // 42
        "end Main; end C;"), 42);
}
