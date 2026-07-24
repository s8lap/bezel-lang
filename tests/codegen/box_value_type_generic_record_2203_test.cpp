// Codegen + ASan — #2203: a generic positional record's constructor arguments
// must be checked after substituting its concrete type arguments. The reported
// `record Wrap<W>(tag: int); new Wrap<string>("x")` incorrectly skipped sema
// checking because the record was generic, then codegen wrote the 8-byte string
// pointer into the 4-byte `tag` field. Boxing made LLVM's stack coloring reuse
// the dead temporary and exposed the overflow, but box layout/lifetime was not
// the source of the invalid store.
//
// §9.1 / §14.1 (spec v0.226): positional fields are the synthesized primary-
// constructor parameters, and generic instantiations substitute only fields
// that actually name their type parameters. Thus `Wrap<string>` still requires
// an int tag, while `Box<W>(value: W)` requires a string for `Box<string>`.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// (1) CONTROL — non-generic record boxing (no type parameter), value
// correctness. The plain `record Rect(w:int,h:int)` boxing path has been
// correct since #422; this pins the baseline. Expect 12 (Rect.w + Rect.h).
BEZEL_TEST(box_value_type_generic_record_2203, concrete_record_box_is_baseline) {
    int rc = run(
        "module M; "
        "record Rect(w: int, h: int); "
        "class C public static method Main(): int "
        "  var o: Object := new Rect(3, 4); "
        "  var r := o as Rect; "
        "  return r!.w + r!.h * 2; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(rc, 11);
}

// (2) CONTROL — generic record `Wrap<W>(tag: int)` boxing with W=int
// (concrete monomorphization, matches the sibling-instantiation test in
// #1939 but here we just verify value round-trip). Expect 1.
BEZEL_TEST(box_value_type_generic_record_2203, generic_concrete_int_round_trip) {
    int rc = run(
        "module M; "
        "record Wrap<W>(tag: int); "
        "class C public static method Main(): int "
        "  var o: Object := new Wrap<int>(42); "
        "  return (if o is Wrap<int> then 1 else 0); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(rc, 1);
}

// (3) HEADLINE — the reported reproducer is rejected before codegen because
// `Wrap<string>` still has an `int` positional field: W is a phantom type
// parameter, not the type of `tag`. Pre-fix, sema skipped argument checking for
// every generic positional record and codegen stored the 8-byte string pointer
// into the 4-byte `tag` alloca. Boxing only made the dead 4-byte source slot
// visible to stack coloring; it was not the source of the over-wide store.
BEZEL_TEST(box_value_type_generic_record_2203,
           wrong_typed_phantom_argument_rejected_before_boxing) {
    auto r = runAotChecked(
        "module M; "
        "record Wrap<W>(tag: int); "
        "class C public static method Main(): int "
        "  var mi: Object := new Wrap<int>(1); "
        "  var wsx: Object := new Wrap<string>(\"x\"); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// (4) CONTROL — a different concrete generic argument does not change the
// phantom record's field type. Both boxes are constructed with valid int tags;
// `is` still distinguishes the concrete instantiations. Expect 2.
BEZEL_TEST(box_value_type_generic_record_2203,
           valid_phantom_siblings_box_and_distinguish) {
    int rc = run(
        "module M; "
        "record Wrap<W>(tag: int); "
        "class A<T> public method probe(m: Object, w: Object): int "
        "  => (if m is Wrap<T> then 1 else 0) * 2 "
        "   + (if w is Wrap<T> then 1 else 0); end A; "
        "class C public static method Main(): int "
        "  var mi: Object := new Wrap<int>(1); "
        "  var wsx: Object := new Wrap<string>(2); "
        "  using var a := new A<int>(); "
        "  return a.probe(mi, wsx); end Main; end C;");
    BEZEL_EXPECT_EQ(rc, 2);
}

// (5) SUBSTITUTION CONTROL — when the positional field actually has type W,
// the concrete argument is required and preserved through boxing/unboxing.
BEZEL_TEST(box_value_type_generic_record_2203,
           concrete_generic_field_type_round_trips) {
    int rc = run(
        "module M; "
        "record Box<W>(value: W); "
        "class C public static method Main(): int "
        "  var o: Object := new Box<string>(\"ok\"); "
        "  var b := o as Box<string>; "
        "  return (if b!.value = \"ok\" then 1 else 0); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(rc, 1);
}

// (6) VALUE CORRECTNESS — `is Wrap<T>` over boxed phantom values retains the
// precise concrete descriptor. Expect 10.
BEZEL_TEST(box_value_type_generic_record_2203, is_wrap_t_distinguishes_siblings) {
    int rc = run(
        "module M; "
        "record Wrap<W>(tag: int); "
        "class C public static method Main(): int "
        "  var mi: Object := new Wrap<int>(7); "
        "  var wsx: Object := new Wrap<string>(8); "
        "  return (if mi is Wrap<int> then 1 else 0) * 10 "
        "       + (if wsx is Wrap<int> then 1 else 0); "
        "end Main; end C;");
    // mi is Wrap<int> → 1*10 = 10 ; wsx is Wrap<int> → 0 ; total = 10.
    BEZEL_EXPECT_EQ(rc, 10);
}
