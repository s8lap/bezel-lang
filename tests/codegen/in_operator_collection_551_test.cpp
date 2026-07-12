// Codegen — collection form of the `in` operator (issue #551 / §17.5).
//
// §17.5 spec (Bezel_Language_Spec_0_130.md):
//   `x in collection` evaluates to `true` if `collection.Contains(x)` is true.
//   The Contains lookup is STRUCTURAL: any accessible method named `Contains`
//   taking one compatible parameter and returning `bool` qualifies.
//   The result is `bool`. Supported receiver types include Array<T>, Stack<T>,
//   Queue<T>, LinkedList<T> (and their Owned counterparts), plus Range<T> and
//   string (already implemented). Set<T>/OwningSet<T> and Dictionary<K,V> are
//   also already implemented (§4.8/#524). This file covers the remaining cases.
//
// Before fix: lowerBinary fell through to `return nullptr` ("other `in` forms
// (collection) not yet implemented") for Array/Stack/Queue/LinkedList receivers,
// causing the JIT to produce an empty program (exitCode == -999 in our harness).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ===========================================================================
// §17.5 example 1: `42 in numbers` — Array<int> receiver
// ===========================================================================

// §17.5: element present → true.
// Pre-fix: exit -999 (codegen returned nullptr). Post-fix: exit 1.
BEZEL_TEST(in_operator_collection_spec_17_5, array_int_present) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(42); a.Add(99); "
        "  if 42 in a then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §17.5: element absent → false.
BEZEL_TEST(in_operator_collection_spec_17_5, array_int_absent) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(42); a.Add(99); "
        "  if 7 in a then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §17.5: empty array → always false.
BEZEL_TEST(in_operator_collection_spec_17_5, array_int_empty) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  if 1 in a then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §17.5 / §4.6: string element in Array<string> — content equality.
BEZEL_TEST(in_operator_collection_spec_17_5, array_string_present) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<string>(); "
        "  a.Add(\"hello\"); a.Add(\"world\"); "
        "  if \"world\" in a then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §17.5 / §4.6: string element absent — content inequality.
BEZEL_TEST(in_operator_collection_spec_17_5, array_string_absent) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<string>(); "
        "  a.Add(\"hello\"); a.Add(\"world\"); "
        "  if \"bye\" in a then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §17.5 / §8.3: record element in Array<T> — structural (field-by-field) equality.
BEZEL_TEST(in_operator_collection_spec_17_5, array_record_present) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record Pt(X: int, Y: int) end Pt; "
        "class C public static method Main(): int "
        "  using var a := new Array<Pt>(); "
        "  a.Add(new Pt(1, 2)); a.Add(new Pt(3, 4)); "
        "  if new Pt(3, 4) in a then return 1; end; "  // equal by value
        "  return 0; "
        "end Main; end C;"), 1);
}

// §17.5 / §8.3: record element absent.
BEZEL_TEST(in_operator_collection_spec_17_5, array_record_absent) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record Pt(X: int, Y: int) end Pt; "
        "class C public static method Main(): int "
        "  using var a := new Array<Pt>(); "
        "  a.Add(new Pt(1, 2)); a.Add(new Pt(3, 4)); "
        "  if new Pt(5, 6) in a then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// ===========================================================================
// §17.5: Stack<T> receiver (array-backed)
// ===========================================================================

// §17.5 + §4.11: Stack<int> — element present.
BEZEL_TEST(in_operator_collection_spec_17_5, stack_int_present) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Stack<int>(); "
        "  s.Push(1); s.Push(2); s.Push(3); "
        "  if 2 in s then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §17.5 + §4.11: Stack<int> — element absent.
BEZEL_TEST(in_operator_collection_spec_17_5, stack_int_absent) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Stack<int>(); "
        "  s.Push(1); s.Push(2); s.Push(3); "
        "  if 99 in s then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// ===========================================================================
// §17.5: Queue<T> receiver (array-backed)
// ===========================================================================

// §17.5 + §4.12: Queue<int> — element present.
BEZEL_TEST(in_operator_collection_spec_17_5, queue_int_present) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var q := new Queue<int>(); "
        "  q.Enqueue(10); q.Enqueue(20); q.Enqueue(30); "
        "  if 20 in q then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §17.5 + §4.12: Queue<int> — element absent.
BEZEL_TEST(in_operator_collection_spec_17_5, queue_int_absent) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var q := new Queue<int>(); "
        "  q.Enqueue(10); q.Enqueue(20); q.Enqueue(30); "
        "  if 99 in q then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// ===========================================================================
// §17.5: LinkedList<T> receiver (bz_llist_t-backed)
// ===========================================================================

// §17.5 + §4.6: LinkedList<int> — element present.
// bz_llist_find uses memcmp on the inline element bytes — value equality.
BEZEL_TEST(in_operator_collection_spec_17_5, linkedlist_int_present) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(7); l.AddLast(14); l.AddLast(21); "
        "  if 14 in l then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §17.5 + §4.6: LinkedList<int> — element absent.
BEZEL_TEST(in_operator_collection_spec_17_5, linkedlist_int_absent) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(7); l.AddLast(14); l.AddLast(21); "
        "  if 99 in l then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// ===========================================================================
// Regression guards — existing `in` forms must still work after this change
// ===========================================================================

// §17.5 control: `x in range` still works (inline literal fast path).
BEZEL_TEST(in_operator_collection_spec_17_5, range_literal_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  if 5 in 1..10 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  if 11 in 1..10 then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §17.5 control: `c in s` (char in string) still works.
BEZEL_TEST(in_operator_collection_spec_17_5, char_in_string_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  if 'e' in \"hello\" then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  if 'z' in \"hello\" then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §17.5 control: `elem in set` (Set<int>) still works.
BEZEL_TEST(in_operator_collection_spec_17_5, set_int_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Set<int>(); "
        "  s.Add(5); "
        "  if 5 in s then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}
