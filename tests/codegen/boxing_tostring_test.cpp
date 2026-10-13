// §8.6 boxing + §8.5 ToString + §12.7 is/as, through Object (#417).
//
// A uniform virtual `ToString` slot lives in every vtable (CG35-itable + #417
// layout: [0]=td, [1]=itmap, [2]=Dispose, [3]=ToString, [4..]=virtuals). A
// call `o.ToString()` on an Object/interface receiver dispatches through that
// slot so it reaches the right implementation for a boxed primitive / boxed
// record / boxed enum / class instance; a string operand (no vtable) returns
// itself. `o is string` / `o as string` recognize a bare bz_string* in an
// Object slot via the runtime bz_obj_is_string discriminator; `o is Object`
// is true for any non-null reference (§8.1 / §12.7).
//
// The JIT harness observes only the int exit code, so each case reduces the
// produced string to an int via `.Length` or an `=` comparison, or uses the
// bool is/as result directly.

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

// ── §8.5 / §8.6 (#417): boxed PRIMITIVE ToString through Object ──────────────

// var o: Object := 42; o.ToString() = "42" (§9.1: int → decimal).
BEZEL_TEST(codegen_boxing_tostring_417, boxed_int_tostring_via_object) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 42; return (if o.ToString() = \"42\" then 1 else 0); "
        "end Main; end C;"), 1);
}

// var o: Object := 42; o.ToString().Length = 2.
BEZEL_TEST(codegen_boxing_tostring_417, boxed_int_tostring_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 42; return o.ToString().Length; "
        "end Main; end C;"), 2);
}

// var o: Object := true; o.ToString() = "True" — per stdlib bool.ToString() → "True"/"False"
// (#452: was incorrectly "true"; stdlib specifies capital first letter).
BEZEL_TEST(codegen_boxing_tostring_417, boxed_bool_tostring_via_object) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := true; return (if o.ToString() = \"True\" then 1 else 0); "
        "end Main; end C;"), 1);
}

// var o: Object := 1000000L; o.ToString().Length = 7.
BEZEL_TEST(codegen_boxing_tostring_417, boxed_long_tostring_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 1000000L; return o.ToString().Length; "
        "end Main; end C;"), 7);
}

// ── §8.5 / §8.6 (#417): boxed RECORD ToString through Object ─────────────────

// §8.5/§9.1: a positional record's ToString is "TypeName(F1: v1, F2: v2)".
// var o: Object := new Point(3, 4); o.ToString() = "Point(X: 3, Y: 4)".
BEZEL_TEST(codegen_boxing_tostring_417, boxed_record_tostring_via_object) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var o: Object := new Point(3, 4); "
        "  return (if o.ToString() = \"Point(X: 3, Y: 4)\" then 1 else 0); "
        "end Main; end C;"), 1);
}

// A boxed record reached through an INTERFACE variable (§9.6) dispatches the
// same synthesized ToString.
BEZEL_TEST(codegen_boxing_tostring_417, boxed_record_tostring_via_interface) {
    BEZEL_EXPECT_EQ(run(
        "module M; interface IShape method Area(): int; end IShape; "
        "record Rect(w: int, h: int): IShape "
        "  public method Area(): int => w * h; end Rect; "
        "class C public static method Main(): int "
        "  var s: IShape := new Rect(2, 5); "
        "  return (if s.ToString() = \"Rect(w: 2, h: 5)\" then 1 else 0); "
        "end Main; end C;"), 1);
}

// ── §8.5 / §8.6 (#417): boxed ENUM ToString through Object ───────────────────

// §8.5: an enum's default ToString is the type's qualified name.
// var o: Object := Color.Green; o.ToString() = "Color".
BEZEL_TEST(codegen_boxing_tostring_417, boxed_enum_tostring_via_object) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum Color Red := 0, Green := 1, Blue := 2; end Color; "
        "class C public static method Main(): int "
        "  var o: Object := Color.Green; "
        // #2934 (§8.5, v0.252): was "Color" — §8.5 renders an enum's
        // default as the QUALIFIED type name, boxed dispatch included.
        "  return (if o.ToString() = \"M.Color\" then 1 else 0); "
        "end Main; end C;"), 1);
}

// ── §8.5 / §8.6 (#417): CLASS ToString through Object ────────────────────────

// A class with a ToString override, called through Object, runs the override.
BEZEL_TEST(codegen_boxing_tostring_417, class_override_tostring_via_object) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Widget "
        "  public override method ToString(): string => \"W!\"; "
        "end Widget; "
        "class C public static method Main(): int "
        "  using var w: Object := new Widget(); "
        "  return (if w.ToString() = \"W!\" then 1 else 0); "
        "end Main; end C;"), 1);
}

// A class WITHOUT a ToString override, through Object → the type name (§8.5).
BEZEL_TEST(codegen_boxing_tostring_417, class_default_tostring_via_object) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Gadget end Gadget; "
        "class C public static method Main(): int "
        "  using var g: Object := new Gadget(); "
        // #2934 (§8.5, v0.252): was "Gadget" — "For classes: the type's
        // qualified name".
        "  return (if g.ToString() = \"M.Gadget\" then 1 else 0); "
        "end Main; end C;"), 1);
}

