// Value-type BOXING (§8.6) in the LLVM backend. When a value type (record,
// primitive, enum, nullable value) is used where `Object` or an interface it
// implements is expected, the value is boxed: a heap wrapper holds the value,
// with a leading descriptor/vtable pointer so `is` (§12.7), `as` (§17.4), and
// virtual dispatch recognize the boxed value's dynamic type uniformly with
// class instances. `as` to a value type unboxes (§8.6 "Unboxing reverses the
// process"; §17.4 "the boxed Object input is unboxed to the value type if
// compatible").
//
// REGRESSION GUARDS: the #399 reference-type Object path (a class instance
// coerced to Object is the bare pointer, NO box) must keep working: `o is
// Animal`, `o as Animal`, and dispatch on an interface-typed class instance.
//
// The JIT harness observes only the int exit code; ToString/Console output
// (finding [07]) is not exercised here. Each test names the finding
// ([05]..[14]) and spec section it derives from.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

// A module with a Rect record (implements IShape) and a Box record, plus a
// Main body. Reused across record-boxing cases.
int runRec(const std::string& mainBody) {
    return run(
        "module M; "
        "interface IShape method Area(): int; end IShape; "
        "record Rect(w: int, h: int): IShape "
        "  public method Area(): int => w * h; "
        "end Rect; "
        "record Box(V: int); "
        "class C public static method Main(): int " + mainBody +
        " end Main; end C;");
}

} // namespace

// ---- finding [06] / §8.6 / §12.7: `is` on a boxed record ------------------

BEZEL_TEST(codegen_boxing_06, is_record_via_object_true) {
    // var o: Object := new Rect(3,4); o is Rect → true. Was 0 (false).
    BEZEL_EXPECT_EQ(runRec(
        "var o: Object := new Rect(3, 4); return (1 if o is Rect else 0);"), 1);
}

BEZEL_TEST(codegen_boxing_06, is_record_via_interface_true) {
    // §9.6: a record boxed into an interface variable retains its type.
    BEZEL_EXPECT_EQ(runRec(
        "var s: IShape := new Rect(3, 4); return (1 if s is Rect else 0);"), 1);
}

BEZEL_TEST(codegen_boxing_06, is_record_wrong_type_false) {
    // The boxed value's dynamic type is Rect, not Box → `is Box` is false.
    BEZEL_EXPECT_EQ(runRec(
        "var o: Object := new Rect(3, 4); return (1 if o is Box else 0);"), 0);
}

BEZEL_TEST(codegen_boxing_06, is_not_record_false_branch) {
    // `o is not Rect` on a boxed Rect → false.
    BEZEL_EXPECT_EQ(runRec(
        "var o: Object := new Rect(3, 4); return (1 if o is not Rect else 0);"),
        0);
}

// ---- finding [12] / §8.6 / §12.7: `is int` / `is string` on a box ---------

BEZEL_TEST(codegen_boxing_12, is_int_via_object_true) {
    // var o: Object := 42; o is int → true. Was always false.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "var o: Object := 42; if o is int then return 7; else return 99; end; "
        "end Main; end C;"), 7);
}

BEZEL_TEST(codegen_boxing_12, is_int_wrong_target_false) {
    // A boxed int is not a bool.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "var o: Object := 42; return (1 if o is bool else 0); "
        "end Main; end C;"), 0);
}

BEZEL_TEST(codegen_boxing_12, is_bool_via_object_true) {
    // A boxed bool is a bool.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "var o: Object := true; return (1 if o is bool else 0); "
        "end Main; end C;"), 1);
}

// DEFERRED: `o is string` (finding [12], string sub-case). A string stored in
// Object is a bare bz_string* with no vtable/descriptor word, so the emitIsA
// descriptor walk cannot run on it; correct `is string` needs a string runtime
// type tag that does not exist. lowerTypeTest returns nullptr for a string
// target (the prior non-crashing fallback) rather than dereferencing it.

// ---- finding [08] / §8.6 / §17.4: `as` unbox of a record round-trips ------
// The success path (a non-null boxed Rect unboxes to its fields) is exercised
// here. The FAILURE path (`o as Box` → null) and the `r = null` comparison
// depend on a tagged nullable-record representation (finding [11]) which is
// deferred; this milestone fixes the unbox itself.

BEZEL_TEST(codegen_boxing_08, as_record_roundtrips_fields) {
    // var o: Object := new Rect(3,4); var r := o as Rect; r!.w + r!.h*10 = 43.
    // Was: a non-null cast yielding garbage field reads.
    BEZEL_EXPECT_EQ(runRec(
        "var o: Object := new Rect(3, 4); var r := o as Rect; "
        "return r!.w + r!.h * 10;"), 43);
}

// ---- finding [13] / §8.6 / §17.4: `as int` unbox round-trips --------------

BEZEL_TEST(codegen_boxing_13, as_int_roundtrips) {
    // var o: Object := 42; var n := o as int; return n!  → 42. Was garbage.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "var o: Object := 42; var n: int? := o as int; "
        "if n = null then return 100; end; return n!; "
        "end Main; end C;"), 42);
}

BEZEL_TEST(codegen_boxing_13, as_int_wrong_target_null) {
    // A boxed int unboxed as bool → null (incompatible).
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "var o: Object := 42; var b: bool? := o as bool; "
        "return (1 if b = null else 0); "
        "end Main; end C;"), 1);
}

// ---- finding [05] / §9.6 / §8.6: interface dispatch on a boxed record -----

