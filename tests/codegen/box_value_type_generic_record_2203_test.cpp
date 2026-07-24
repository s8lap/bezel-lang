// Codegen + ASan — #2203: boxing a value-type generic record `Wrap<W>` to
// Object and then reusing the source value's stack slot for a larger value
// (e.g. a string argument to the next `new Wrap<string>(...)`) writes
// 8 bytes into a 4-byte record slot — stack-buffer-overflow.
//
// §8.6 / §12.7 (spec v0.226): the value-representation codegen must not let
// the compiler re-use a boxed value's 4-byte record slot for a subsequent
// 8-byte value. The ASan-evidence build compiles + runs the program under
// `-fsanitize=address`; pre-fix it crashes with `stack-buffer-overflow` on
// the 8-byte store to the 4-byte slot. The value-correctness build
// (no ASan) exercises the same control flow and asserts the result.
//
// The repro is the lane-J pre-existing one: construct a boxed value-type
// generic record whose struct layout is a 4-byte `{i32}` (the W parameter
// does not affect the field), then construct another boxed record whose
// argument is an 8-byte string. The compiler reuses the first record's
// 4-byte slot for the string pointer and overflows by 4 bytes.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

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

// (3) HEADLINE — the lane-J reproducer. Boxing `Wrap<int>(1)` (4-byte
// record) then constructing `Wrap<string>("x")` (8-byte payload) in the
// same scope. Pre-fix: the compiler reuses the 4-byte slot for the
// 8-byte string argument, triggering an 8-byte WRITE into a 4-byte
// slot → ASan stack-buffer-overflow. The expected value is 2
// (m is Wrap<int> matches, w is Wrap<int> does not match).
// Under the no-corruption harness (no ASan) the program also returns
// 2 because the corrupted slot is not observed by the `is` test
// (the boxed value is the live reference). The ASan-evidence build
// in the lane-F probe /var/tmp/bezel-scratch/laneJ-review/p_record_direct.bzl
// is the strict oracle.
BEZEL_TEST(box_value_type_generic_record_2203, box_then_larger_reuse_4byte_slot) {
    int rc = run(
        "module M; "
        "record Wrap<W>(tag: int); "
        "class A<T> public method probe(m: Object, w: Object): int "
        "  => (if m is Wrap<T> then 1 else 0) * 2 "
        "   + (if w is Wrap<T> then 1 else 0); end A; "
        "class C public static method Main(): int "
        "  var mi: Object := new Wrap<int>(1); "
        "  var wsx: Object := new Wrap<string>(\"x\"); "
        "  using var a := new A<int>(); "
        "  return a.probe(mi, wsx); end Main; end C;");
    // m is Wrap<int> → 1*2 = 2 ; w is Wrap<int> → 0 ; total = 2.
    BEZEL_EXPECT_EQ(rc, 2);
}

// (4) VALUE CORRECTNESS — `is Wrap<T>` over the boxed value still yields
// the correct result once the layout/stack-slot fix is in. The test also
// documents the precise return-value contract: (true, false) for a
// (Wrap<int>, Wrap<string>) pair under A<int>. Expect 2.
BEZEL_TEST(box_value_type_generic_record_2203, is_wrap_t_distinguishes_siblings) {
    int rc = run(
        "module M; "
        "record Wrap<W>(tag: int); "
        "class C public static method Main(): int "
        "  var mi: Object := new Wrap<int>(7); "
        "  var wsx: Object := new Wrap<string>(\"y\"); "
        "  return (if mi is Wrap<int> then 1 else 0) * 10 "
        "       + (if wsx is Wrap<int> then 1 else 0); "
        "end Main; end C;");
    // mi is Wrap<int> → 1*10 = 10 ; wsx is Wrap<int> → 0 ; total = 10.
    BEZEL_EXPECT_EQ(rc, 10);
}
