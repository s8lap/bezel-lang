// Codegen — §9.2 / §35.7 (v0.307, #4652): a choice payload field marked
// `equality skip` is excluded from the synthesized `=` and `GetHashCode`.
// The existing `choice_payload_equality_skip_wire_4652` test in the module
// suite pins only the METADATA serialization side (§35.7 wire format). This
// test pins the RUNTIME side: equality (`=`) must be true when only the
// equality-skipped payload differs, and `GetHashCode` must agree for equal
// values.
//
// The repro from the issue:
//   public choice Pt
//     Pos(X: int, Cache: int equality skip);
//   end Pt;
//   var a := Pt.Pos(1, 10);
//   var b := Pt.Pos(1, 20);
//   // a = b should be TRUE (Cache is skipped) but printed "not equal"

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §35.7 / §9.2 — runtime choice equality: `equality skip` on a payload
// field removes it from the membership computation. Pt.Pos(1, 10) and
// Pt.Pos(1, 20) differ only in the skipped Cache field, so they must
// compare equal.
BEZEL_TEST(codegen_choice_payload_equality_skip_runtime_4652,
           spec_35_7_choice_payload_equality_skip_runtime_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public choice Pt "
        "  Pos(X: int, Cache: int equality skip); "
        "end Pt; "
        "class C public static method Main(): int "
        "  var a := Pt.Pos(1, 10); "
        "  var b := Pt.Pos(1, 20); "
        "  // Cache differs, but equality skip means a = b must be true"
        "  if a = b then return 1; else return 0; end; "
        "end Main; end C;"), 1);
}

// §35.7 / §9.2 — runtime choice inequality: two choices with different
// non-skipped payload values must NOT be equal.
BEZEL_TEST(codegen_choice_payload_equality_skip_runtime_4652,
           spec_35_7_choice_payload_equality_skip_runtime_unequal) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public choice Pt "
        "  Pos(X: int, Cache: int equality skip); "
        "end Pt; "
        "class C public static method Main(): int "
        "  var a := Pt.Pos(1, 10); "
        "  var b := Pt.Pos(2, 20); "
        "  // X differs (1 vs 2), so a = b must be false"
        "  if not (a = b) then return 1; else return 0; end; "
        "end Main; end C;"), 1);
}

// §8.3 / §35.7 — hash-code invariant: equal values (§35.7 reduced
// membership) must hash equal. Pt.Pos(1, 10) and Pt.Pos(1, 20) are equal
// (Cache is equality-skipped), so GetHashCode must agree.
BEZEL_TEST(codegen_choice_payload_equality_skip_runtime_4652,
           spec_35_7_choice_payload_equality_skip_hashcode_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public choice Pt "
        "  Pos(X: int, Cache: int equality skip); "
        "end Pt; "
        "class C public static method Main(): int "
        "  var a := Pt.Pos(1, 10); "
        "  var b := Pt.Pos(1, 20); "
        "  // Equal values must have equal hashes"
        "  if a.GetHashCode() = b.GetHashCode() then return 1; else return 0; end; "
        "end Main; end C;"), 1);
}

// §8.3 / §35.7 — hash-code invariant: unequal values must (probably)
// have different hashes. Pt.Pos(1, 10) and Pt.Pos(2, 20) are unequal
// (X differs), so their hashes should differ.
BEZEL_TEST(codegen_choice_payload_equality_skip_runtime_4652,
           spec_35_7_choice_payload_equality_skip_hashcode_unequal) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public choice Pt "
        "  Pos(X: int, Cache: int equality skip); "
        "end Pt; "
        "class C public static method Main(): int "
        "  var a := Pt.Pos(1, 10); "
        "  var b := Pt.Pos(2, 20); "
        "  // Unequal values should have different hashes (strong but not absolute)"
        "  if a.GetHashCode() <> b.GetHashCode() then return 1; else return 0; end; "
        "end Main; end C;"), 1);
}

// §35.7 / §9.2 — payload-free case is always compared by discriminant
// only; the equality-skip on a different case's payload field is irrelevant.
BEZEL_TEST(codegen_choice_payload_equality_skip_runtime_4652,
           spec_35_7_choice_payload_equality_skip_payload_free_case) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public choice Pt "
        "  Pos(X: int, Cache: int equality skip), "
        "  Empty; "
        "end Pt; "
        "class C public static method Main(): int "
        "  var a := Pt.Pos(1, 10); "
        "  var b := Pt.Pos(1, 99); "
        "  var c := Pt.Empty; "
        "  // a = b: Pos with same X, different Cache (skipped) -> equal"
        "  // a = c: different case -> not equal"
        "  if a = b and not (a = c) then return 1; else return 0; end; "
        "end Main; end C;"), 1);
}

// §35.7 / §9.2 — multiple payloads, one skipped: only the non-skipped
// fields participate in equality.
BEZEL_TEST(codegen_choice_payload_equality_skip_runtime_4652,
           spec_35_7_choice_multiple_payloads_one_skipped) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public choice Multi "
        "  Mk(A: int, B: int, Cache: int equality skip); "
        "end Multi; "
        "class C public static method Main(): int "
        "  var a := Multi.Mk(1, 2, 100); "
        "  var b := Multi.Mk(1, 2, 200); "
        "  var c := Multi.Mk(1, 3, 100); "
        "  // a = b: A and B match, Cache differs but is skipped -> equal"
        "  // a = c: B differs (2 vs 3) -> not equal"
        "  if a = b and not (a = c) then return 1; else return 0; end; "
        "end Main; end C;"), 1);
}

// §35.7 / §9.2 — hash agreement for multiple-payload, one-skipped case.
BEZEL_TEST(codegen_choice_payload_equality_skip_runtime_4652,
           spec_35_7_choice_multiple_payloads_one_skipped_hash) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public choice Multi "
        "  Mk(A: int, B: int, Cache: int equality skip); "
        "end Multi; "
        "class C public static method Main(): int "
        "  var a := Multi.Mk(1, 2, 100); "
        "  var b := Multi.Mk(1, 2, 200); "
        "  // Equal values must have equal hashes"
        "  if a.GetHashCode() = b.GetHashCode() then return 1; else return 0; end; "
        "end Main; end C;"), 1);
}