// §8.5: a ToString OVERRIDE in a derived class dispatches virtually through
// Object — the runtime object's own vtable ToString slot is reached, so a base
// and a derived instance produce their respective overrides.
BEZEL_TEST(codegen_boxing_tostring_417, class_override_virtual_dispatch_via_object) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Animal "
        "  public override method ToString(): string => \"animal\"; "
        "end Animal; "
        "class Dog : Animal "
        "  public override method ToString(): string => \"dog\"; "
        "end Dog; "
        "class C public static method Main(): int "
        "  using var a: Object := new Animal(); "
        "  using var d: Object := new Dog(); "
        "  var s := 0; "
        "  if a.ToString() = \"animal\" then s := s + 1; end; "
        "  if d.ToString() = \"dog\" then s := s + 2; end; "
        "  return s; "
        "end Main; end C;"), 3);
}

// §8.5/§9.2: a record with a USER ToString override, reached through Object,
// runs the override (not the synthesized property-listing default).
BEZEL_TEST(codegen_boxing_tostring_417, boxed_record_user_override_via_object) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Temp(C: int) "
        "  public method ToString(): string => \"hot\"; "
        "end Temp; "
        "class C public static method Main(): int "
        "  var o: Object := new Temp(99); "
        "  return (if o.ToString() = \"hot\" then 1 else 0); "
        "end Main; end C;"), 1);
}

// A ToString override reached through an INTERFACE-typed receiver dispatches to
// the override (the vtable ToString slot is shared with class dispatch).
BEZEL_TEST(codegen_boxing_tostring_417, class_override_tostring_via_interface) {
    BEZEL_EXPECT_EQ(run(
        "module M; interface IShape method Area(): int; end IShape; "
        "class Square : IShape "
        "  public method Area(): int => 9; "
        "  public override method ToString(): string => \"sq\"; "
        "end Square; "
        "class C public static method Main(): int "
        "  using var s: IShape := new Square(); "
        "  return (if s.ToString() = \"sq\" then 1 else 0); "
        "end Main; end C;"), 1);
}

// ── §8.6 / §12.7 (#417): `o.ToString()` where o holds a STRING returns it ────

BEZEL_TEST(codegen_boxing_tostring_417, string_in_object_tostring_identity) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := \"hello\"; "
        "  return (if o.ToString() = \"hello\" then 1 else 0); "
        "end Main; end C;"), 1);
}

// ── §8.6 / §12.7 (#417): `o is string` / `o as string` through Object ────────

// o holds a string → `o is string` is true.
BEZEL_TEST(codegen_boxing_isas_417, is_string_via_object_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := \"hi\"; return (if o is string then 1 else 0); "
        "end Main; end C;"), 1);
}

// o holds an int (boxed) → `o is string` is false.
BEZEL_TEST(codegen_boxing_isas_417, is_string_on_boxed_int_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 42; return (if o is string then 1 else 0); "
        "end Main; end C;"), 0);
}

// o holds a class instance → `o is string` is false.
BEZEL_TEST(codegen_boxing_isas_417, is_string_on_class_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Animal end Animal; "
        "class C public static method Main(): int "
        "  using var o: Object := new Animal(); "
        "  return (if o is string then 1 else 0); "
        "end Main; end C;"), 0);
}

// o holds a boxed record → `o is string` is false.
BEZEL_TEST(codegen_boxing_isas_417, is_string_on_boxed_record_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var o: Object := new Point(1, 2); "
        "  return (if o is string then 1 else 0); "
        "end Main; end C;"), 0);
}

// `o is not string` on a string → false; on a boxed int → true.
BEZEL_TEST(codegen_boxing_isas_417, is_not_string_on_string_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := \"x\"; return (if o is not string then 1 else 0); "
        "end Main; end C;"), 0);
}

BEZEL_TEST(codegen_boxing_isas_417, is_not_string_on_boxed_int_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 7; return (if o is not string then 1 else 0); "
        "end Main; end C;"), 1);
}

// §17.4: `o as string` on a string → the string (round-trips, length checked).
BEZEL_TEST(codegen_boxing_isas_417, as_string_via_object_roundtrips) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := \"abcd\"; var s: string? := o as string; "
        "  if s = null then return 100; end; return s!.Length; "
        "end Main; end C;"), 4);
}

// §17.4: `o as string` on a boxed int → null (incompatible).
BEZEL_TEST(codegen_boxing_isas_417, as_string_on_boxed_int_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 42; var s: string? := o as string; "
        "  return (if s = null then 1 else 0); "
        "end Main; end C;"), 1);
}

// ── §8.1 / §12.7 (#417): `o is Object` is true for any non-null reference ────

BEZEL_TEST(codegen_boxing_isas_417, is_object_on_string_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := \"y\"; return (if o is Object then 1 else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_boxing_isas_417, is_object_on_class_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Animal end Animal; "
        "class C public static method Main(): int "
        "  using var o: Object := new Animal(); "
        "  return (if o is Object then 1 else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_boxing_isas_417, is_object_on_null_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object? := null; return (if o is Object then 1 else 0); "
        "end Main; end C;"), 0);
}
