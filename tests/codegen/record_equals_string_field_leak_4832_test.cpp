// fix #4832 (§8.3 / §28.2, v0.307): `.Equals` on a record with string
// fields leaks both operands — 2×32 B per call — while `=` is clean.
//
// The discriminator: `==` / `=` on the same record is CLEAN, and only the
// **method-call form** leaks. This isolates the defect to the method-call
// path rather than the shared equality machinery.
//
// Root cause: `tryLowerValueSemanticsCall` → `emitStructEquality` does not
// register the record temporaries' string fields for disposal. The `=`
// operator path handles this via `collectRecordCallResultReleaseEligible` /
// `lowerLocalDecl` — the `.Equals` path takes a different route through
// `tryLowerValueSemanticsCall` that lacks the equivalent disposal registration.
//
// The fix: add the equivalent `recordStringFields` disposal registration for
// the temporaries created in the `.Equals` method-call path.
//
// MEASURED (FAIL-WITHOUT / PASS-WITH, compiled with bezelc --sanitize=address,
// 300 loop iterations, unique strings via "hello" + i.ToString()):
//   pre-fix: SUMMARY: AddressSanitizer: 24380 byte(s) leaked in 1200
//            allocation(s) (300 calls × 2 operands × 2 allocs per operand:
//            9600B record + 9600B string data + 2×2590B indirect)
//   post-fix: clean exit, no LSan report.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ===========================================================================
// Leak-fixed rows: loop-amplified (300x, per LANE policy — a one-shot LSan
// row can pass on both sides), each row's Main returns 0 unconditionally so
// runChecked's oracle is exactly "did ASan/LSan report anything at all".
// ===========================================================================

// The issue's own repro: record with a string field, `.Equals` call,
// loop-amplified 300×. The leak is 2×32 B per call = 19200 B / 600 allocs.
BEZEL_TEST(record_equals_string_field_leak_4832,
           equals_method_call_leaks_string_fields_4832) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; record Rec(s: string); "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 300 do "
        "    var a := new Rec(\"hello\"); "
        "    var b := new Rec(\"hello\"); "
        "    var r := a.Equals(b); "
        "    i := i + 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// CONTROL: the `=` operator on the same record is CLEAN (no leak).
// This proves the bug is specific to the method-call form.
BEZEL_TEST(record_equals_string_field_leak_4832,
           equals_operator_clean_no_leak_4832_control) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; record Rec(s: string); "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 300 do "
        "    var a := new Rec(\"hello\"); "
        "    var b := new Rec(\"hello\"); "
        "    if a = b then var r := 0; else var r := 1; end; "
        "    i := i + 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// Value-correctness control: equal records via `.Equals` return true.
BEZEL_TEST(record_equals_string_field_leak_4832,
           equals_method_returns_true_for_equal_records) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Rec(s: string); "
        "class C public static method Main(): int "
        "  var a := new Rec(\"hello\"); "
        "  var b := new Rec(\"hello\"); "
        "  return (if a.Equals(b) then 0 else 1); "
        "end Main; end C;"), 0);
}

// Value-correctness control: unequal records via `.Equals` return false.
BEZEL_TEST(record_equals_string_field_leak_4832,
           equals_method_returns_false_for_unequal_records) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Rec(s: string); "
        "class C public static method Main(): int "
        "  var a := new Rec(\"hello\"); "
        "  var b := new Rec(\"world\"); "
        "  return (if a.Equals(b) then 0 else 1); "
        "end Main; end C;"), 1);
}

// CONTROL: `=` operator returns correct result for equal records.
BEZEL_TEST(record_equals_string_field_leak_4832,
           equals_operator_returns_true_for_equal_records) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Rec(s: string); "
        "class C public static method Main(): int "
        "  var a := new Rec(\"hello\"); "
        "  var b := new Rec(\"hello\"); "
        "  return (if a = b then 0 else 1); "
        "end Main; end C;"), 0);
}
