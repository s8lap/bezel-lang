// Codegen slice CG19 — `is` / `is not` / `as` type tests (§12.7). Each
// object carries a runtime type descriptor (vtable index 0) listing its
// class plus all transitive base types and implemented interfaces. A type
// test walks that descriptor for the target type's identity, so it works
// for exact types, base classes, and interfaces. `as` yields the operand
// when the test holds, else null. Observable via the int exit code (a
// conditional expression turns the bool result into 1/0).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// A small hierarchy (Animal ← Dog) plus the IShape interface on Square,
// followed by a Main body. Reused across cases.
int runHier(const std::string& mainBody) {
    return run(
        "module M; "
        "class Animal end Animal; "
        "class Dog : Animal end Dog; "
        "interface IShape method Area(): int; end IShape; "
        "class Square : IShape public method Area(): int => 4; end Square; "
        "class C public static method Main(): int " + mainBody +
        " end Main; end C;");
}

} // namespace

BEZEL_TEST(codegen_cg19, is_exact_type) {
    BEZEL_EXPECT_EQ(runHier(
        "var d := new Dog(); return (1 if d is Dog else 0);"), 1);
}

BEZEL_TEST(codegen_cg19, is_base_class) {
    BEZEL_EXPECT_EQ(runHier(
        "var d := new Dog(); return (1 if d is Animal else 0);"), 1);
}

BEZEL_TEST(codegen_cg19, is_false_for_subclass_on_base_instance) {
    BEZEL_EXPECT_EQ(runHier(
        "var a := new Animal(); return (1 if a is Dog else 0);"), 0);
}

BEZEL_TEST(codegen_cg19, is_interface) {
    BEZEL_EXPECT_EQ(runHier(
        "var s := new Square(); return (1 if s is IShape else 0);"), 1);
}

BEZEL_TEST(codegen_cg19, is_not_operator) {
    BEZEL_EXPECT_EQ(runHier(
        "var a := new Animal(); return (1 if a is not Dog else 0);"), 1);
}

BEZEL_TEST(codegen_cg19, is_via_base_typed_parameter) {
    // The static type is Animal, the runtime type is Dog → `is Dog` holds.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Animal end Animal; "
        "class Dog : Animal end Dog; "
        "class C "
        "  public static method Check(a: Animal): int "
        "    => (1 if a is Dog else 0); "
        "  public static method Main(): int "
        "    var d := new Dog(); return Check(d); "
        "  end Main; "
        "end C;"), 1);
}

BEZEL_TEST(codegen_cg19, as_valid_yields_object) {
    // `d as Animal` yields d (a Dog); `(...) is Dog` then holds.
    BEZEL_EXPECT_EQ(runHier(
        "var d := new Dog(); return (1 if (d as Animal) is Dog else 0);"), 1);
}

BEZEL_TEST(codegen_cg19, as_invalid_yields_null) {
    // `a as Dog` yields null (a is only an Animal); `null is Dog` is false.
    BEZEL_EXPECT_EQ(runHier(
        "var a := new Animal(); return (1 if (a as Dog) is Dog else 0);"), 0);
}

// ── §12.7 / §7 / issues #447 + #448: `is` on nullable VALUE types ────────────
// A nullable value type is a tagged struct {i8 hasValue; T value}, NOT a
// pointer. The generic emitIsA path is pointer-based and must NOT be called on
// a struct operand. Instead, `x is T` where x: T? reduces to `x.hasValue`.

// #448: `x is int` where x: int? = 42 → true (hasValue = 1)
BEZEL_TEST(codegen_cg19_nullable_value_is, int_nullable_present_is_true_spec_12_7_issue_448) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x: int? := 42; return (1 if x is int else 0); "
        "end Main; end C;"), 1);
}

// #448: `x is int` where x: int? = null → false (hasValue = 0)
BEZEL_TEST(codegen_cg19_nullable_value_is, int_nullable_null_is_false_spec_12_7_issue_448) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x: int? := null; return (1 if x is int else 0); "
        "end Main; end C;"), 0);
}

// #448: `x is not int` where x: int? = 42 → false
BEZEL_TEST(codegen_cg19_nullable_value_is, int_nullable_present_is_not_false_spec_12_7_issue_448) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x: int? := 42; return (1 if x is not int else 0); "
        "end Main; end C;"), 0);
}

// #448: `x is not int` where x: int? = null → true
BEZEL_TEST(codegen_cg19_nullable_value_is, int_nullable_null_is_not_true_spec_12_7_issue_448) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x: int? := null; return (1 if x is not int else 0); "
        "end Main; end C;"), 1);
}

// #448: nullable enum `c is Color` where c: Color? = Color.Green → true
BEZEL_TEST(codegen_cg19_nullable_value_is, enum_nullable_present_is_true_spec_12_7_issue_448) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Color Red := 0, Green := 1, Blue := 2; end Color; "
        "class C public static method Main(): int "
        "  var c: Color? := Color.Green; return (1 if c is Color else 0); "
        "end Main; end C;"), 1);
}

// #448: nullable enum `c is Color` where c: Color? = null → false
BEZEL_TEST(codegen_cg19_nullable_value_is, enum_nullable_null_is_false_spec_12_7_issue_448) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Color Red := 0, Green := 1, Blue := 2; end Color; "
        "class C public static method Main(): int "
        "  var c: Color? := null; return (1 if c is Color else 0); "
        "end Main; end C;"), 0);
}

