// Codegen — LinkedList<T>.Remove(item) uses structural equality (§4.6/§8.3/§9.1).
//
// Bug #565: LinkedList<T>.Remove(item) used bz_llist_find (memcmp / pointer
// identity), so for string elements it compared bz_string* pointers rather than
// string content (§4.6), and for records with string fields it compared raw bytes
// including the pointer value rather than field-by-field structural equality (§8.3).
// The same root cause as #560 (`in` operator) and #562 (Contains).
//
// Fix: mirror the #562 fix for LinkedList<T>.Contains — walk the list node-by-node
// applying emitRemoveElementEquality for per-element comparison so that §4.6/§8.3/§9.1
// equality semantics are observed, then call bz_llist_remove_node on the found node
// (preserving existing unlink + disposal/ownership semantics).
//
// Tests:
//   §4.6:   LinkedList<string>.Remove — present (returns true, Count decremented),
//           absent (returns false, Count unchanged).
//   §8.3:   LinkedList<record-with-string-field>.Remove — present and absent.
//   Regression guards: LinkedList<int>.Remove and LinkedList<plain-record>.Remove.
//   Memory: ASan/LSan guards (Remove touches element memory; no leak/double-free).

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
// §4.6 — Bug #565: LinkedList<string>.Remove uses content equality
// ===========================================================================

// §4.6: LinkedList<string>.Remove — element present (returns true, content equality).
// Before fix: bz_llist_find compared bz_string* pointers → always false for
// separately allocated string literals with equal content.
BEZEL_TEST(llist_remove_eq_spec_4_6, linkedlist_string_remove_present_returns_true_bug565) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<string>(); "
        "  l.AddLast(\"hello\"); l.AddLast(\"world\"); "
        "  if l.Remove(\"hello\") then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §4.6: LinkedList<string>.Remove — Count decremented after successful removal.
BEZEL_TEST(llist_remove_eq_spec_4_6, linkedlist_string_remove_present_count_decremented_bug565) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<string>(); "
        "  l.AddLast(\"hello\"); l.AddLast(\"world\"); "
        "  l.Remove(\"hello\"); "
        "  return l.Count; "  // 1 after removal
        "end Main; end C;"), 1);
}

// §4.6: LinkedList<string>.Remove — element absent (returns false, Count unchanged).
BEZEL_TEST(llist_remove_eq_spec_4_6, linkedlist_string_remove_absent_returns_false_bug565) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<string>(); "
        "  l.AddLast(\"hello\"); l.AddLast(\"world\"); "
        "  if l.Remove(\"bye\") then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §4.6: LinkedList<string>.Remove — absent does not decrement Count.
BEZEL_TEST(llist_remove_eq_spec_4_6, linkedlist_string_remove_absent_count_unchanged_bug565) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<string>(); "
        "  l.AddLast(\"hello\"); l.AddLast(\"world\"); "
        "  l.Remove(\"bye\"); "
        "  return l.Count; "  // still 2
        "end Main; end C;"), 2);
}

// §4.6: LinkedList<string>.Remove — match is the last node (walks the full list).
BEZEL_TEST(llist_remove_eq_spec_4_6, linkedlist_string_remove_last_node_bug565) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<string>(); "
        "  l.AddLast(\"a\"); l.AddLast(\"b\"); l.AddLast(\"c\"); "
        "  if l.Remove(\"c\") then return l.Count; end; "  // 2
        "  return 0; "
        "end Main; end C;"), 2);
}

// ===========================================================================
// §8.3/§9.1 — Bug #565: LinkedList<record-with-string-field>.Remove
// ===========================================================================

// §8.3: LinkedList<record-with-string-field>.Remove — present (structural eq).
// Before fix: memcmp on raw bytes compared string pointer values, not content.
BEZEL_TEST(llist_remove_eq_spec_8_3_9_1, linkedlist_record_str_remove_present_bug565) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record NamedVal(Name: string, N: int) end NamedVal; "
        "class C public static method Main(): int "
        "  using var l := new LinkedList<NamedVal>(); "
        "  l.AddLast(new NamedVal(\"alice\", 1)); "
        "  l.AddLast(new NamedVal(\"bob\", 2)); "
        "  if l.Remove(new NamedVal(\"alice\", 1)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.3: LinkedList<record-with-string-field>.Remove — present, Count decremented.
