// Codegen — LinkedList<T>.Contains(item) uses structural equality (§4.6/§8.3/§9.1).
//
// Bug #562: LinkedList<T>.Contains(item) used bz_llist_find (memcmp / pointer
// identity), so for string elements it compared bz_string* pointers rather than
// string content (§4.6), and for records with string fields it compared raw bytes
// including the pointer value rather than field-by-field structural equality (§8.3).
//
// Fix: mirror the #560 fix for `x in LinkedList<T>` — walk the list node-by-node
// applying emitRemoveElementEquality for per-element comparison (the same function
// used by Array.Contains, Remove, etc.) so that §4.6/§8.3/§9.1 equality semantics
// are observed for every element type.
//
// Tests:
//   §4.6:   LinkedList<string>.Contains — present (content eq) and absent.
//   §8.3:   LinkedList<record-with-string-field>.Contains — present and absent.
//   Regression guards: LinkedList<int>.Contains, LinkedList<plain-record>.Contains.

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
// §4.6 — Bug #562: LinkedList<string>.Contains uses content equality
// ===========================================================================

// §4.6: LinkedList<string>.Contains — element present (content equality).
// Before fix: bz_llist_find compared bz_string* pointers → always false for
// separately allocated string literals with equal content.
BEZEL_TEST(llist_contains_eq_spec_4_6, linkedlist_string_contains_present_bug562) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<string>(); "
        "  l.AddLast(\"hello\"); l.AddLast(\"world\"); "
        "  if l.Contains(\"hello\") then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §4.6: LinkedList<string>.Contains — element absent.
BEZEL_TEST(llist_contains_eq_spec_4_6, linkedlist_string_contains_absent_bug562) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<string>(); "
        "  l.AddLast(\"hello\"); l.AddLast(\"world\"); "
        "  if l.Contains(\"bye\") then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §4.6: LinkedList<string>.Contains — match is the last node (walks the full list).
BEZEL_TEST(llist_contains_eq_spec_4_6, linkedlist_string_contains_last_node_bug562) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<string>(); "
        "  l.AddLast(\"a\"); l.AddLast(\"b\"); l.AddLast(\"c\"); "
        "  if l.Contains(\"c\") then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ===========================================================================
// §8.3/§9.1 — Bug #562: LinkedList<record-with-string-field>.Contains
// ===========================================================================

// §8.3: LinkedList<record-with-string-field>.Contains — present (structural eq).
// Before fix: memcmp on raw bytes compared string pointer values, not content.
BEZEL_TEST(llist_contains_eq_spec_8_3_9_1, linkedlist_record_str_contains_present_bug562) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record NamedVal(Name: string, N: int) end NamedVal; "
        "class C public static method Main(): int "
        "  using var l := new LinkedList<NamedVal>(); "
        "  l.AddLast(new NamedVal(\"alice\", 1)); "
        "  l.AddLast(new NamedVal(\"bob\", 2)); "
        "  if l.Contains(new NamedVal(\"alice\", 1)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.3: LinkedList<record-with-string-field>.Contains — absent.
BEZEL_TEST(llist_contains_eq_spec_8_3_9_1, linkedlist_record_str_contains_absent_bug562) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record NamedVal(Name: string, N: int) end NamedVal; "
        "class C public static method Main(): int "
        "  using var l := new LinkedList<NamedVal>(); "
        "  l.AddLast(new NamedVal(\"alice\", 1)); "
        "  l.AddLast(new NamedVal(\"bob\", 2)); "
        "  if l.Contains(new NamedVal(\"carol\", 3)) then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §8.3: record — matching name but different int → absent.
BEZEL_TEST(llist_contains_eq_spec_8_3_9_1, linkedlist_record_str_contains_diff_int_bug562) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record NamedVal(Name: string, N: int) end NamedVal; "
        "class C public static method Main(): int "
        "  using var l := new LinkedList<NamedVal>(); "
        "  l.AddLast(new NamedVal(\"alice\", 1)); "
        "  if l.Contains(new NamedVal(\"alice\", 99)) then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// ===========================================================================
// Regression guards — int and plain-record must not regress
// ===========================================================================

// §4.6/§8.3: LinkedList<int>.Contains — present (regression guard).
BEZEL_TEST(llist_contains_eq_spec_4_6, linkedlist_int_contains_present_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(7); l.AddLast(14); l.AddLast(21); "
        "  if l.Contains(14) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §4.6/§8.3: LinkedList<int>.Contains — absent (regression guard).
BEZEL_TEST(llist_contains_eq_spec_4_6, linkedlist_int_contains_absent_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(7); l.AddLast(14); l.AddLast(21); "
        "  if l.Contains(99) then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §8.3: LinkedList<record> with plain int fields — present (regression guard).
BEZEL_TEST(llist_contains_eq_spec_8_3, linkedlist_record_int_contains_present_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record Pt(X: int, Y: int) end Pt; "
        "class C public static method Main(): int "
        "  using var l := new LinkedList<Pt>(); "
        "  l.AddLast(new Pt(1, 2)); l.AddLast(new Pt(3, 4)); "
        "  if l.Contains(new Pt(3, 4)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.3: LinkedList<record> with plain int fields — absent (regression guard).
BEZEL_TEST(llist_contains_eq_spec_8_3, linkedlist_record_int_contains_absent_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record Pt(X: int, Y: int) end Pt; "
        "class C public static method Main(): int "
        "  using var l := new LinkedList<Pt>(); "
        "  l.AddLast(new Pt(1, 2)); l.AddLast(new Pt(3, 4)); "
        "  if l.Contains(new Pt(5, 6)) then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}