// REGRESSION: nullable CLASS `d is Dog` where d: Dog? = new Dog() → true.
// A nullable CLASS type lowers to a plain pointer (null = absent); the existing
// emitIsA pointer-walk path must still work unchanged for reference types.
BEZEL_TEST(codegen_cg19_nullable_value_is, nullable_class_present_is_true_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Animal end Animal; "
        "class Dog : Animal end Dog; "
        "class C public static method Main(): int "
        "  var d: Dog? := new Dog(); return (1 if d is Dog else 0); "
        "end Main; end C;"), 1);
}

// REGRESSION: non-nullable class `is` check still works (no struct path taken).
BEZEL_TEST(codegen_cg19_nullable_value_is, nonnull_class_is_true_regression) {
    BEZEL_EXPECT_EQ(runHier(
        "var d := new Dog(); return (1 if d is Dog else 0);"), 1);
}

// REGRESSION #1071 (§8.6 / §12.7): an Object/interface operand may hold a bare
// `bz_string*`, which has NO leading vtable/descriptor word. `o is T` / `o as T`
// for a non-string target T must NOT walk the string's `char* data` word as a
// descriptor (that SIGSEGV'd); a string is never any other concrete type, so the
// result is false (`is`) / true (`is not`) / null (`as`).
BEZEL_TEST(codegen_cg19_string_operand_1071, is_value_target_on_string_is_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := \"hello\"; return (1 if o is int else 0); "
        "end Main; end C;"), 0);
}

BEZEL_TEST(codegen_cg19_string_operand_1071, is_not_value_target_on_string_is_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := \"hello\"; return (1 if o is not int else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_cg19_string_operand_1071, is_class_target_on_string_is_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Foo public x: int := 0; end Foo; "
        "class C public static method Main(): int "
        "  var o: Object := \"hello\"; return (1 if o is Foo else 0); "
        "end Main; end C;"), 0);
}

BEZEL_TEST(codegen_cg19_string_operand_1071, as_value_target_on_string_is_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := \"hello\"; return (o as int ?? 7); "
        "end Main; end C;"), 7);
}

// REGRESSION #1071: the matching cases still work — a boxed int IS int, and an
// Object holding a string IS string (the guard must not break the genuine box
// descriptor walk nor the existing `is string` fast path).
BEZEL_TEST(codegen_cg19_string_operand_1071, boxed_int_is_int_still_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 42; return (1 if o is int else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_cg19_string_operand_1071, object_holding_string_is_string_still_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := \"hello\"; return (1 if o is string else 0); "
        "end Main; end C;"), 1);
}

// ── fix #1154 (§17.4 / §8.6): `o as int?` / `o as Rec?` where the `as` TARGET
// is written already-nullable. §17.4 "the result type of `as` is always
// nullable", so the target may be written bare (`o as int`) or nullable
// (`o as int?`); both must unbox to the same tagged {hasValue, T}. A nullable
// value-type target previously double-wrapped the unboxed result — the {i8,T}
// tagged struct was inserted into the T value-slot of another {i8,T}, emitting
// `insertvalue { i8, i32 } %a, { i8, i32 } %b, 1` → IR-verify failure.

// §17.4: a boxed int, `o as int?` → Some(7); the value reads back unchanged.
BEZEL_TEST(codegen_cg19_unbox_nullable_target_1154, object_as_int_nullable_match_spec_17_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 7; var x := o as int?; "
        "  return (x! if x <> null else -1); "
        "end Main; end C;"), 7);
}

// §17.4 / §8.6: an Object holding a record, `o as int?` is null (no crash).
BEZEL_TEST(codegen_cg19_unbox_nullable_target_1154, object_as_int_nullable_nomatch_is_null_spec_17_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Rec(v: int); class C public static method Main(): int "
        "  var o: Object := new Rec(5); var x := o as int?; "
        "  return (1 if x = null else 0); "
        "end Main; end C;"), 1);
}

// §17.4 / §8.6 (issue #416): a boxed record, `o as Rec?` → Some(Rec); field reads.
BEZEL_TEST(codegen_cg19_unbox_nullable_target_1154, object_as_record_nullable_match_spec_17_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Rec(v: int); class C public static method Main(): int "
        "  var o: Object := new Rec(9); var r := o as Rec?; "
        "  return (r!.v if r <> null else -1); "
        "end Main; end C;"), 9);
}

// §17.4 / §8.6: an Object holding an int, `o as Rec?` is null (no crash).
BEZEL_TEST(codegen_cg19_unbox_nullable_target_1154, object_as_record_nullable_nomatch_is_null_spec_17_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Rec(v: int); class C public static method Main(): int "
        "  var o: Object := 7; var r := o as Rec?; "
        "  return (1 if r = null else 0); "
        "end Main; end C;"), 1);
}

// REGRESSION: a REFERENCE-type nullable target `o as Dog?` lowers to a plain
// pointer (select isA ? obj : null); the value-unbox fix must not touch it.
BEZEL_TEST(codegen_cg19_unbox_nullable_target_1154, object_as_class_nullable_still_works_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Animal end Animal; class Dog : Animal end Dog; "
        "class C public static method Main(): int "
        "  var d := new Dog(); var o: Object := d; var dd := o as Dog?; "
        "  return (1 if dd <> null else 0); "
        "end Main; end C;"), 1);
}
