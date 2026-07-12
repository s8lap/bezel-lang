// Codegen — LinkedList<T>.Find(item) uses structural equality (§4.6/§8.3/§9.1).
//
// Bug #566: LinkedList<T>.Find(item) used bz_llist_find (memcmp / pointer
// identity), so for string elements it compared bz_string* pointers rather than
// string content (§4.6), and for records with string fields it compared raw bytes
// including the pointer value rather than field-by-field structural equality (§8.3).
// Same root cause as #560 (`in` operator), #562 (Contains), #565 (Remove).
//
// Fix: mirror the #562 fix for LinkedList<T>.Contains — walk the list node-by-node
// applying emitRemoveElementEquality for per-element comparison so that §4.6/§8.3/§9.1
// equality semantics are observed.  Return the matching LinkedListNode<T> pointer
// (or null if no match) per the stdlib contract (§4.6 Find: LinkedListNode<T>?).
//
// Tests:
//   §4.6:   LinkedList<string>.Find — present (non-null node, content equality),
//           absent (null), and match at last node (full walk).
//   §8.3:   LinkedList<record-with-string-field>.Find — present and absent.
//   Regression guards: LinkedList<int>.Find and LinkedList<plain-record>.Find.

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
// §4.6 — Bug #566: LinkedList<string>.Find uses content equality
// ===========================================================================

// §4.6: LinkedList<string>.Find — element present returns non-null node.
// Before fix: bz_llist_find compared bz_string* pointers → always null for
// separately allocated string literals with equal content.
BEZEL_TEST(llist_find_eq_spec_4_6, linkedlist_string_find_present_nonnull_bug566) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<string>(); "
        "  l.AddLast(\"hello\"); l.AddLast(\"world\"); "
        "  var n := l.Find(\"hello\"); "
        "  if n = null then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §4.6: LinkedList<string>.Find — element absent returns null.
BEZEL_TEST(llist_find_eq_spec_4_6, linkedlist_string_find_absent_null_bug566) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<string>(); "
        "  l.AddLast(\"hello\"); l.AddLast(\"world\"); "
        "  var n := l.Find(\"bye\"); "
        "  if n = null then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §4.6: LinkedList<string>.Find — match is the last node (walks full list).
BEZEL_TEST(llist_find_eq_spec_4_6, linkedlist_string_find_last_node_bug566) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<string>(); "
        "  l.AddLast(\"a\"); l.AddLast(\"b\"); l.AddLast(\"c\"); "
        "  var n := l.Find(\"c\"); "
        "  if n = null then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// ===========================================================================
// §8.3/§9.1 — Bug #566: LinkedList<record-with-string-field>.Find
// ===========================================================================

// §8.3: LinkedList<record-with-string-field>.Find — present (non-null, structural eq).
// Before fix: memcmp on raw bytes compared string pointer values, not content.
BEZEL_TEST(llist_find_eq_spec_8_3_9_1, linkedlist_record_str_find_present_bug566) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record NamedVal(Name: string, N: int) end NamedVal; "
        "class C public static method Main(): int "
        "  using var l := new LinkedList<NamedVal>(); "
        "  l.AddLast(new NamedVal(\"alice\", 1)); "
        "  l.AddLast(new NamedVal(\"bob\", 2)); "
        "  var n := l.Find(new NamedVal(\"alice\", 1)); "
        "  if n = null then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §8.3: LinkedList<record-with-string-field>.Find — absent (null).
BEZEL_TEST(llist_find_eq_spec_8_3_9_1, linkedlist_record_str_find_absent_bug566) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record NamedVal(Name: string, N: int) end NamedVal; "
        "class C public static method Main(): int "
        "  using var l := new LinkedList<NamedVal>(); "
        "  l.AddLast(new NamedVal(\"alice\", 1)); "
        "  l.AddLast(new NamedVal(\"bob\", 2)); "
        "  var n := l.Find(new NamedVal(\"carol\", 3)); "
        "  if n = null then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.3: record — matching name but different int → absent (null).
BEZEL_TEST(llist_find_eq_spec_8_3_9_1, linkedlist_record_str_find_diff_int_bug566) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record NamedVal(Name: string, N: int) end NamedVal; "
        "class C public static method Main(): int "
        "  using var l := new LinkedList<NamedVal>(); "
        "  l.AddLast(new NamedVal(\"alice\", 1)); "
        "  var n := l.Find(new NamedVal(\"alice\", 99)); "
        "  if n = null then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ===========================================================================
// Regression guards — int and plain-record must not regress
// ===========================================================================

// §4.6/§8.3: LinkedList<int>.Find — present returns non-null (regression guard).
BEZEL_TEST(llist_find_eq_spec_4_6, linkedlist_int_find_present_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(7); l.AddLast(14); l.AddLast(21); "
        "  var n := l.Find(14); "
        "  if n = null then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §4.6/§8.3: LinkedList<int>.Find — absent returns null (regression guard).
BEZEL_TEST(llist_find_eq_spec_4_6, linkedlist_int_find_absent_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(7); l.AddLast(14); l.AddLast(21); "
        "  var n := l.Find(99); "
        "  if n = null then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.3: LinkedList<record> with plain int fields — present (regression guard).
BEZEL_TEST(llist_find_eq_spec_8_3, linkedlist_record_int_find_present_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record Pt(X: int, Y: int) end Pt; "
        "class C public static method Main(): int "
        "  using var l := new LinkedList<Pt>(); "
        "  l.AddLast(new Pt(1, 2)); l.AddLast(new Pt(3, 4)); "
        "  var n := l.Find(new Pt(3, 4)); "
        "  if n = null then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §8.3: LinkedList<record> with plain int fields — absent (regression guard).
BEZEL_TEST(llist_find_eq_spec_8_3, linkedlist_record_int_find_absent_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record Pt(X: int, Y: int) end Pt; "
        "class C public static method Main(): int "
        "  using var l := new LinkedList<Pt>(); "
        "  l.AddLast(new Pt(1, 2)); l.AddLast(new Pt(3, 4)); "
        "  var n := l.Find(new Pt(5, 6)); "
        "  if n = null then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}