BEZEL_TEST(codegen_boxing_05, interface_method_on_boxed_record) {
    // var s: IShape := new Rect(3,4); s.Area() → 12. Was a SIGSEGV.
    BEZEL_EXPECT_EQ(runRec(
        "var s: IShape := new Rect(3, 4); return s.Area();"), 12);
}

BEZEL_TEST(codegen_boxing_05, boxed_record_with_aligned_field_roundtrip) {
    // A record with an 8-byte-aligned (long) field round-trips through the box
    // (§8.6) and interface dispatch reaches its method — exercises the box
    // {vtable*, value} layout and the this-offset thunk on a wider aggregate.
    BEZEL_EXPECT_EQ(run(
        "module M; interface IVal method Get(): long; end IVal; "
        "record R(a: long, b: int): IVal "
        "  public method Get(): long => a + b; end R; "
        "class C public static method Main(): int "
        "  var o: Object := new R(5000000000L, 7); var r := o as R; "
        "  var s: IVal := new R(40L, 2); "
        "  return int(r!.a / 1000000000L) + r!.b + int(s.Get()); "
        "end Main; end C;"), 54);
}

// ---- finding [10] / §8.6 / §8.4: nullable value boxed into Object? --------
// §8.6 Nullable value-type boxing: a null T? boxes to a null Object reference;
// a non-null T? boxes to a boxed T. The `o = null` / `o <> null` comparison was
// aborting the compiler (checkGEPType) because the int? was not actually boxed.

BEZEL_TEST(codegen_boxing_10, null_int_boxed_into_object_is_null) {
    // var a: int? := null; var o: Object? := a; o = null → true. Was a crash.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "var a: int? := null; var o: Object? := a; "
        "if o = null then return 1; end; return 0; "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_boxing_10, nonnull_int_boxed_into_object_not_null) {
    // var a: int? := 7; var o: Object? := a; o = null → false (boxed, non-null).
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "var a: int? := 7; var o: Object? := a; "
        "if o = null then return 1; end; return 0; "
        "end Main; end C;"), 0);
}

BEZEL_TEST(codegen_boxing_10, nonnull_int_boxed_into_object_ne_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "var a: int? := 7; var o: Object? := a; "
        "if o <> null then return 2; end; return 0; "
        "end Main; end C;"), 2);
}

// ---- REGRESSION GUARDS: #399 reference-type Object path -------------------

BEZEL_TEST(codegen_boxing_regress, ref_is_class_via_object) {
    // A class instance coerced to Object is the bare pointer (no box).
    BEZEL_EXPECT_EQ(run(
        "module M; class Animal end Animal; class Dog : Animal end Dog; "
        "class C public static method Main(): int "
        "using var d: Object := new Dog(); return (1 if d is Animal else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_boxing_regress, ref_as_class_via_object) {
    // `o as Dog` on a boxed-into-Object Dog yields the pointer; `is Dog` holds.
    BEZEL_EXPECT_EQ(run(
        "module M; class Animal end Animal; class Dog : Animal end Dog; "
        "class C public static method Main(): int "
        "using var o: Object := new Dog(); "
        "return (1 if (o as Dog) is Dog else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_boxing_regress, ref_object_null_is_class) {
    // A null Object is not a Dog.
    BEZEL_EXPECT_EQ(run(
        "module M; class Animal end Animal; class Dog : Animal end Dog; "
        "class C public static method Main(): int "
        "var o: Object? := null; return (1 if o is Dog else 0); "
        "end Main; end C;"), 0);
}

BEZEL_TEST(codegen_boxing_regress, ref_interface_dispatch_class) {
    // Interface dispatch on a class instance stored in an interface variable.
    BEZEL_EXPECT_EQ(run(
        "module M; interface IShape method Area(): int; end IShape; "
        "class Square : IShape public method Area(): int => 9; end Square; "
        "class C public static method Main(): int "
        "using var s: IShape := new Square(); return s.Area(); "
        "end Main; end C;"), 9);
}

// #1279 (§8.6 / §14.1): a generic value-type parameter returned where Object /
// Object? is expected (`Box<T>(x: T?): Object? where T: value  return x;`) must
// be boxed like its concrete monomorphized type. Under monomorphization the
// source's sema type is still the bare parameter `T?` (declaration null), which
// the box guard did not recognise as a value type — so the box was skipped and
// the raw `{i8, int}` value was returned into the `ptr` return (IR-verify fail).
// The parameter is now resolved to its concrete value type before boxing; the
// result is a genuine box that round-trips through `is`/`as`.
BEZEL_TEST(codegen_boxing_regress, generic_nullable_value_param_boxed_return_1279) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class U "
        "  public static method Box<T>(x: T?): Object? where T: value "
        "    return x; end Box; end U; "
        "class C public static method Main(): int "
        "  var x: int? := 33; var o := U.Box<int>(x); "
        "  return (33 if o is not null else -1); "
        "end Main; end C;"), 33);
}

// #1279 non-nullable variant: `Box<T>(x: T): Object where T: value` boxes a bare
// generic value, and the box round-trips (unbox via `as int`).
BEZEL_TEST(codegen_boxing_regress, generic_value_param_boxed_return_roundtrips_1279) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class U "
        "  public static method Box<T>(x: T): Object where T: value "
        "    return x; end Box; end U; "
        "class C public static method Main(): int "
        "  var o := U.Box<int>(41); "
        "  if o is int then var n := o as int; return n! + 1; end; "  // 42
        "  return -1; "
        "end Main; end C;"), 42);
}