BEZEL_TEST(llist_remove_eq_spec_8_3_9_1, linkedlist_record_str_remove_present_count_bug565) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record NamedVal(Name: string, N: int) end NamedVal; "
        "class C public static method Main(): int "
        "  using var l := new LinkedList<NamedVal>(); "
        "  l.AddLast(new NamedVal(\"alice\", 1)); "
        "  l.AddLast(new NamedVal(\"bob\", 2)); "
        "  l.Remove(new NamedVal(\"alice\", 1)); "
        "  return l.Count; "  // 1
        "end Main; end C;"), 1);
}

// §8.3: LinkedList<record-with-string-field>.Remove — absent (returns false).
BEZEL_TEST(llist_remove_eq_spec_8_3_9_1, linkedlist_record_str_remove_absent_bug565) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record NamedVal(Name: string, N: int) end NamedVal; "
        "class C public static method Main(): int "
        "  using var l := new LinkedList<NamedVal>(); "
        "  l.AddLast(new NamedVal(\"alice\", 1)); "
        "  l.AddLast(new NamedVal(\"bob\", 2)); "
        "  if l.Remove(new NamedVal(\"carol\", 3)) then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §8.3: record — matching name but different int → absent (returns false).
BEZEL_TEST(llist_remove_eq_spec_8_3_9_1, linkedlist_record_str_remove_diff_int_bug565) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record NamedVal(Name: string, N: int) end NamedVal; "
        "class C public static method Main(): int "
        "  using var l := new LinkedList<NamedVal>(); "
        "  l.AddLast(new NamedVal(\"alice\", 1)); "
        "  if l.Remove(new NamedVal(\"alice\", 99)) then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// ===========================================================================
// Regression guards — int and plain-record must not regress
// ===========================================================================

// §4.6/§8.3: LinkedList<int>.Remove — present (returns true, regression guard).
BEZEL_TEST(llist_remove_eq_spec_4_6, linkedlist_int_remove_present_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(7); l.AddLast(14); l.AddLast(21); "
        "  if l.Remove(14) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §4.6/§8.3: LinkedList<int>.Remove — absent (returns false, regression guard).
BEZEL_TEST(llist_remove_eq_spec_4_6, linkedlist_int_remove_absent_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(7); l.AddLast(14); l.AddLast(21); "
        "  if l.Remove(99) then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §4.6/§8.3: LinkedList<int>.Remove — Count decremented (regression guard).
BEZEL_TEST(llist_remove_eq_spec_4_6, linkedlist_int_remove_count_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(7); l.AddLast(14); l.AddLast(21); "
        "  l.Remove(14); "
        "  return l.Count; "  // 2
        "end Main; end C;"), 2);
}

// §8.3: LinkedList<record> with plain int fields — present (regression guard).
BEZEL_TEST(llist_remove_eq_spec_8_3, linkedlist_record_int_remove_present_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record Pt(X: int, Y: int) end Pt; "
        "class C public static method Main(): int "
        "  using var l := new LinkedList<Pt>(); "
        "  l.AddLast(new Pt(1, 2)); l.AddLast(new Pt(3, 4)); "
        "  if l.Remove(new Pt(3, 4)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.3: LinkedList<record> with plain int fields — absent (regression guard).
BEZEL_TEST(llist_remove_eq_spec_8_3, linkedlist_record_int_remove_absent_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record Pt(X: int, Y: int) end Pt; "
        "class C public static method Main(): int "
        "  using var l := new LinkedList<Pt>(); "
        "  l.AddLast(new Pt(1, 2)); l.AddLast(new Pt(3, 4)); "
        "  if l.Remove(new Pt(5, 6)) then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}
