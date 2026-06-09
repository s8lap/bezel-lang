// Codegen — `string in string` (§17.5/§4.6) and LinkedList<T> equality (§17.5/§8.3).
//
// §17.5: `x in collection` dispatches to `collection.Contains(x)`. When RHS is a
// `string`, both `char in string` (already implemented) and `string in string`
// (substring containment ≡ rhs.Contains(lhs), §9.6 Contains(sub: string): bool)
// are covered. §4.6 specifies that string operations use code-point equality.
//
// §17.5/§8.3: `x in LinkedList<T>` must use structural equality per element type —
// record structural (§8.3/§9.1), string content (§4.6), value `==` (§8.3).
// Bug #560: the old bz_llist_find path used memcmp on raw element bytes, which
// for `string` compared `bz_string*` pointer values (not content), so
// LinkedList<string> and LinkedList<record-with-string-field> always returned false.
//
// Bug #558: `string in string` was never implemented — the codegen fell through to
// `return nullptr` ("other in forms"), yielding JIT exit -999. Pre-existing gap,
// not a #551 regression.
// Bug #560: LinkedList<string> `in` compared pointers (memcmp), not string content.
// Introduced by #551's use of `bz_llist_find` for the LinkedList branch.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ===========================================================================
// §17.5/§4.6 — Bug #558: `string in string` (substring containment)
// ===========================================================================

// §17.5/§4.6: substring present → true. Before fix: exit -999.
// `"ello" in "hello"` ≡ `"hello".Contains("ello")` per §17.5.
BEZEL_TEST(in_operator_string_llist_spec_17_5_4_6, string_in_string_present_bug558) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  if \"ello\" in \"hello\" then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §17.5/§4.6: substring absent → false.
BEZEL_TEST(in_operator_string_llist_spec_17_5_4_6, string_in_string_absent_bug558) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  if \"xyz\" in \"hello\" then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §17.5/§4.6: equal strings → true (entire string is a substring of itself).
BEZEL_TEST(in_operator_string_llist_spec_17_5_4_6, string_in_string_equal_bug558) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  if \"hello\" in \"hello\" then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §17.5/§4.6: empty string in any string → true (empty is a substring of everything).
BEZEL_TEST(in_operator_string_llist_spec_17_5_4_6, empty_string_in_string_bug558) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  if \"\" in \"hello\" then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ===========================================================================
// §17.5/§4.6 — Bug #560: LinkedList<string> `in` (content equality)
// ===========================================================================

// §17.5/§4.6: LinkedList<string> — present element (content equality). Before fix:
// memcmp compared bz_string* pointers → always false for heap-allocated literals.
BEZEL_TEST(in_operator_string_llist_spec_17_5_4_6, linkedlist_string_present_bug560) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<string>(); "
        "  l.AddLast(\"hello\"); l.AddLast(\"world\"); "
        "  if \"hello\" in l then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §17.5/§4.6: LinkedList<string> — absent element.
BEZEL_TEST(in_operator_string_llist_spec_17_5_4_6, linkedlist_string_absent_bug560) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<string>(); "
        "  l.AddLast(\"hello\"); l.AddLast(\"world\"); "
        "  if \"bye\" in l then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// ===========================================================================
// §17.5/§8.3/§9.1 — Bug #560: LinkedList<record-with-string-field> `in`
// ===========================================================================

// §17.5/§8.3: record with a string field in LinkedList<T>. Before fix: memcmp
// compared raw bytes including the string pointer → wrong for equal-content strings.
BEZEL_TEST(in_operator_string_llist_spec_17_5_8_3, linkedlist_record_str_present_bug560) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record NamedVal(Name: string, N: int) end NamedVal; "
        "class C public static method Main(): int "
        "  using var l := new LinkedList<NamedVal>(); "
        "  l.AddLast(new NamedVal(\"alice\", 1)); "
        "  l.AddLast(new NamedVal(\"bob\", 2)); "
        "  if new NamedVal(\"alice\", 1) in l then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §17.5/§8.3: record with string field — absent.
BEZEL_TEST(in_operator_string_llist_spec_17_5_8_3, linkedlist_record_str_absent_bug560) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record NamedVal(Name: string, N: int) end NamedVal; "
        "class C public static method Main(): int "
        "  using var l := new LinkedList<NamedVal>(); "
        "  l.AddLast(new NamedVal(\"alice\", 1)); "
        "  l.AddLast(new NamedVal(\"bob\", 2)); "
        "  if new NamedVal(\"carol\", 3) in l then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §17.5/§8.3: record with string field, matching name but different int → absent.
BEZEL_TEST(in_operator_string_llist_spec_17_5_8_3, linkedlist_record_str_diff_int_bug560) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record NamedVal(Name: string, N: int) end NamedVal; "
        "class C public static method Main(): int "
        "  using var l := new LinkedList<NamedVal>(); "
        "  l.AddLast(new NamedVal(\"alice\", 1)); "
        "  if new NamedVal(\"alice\", 99) in l then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// ===========================================================================
// Regression guards — must not regress LinkedList<int> or LinkedList<plain-record>
// ===========================================================================

// §17.5: LinkedList<int> — present (regression guard for #560 fix).
BEZEL_TEST(in_operator_string_llist_spec_17_5, linkedlist_int_present_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(7); l.AddLast(14); l.AddLast(21); "
        "  if 14 in l then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §17.5: LinkedList<int> — absent (regression guard).
BEZEL_TEST(in_operator_string_llist_spec_17_5, linkedlist_int_absent_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(7); l.AddLast(14); l.AddLast(21); "
        "  if 99 in l then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §17.5/§8.3: LinkedList<record> with plain int fields — present (regression guard).
BEZEL_TEST(in_operator_string_llist_spec_17_5_8_3, linkedlist_record_int_present_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record Pt(X: int, Y: int) end Pt; "
        "class C public static method Main(): int "
        "  using var l := new LinkedList<Pt>(); "
        "  l.AddLast(new Pt(1, 2)); l.AddLast(new Pt(3, 4)); "
        "  if new Pt(3, 4) in l then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §17.5/§8.3: LinkedList<record> with plain int fields — absent (regression guard).
BEZEL_TEST(in_operator_string_llist_spec_17_5_8_3, linkedlist_record_int_absent_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record Pt(X: int, Y: int) end Pt; "
        "class C public static method Main(): int "
        "  using var l := new LinkedList<Pt>(); "
        "  l.AddLast(new Pt(1, 2)); l.AddLast(new Pt(3, 4)); "
        "  if new Pt(5, 6) in l then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}
