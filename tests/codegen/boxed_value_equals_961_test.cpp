// Codegen — Boxed value-type Equals / GetHashCode through an Object receiver
// uses structural / value equality, not reference identity (§9.1 / §9.6).
// Issue #961. Discovered as a follow-on gap from #946/#947/#948.
//
// When a value type (record, enum, primitive) is held in an Object slot it is
// boxed. #946/#947/#948 gave every box vtable the fixed kEqualsSlot /
// kGetHashCodeSlot entries so the uniform Object dispatch is in-bounds — but
// those slots held the shared box-default thunks, which implement §8.3
// REFERENCE identity / identity hash. Two distinct boxes for the same value
// therefore compared unequal even when the underlying values were equal.
//
// Fix (§9.1 / §9.6 #961): replace the shared thunks with per-type structural
// thunks:
//   • boxRecordEqualsThunk  — guard `other` has the same type descriptor,
//     load both payloads, run emitStructEquality (record) / integer ICmpEQ
//     (enum). Mismatched descriptor / null `other` → false.
//   • boxRecordHashThunk    — load payload, run emitRecordHashCode (record)
//     or emitValueHashCode (enum).
//   • primBoxEqualsThunk    — same guard on the primitive descriptor, load
//     both scalar payloads, compare by value (ICmpEQ / FCmpOEQ).
//   • primBoxHashThunk      — load scalar, run emitValueHashCode.
//
// These are installed at kEqualsSlot / kGetHashCodeSlot of the per-type box
// vtable in setupBoxDescriptors (LlvmBackend_Setup.cpp).

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

// --- Boxed record Equals (§9.1 / §9.6 / #961) ---

// #961 / §9.1: two boxes of the same record type with equal fields → Equals
// returns true (structural equality, not reference identity). This is the
// primary repro from the issue.
BEZEL_TEST(codegen_boxed_value_equals_961, record_equal_values_through_object) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(x: int, y: int); "
        "class C public static method Main(): int "
        "  var o: Object := new Pt(1, 2); "
        "  var p: Object := new Pt(1, 2); "
        "  if o.Equals(p) then return 1; end; "
        "  return 7; "
        "end Main; end C;"), 1);
}

// #961 / §9.1: two boxes with differing field values → Equals returns false.
BEZEL_TEST(codegen_boxed_value_equals_961, record_differing_values_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(x: int, y: int); "
        "class C public static method Main(): int "
        "  var o: Object := new Pt(1, 2); "
        "  var p: Object := new Pt(1, 3); "
        "  return (1 if o.Equals(p) else 0); "
        "end Main; end C;"), 0);
}

// #961 / §9.1: boxed record Equals(null) → false (null guard in thunk).
BEZEL_TEST(codegen_boxed_value_equals_961, record_equals_null_is_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(x: int, y: int); "
        "class C public static method Main(): int "
        "  var o: Object := new Pt(1, 2); "
        "  var n: Object? := null; "
        "  return (1 if o.Equals(n) else 0); "
        "end Main; end C;"), 0);
}

// #961 / §9.1: two boxes of different record types → Equals false (descriptor
// mismatch guard).
BEZEL_TEST(codegen_boxed_value_equals_961, record_different_type_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(x: int, y: int); "
        "record Vec(x: int, y: int); "
        "class C public static method Main(): int "
        "  var o: Object := new Pt(1, 2); "
        "  var v: Object := new Vec(1, 2); "
        "  return (1 if o.Equals(v) else 0); "
        "end Main; end C;"), 0);
}

// --- Boxed record GetHashCode (§9.1 / §9.6 / #961) ---

// #961 / §9.1: equal boxed records produce equal hashes.
BEZEL_TEST(codegen_boxed_value_hashcode_961, record_equal_values_same_hash) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(x: int, y: int); "
        "class C public static method Main(): int "
        "  var o: Object := new Pt(3, 4); "
        "  var p: Object := new Pt(3, 4); "
        "  if o.GetHashCode() <> p.GetHashCode() then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// #961 / §9.1: boxed record GetHashCode is consistent with direct record hash.
BEZEL_TEST(codegen_boxed_value_hashcode_961, record_hash_consistent_with_direct) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(x: int, y: int); "
        "class C public static method Main(): int "
        "  var pt := new Pt(7, 8); "
        "  var o: Object := new Pt(7, 8); "
        "  if pt.GetHashCode() <> o.GetHashCode() then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// --- Boxed primitive Equals (§9.x / §9.6 / #961) ---

// #961 / §9.1: two boxed ints with equal values → Equals true. Secondary repro
// from the issue.
BEZEL_TEST(codegen_boxed_value_equals_961, primitive_int_equal_values) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 5; "
        "  var p: Object := 5; "
        "  return (1 if o.Equals(p) else 0); "
        "end Main; end C;"), 1);
}

// #961 / §9.1: two boxed ints with different values → Equals false.
BEZEL_TEST(codegen_boxed_value_equals_961, primitive_int_differing_values_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 5; "
        "  var p: Object := 6; "
        "  return (1 if o.Equals(p) else 0); "
        "end Main; end C;"), 0);
}

// #961 / §9.1: boxed int Equals null → false.
BEZEL_TEST(codegen_boxed_value_equals_961, primitive_int_equals_null_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 42; "
        "  var n: Object? := null; "
        "  return (1 if o.Equals(n) else 0); "
        "end Main; end C;"), 0);
}

// #961 / §9.1: a boxed int and a boxed record with same bit pattern → false
// (descriptor mismatch: int vs. record).
BEZEL_TEST(codegen_boxed_value_equals_961, primitive_vs_record_type_mismatch_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Wrap(v: int); "
        "class C public static method Main(): int "
        "  var o: Object := 1; "
        "  var w: Object := new Wrap(1); "
        "  return (1 if o.Equals(w) else 0); "
        "end Main; end C;"), 0);
}

// --- Boxed primitive GetHashCode (§9.6 / #961) ---

// #961 / §9.6: equal boxed ints produce equal hashes.
BEZEL_TEST(codegen_boxed_value_hashcode_961, primitive_int_equal_values_same_hash) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 99; "
        "  var p: Object := 99; "
        "  if o.GetHashCode() <> p.GetHashCode() then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}
