// Codegen tests for §9.1 / §9.6 (#2176, spec v0.226): boxed GENERIC record
// structural Equals/GetHashCode must compare the payload through the
// INSTANTIATED layout, not the per-declaration TEMPLATE layout.
//
// §9.1 ("Positional records"): the compiler generates "Value equality (=, <>)
// based on all positional fields" and "GetHashCode based on all positional
// fields". §9.6 routes Object-dispatched equality on a record box through the
// same structural semantics (§8.6 boxing; the auto-equality of §9.2 is the
// single normative statement for both `=` and `Equals`).
//
// Pre-fix, boxRecordEqualsThunk/boxRecordHashThunk were emitted ONCE per record
// DECLARATION against recordStruct(ts) with NO active generic substitution, so
// a type-parameter field degraded to its erased i32 placeholder. Two same-value
// boxed `Pair<string,int>("ab",2)` then compared the LOW 32 BITS of two
// distinct "ab" allocations — reporting them UNEQUAL (probe
// /var/tmp/bezel-scratch/laneE/p_boxeq_str.bzl returned 0; expected 10).
// `Pair<int,int>` (template-compatible layout) was unaffected, which is why
// #1939's boxed_equals_same_vs_sibling_9_1 did not surface this.
//
// The fix mints per-instantiation Equals/GetHashCode thunks for constructed
// generic records (mirroring #1939's specializedBoxVtableFor): the specialized
// box vtable's kEqualsSlot/kGetHashCodeSlot are replaced with thunks compiled
// under the substituted genericSubst_, so recordStruct/recordMemberTypeResolved
// resolve the concrete (per-instantiation) field layout. Non-generic records
// keep the shared declaration-level thunk (template == concrete for them).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// (1) HEADLINE — the issue repro. Two same-value boxed `Pair<string,int>`
// must be EQUAL via Object-dispatched Equals; two different-value boxes must
// be UNEQUAL. r1 = same-value-equal (1), r2 = diff-value-equal (0) → 10.
// FAILS WITHOUT the fix: returns 0 (the string field compares erased pointer
// words, so even equal pairs are unequal).
BEZEL_TEST(boxed_generic_record_equals_2176,
           same_value_string_field_pair_equal_9_1_9_6) {
    int rc = run(
        "module M; "
        "record Pair<A, B>(first: A, second: B); "
        "class C public static method Main(): int "
        "  var p1: Pair<string, int> := new Pair<string, int>(\"ab\", 2); "
        "  var p2: Pair<string, int> := new Pair<string, int>(\"ab\", 2); "
        "  var p3: Pair<string, int> := new Pair<string, int>(\"cd\", 2); "
        "  var o1: Object := p1; var o2: Object := p2; var o3: Object := p3; "
        "  var r1 := (if o1.Equals(o2) then 1 else 0); "
        "  var r2 := (if o1.Equals(o3) then 1 else 0); "
        "  return r1 * 10 + r2; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(rc, 10);
}

// (2) HASH CONSISTENCY — §9.1 "GetHashCode based on all positional fields":
// two EQUAL boxed `Pair<string,int>` must produce the SAME hash. Equal values
// implying equal hashes is the hash invariant (§9.2 auto-equality; §8.3).
// Without the fix the erased pointer-word hash distinguished them.
// Returns 1 when h1 = h2, else 0.
BEZEL_TEST(boxed_generic_record_equals_2176,
           same_value_string_field_pair_equal_hash_9_1) {
    int rc = run(
        "module M; "
        "record Pair<A, B>(first: A, second: B); "
        "class C public static method Main(): int "
        "  var p1: Pair<string, int> := new Pair<string, int>(\"ab\", 2); "
        "  var p2: Pair<string, int> := new Pair<string, int>(\"ab\", 2); "
        "  var o1: Object := p1; var o2: Object := p2; "
        "  var h1 := o1.GetHashCode(); "
        "  var h2 := o2.GetHashCode(); "
        "  return (if h1 = h2 then 1 else 0); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(rc, 1);
}

// (3) SIBLING INSTANTIATION still compares false (the #1939 descriptor gate):
// a boxed `Pair<string,int>` must NOT be equal to a boxed `Pair<int,string>`
// even when their erased template layouts coincide — they are different
// runtime types. r1 = same-inst same-value (1), r2 = sibling-inst (0) → 10.
BEZEL_TEST(boxed_generic_record_equals_2176,
           sibling_instantiation_not_equal_12_9) {
    int rc = run(
        "module M; "
        "record Pair<A, B>(first: A, second: B); "
        "class C public static method Main(): int "
        "  var p: Pair<string, int> := new Pair<string, int>(\"ab\", 2); "
        "  var q: Pair<int, string> := new Pair<int, string>(2, \"ab\"); "
        "  var o1: Object := p; var o2: Object := q; "
        "  var r1 := (if o1.Equals(o1) then 1 else 0); "
        "  var r2 := (if o1.Equals(o2) then 1 else 0); "
        "  return r1 * 10 + r2; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(rc, 10);
}

// (4) NON-STRING layout-differing instantiation: a `Pair<long,int>` field
// (i64 first) also has a concrete layout distinct from the i32-erased
// template; two same-value boxes must compare equal. Returns 10.
BEZEL_TEST(boxed_generic_record_equals_2176,
           long_first_field_layout_differs_equal_9_1) {
    int rc = run(
        "module M; "
        "record Pair<A, B>(first: A, second: B); "
        "class C public static method Main(): int "
        "  var p1: Pair<long, int> := new Pair<long, int>(9223372036854775807L, 4); "
        "  var p2: Pair<long, int> := new Pair<long, int>(9223372036854775807L, 4); "
        "  var p3: Pair<long, int> := new Pair<long, int>(0L, 4); "
        "  var o1: Object := p1; var o2: Object := p2; var o3: Object := p3; "
        "  var r1 := (if o1.Equals(o2) then 1 else 0); "
        "  var r2 := (if o1.Equals(o3) then 1 else 0); "
        "  return r1 * 10 + r2; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(rc, 10);
}

// (5) CONTROL — non-generic record with a string field: boxed structural
// equality was already correct (the shared thunk's recordStruct(decl) is the
// concrete layout when the record has no generic parameters). Pins the
// baseline so the fix cannot regress it. Returns 10.
BEZEL_TEST(boxed_generic_record_equals_2176,
           nongeneric_string_field_record_baseline_9_1) {
    int rc = run(
        "module M; "
        "record Named(name: string, n: int); "
        "class C public static method Main(): int "
        "  var p1: Named := new Named(\"ab\", 2); "
        "  var p2: Named := new Named(\"ab\", 2); "
        "  var p3: Named := new Named(\"cd\", 2); "
        "  var o1: Object := p1; var o2: Object := p2; var o3: Object := p3; "
        "  var r1 := (if o1.Equals(o2) then 1 else 0); "
        "  var r2 := (if o1.Equals(o3) then 1 else 0); "
        "  return r1 * 10 + r2; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(rc, 10);
}

// (6) NESTED generic record — the per-instantiation thunk's recordStruct
// resolves the OUTER field's substituted type (Box<int>) and emitStructEquality
// recurses through it. lowerTypeRef saves/restores the subst when lowering the
// nested Box<int>, so the outer Box<Box<int>>'s spec struct embeds the inner
// Box<int> struct (no infinite recursion). Two same-value nested boxes must
// compare equal; different inner values must compare unequal. Returns 10.
BEZEL_TEST(boxed_generic_record_equals_2176,
           nested_generic_record_equal_9_1) {
    int rc = run(
        "module M; "
        "record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  var b1: Box<Box<int>> := new Box<Box<int>>(new Box<int>(7)); "
        "  var b2: Box<Box<int>> := new Box<Box<int>>(new Box<int>(7)); "
        "  var b3: Box<Box<int>> := new Box<Box<int>>(new Box<int>(9)); "
        "  var o1: Object := b1; var o2: Object := b2; var o3: Object := b3; "
        "  var r1 := (if o1.Equals(o2) then 1 else 0); "
        "  var r2 := (if o1.Equals(o3) then 1 else 0); "
        "  return r1 * 10 + r2; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(rc, 10);
}
