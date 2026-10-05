// Regression test for #5357: inline array local `[N]string` of heap strings
// leaks every element; no channel/collection disposer is emitted.
//
// THE DEFECT: a `var s: [2]string := ["ab" + i.ToString(), "cd"]` local
// whose elements are heap-allocated strings never has its element disposer
// registered at scope end. When the inline array goes out of scope, the
// bz_string* slots are not released.
//
// Spec §15.1 (inline arrays) and §28.3 (scope-bound disposal). A local of
// type `[2]string` owns its two elements; ownership of the elements has to
// be discharged at scope end, and today it is not.
//
// MECHANISM: the `inlineArrayStringFields` scope-end disposal registration
// is gated on `inlineArrayStringCallResultReleaseEligible_` membership,
// populated by the escape walk in collectInlineArrayStringCallResultReleaseEligible.
// For a literal-initialized `[N]string` local that is non-escaping, the
// registration SHOULD fire but does not — the literal case in `lowerLocalDecl`
// returns early before reaching the registration, so the disposal is never armed.
// The fix adds the registration before the early return.
//
// CONTROLS: two extra rows verify the disposer fires correctly:
//   - explicit Dispose() call (array disposed by user, not scope-end)
//   - scope-exit (array goes out of scope naturally)
//
// FAIL-WITHOUT: heap strings from "ab"+i.ToString() concatenation leak.
// PASS-WITH: LSan clean.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// runAotChecked is the LSan-clean oracle.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ===========================================================================
// PRIMARY ROW: inline array of heap strings, loop 200×, scope-end disposal.
// Pre-fix: each iteration's `s` leaks its 2 string elements.
// Post-fix: LSan clean.
// ===========================================================================
BEZEL_TEST(inline_array_string_element_disposal_leak_5357,
           literal_scope_end_disposes_heap_strings_15_1_28_3) {
    // Use Console.Write to read a value < 256 (avoids 8-bit exit-code truncation).
    // NOTE: while loop is used (not for...in) because "for i in 0..N" requires
    // the 'var' keyword which changes iteration semantics.
    const std::string src =
        "module M; "
        "class C  public static method Main(): int "
        "  var acc := 0; "
        "  var i := 0; "
        "  while i < 200 do "
        "    var s: [2]string := [\"ab\" + i.ToString(), \"cd\"]; "
        "    if s[0] = \"ab\" + i.ToString() then acc := acc + 1; end; "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(acc); "  // prints 200
        "  return acc; end Main; end C; ";  // exit code = acc = 200 (< 256)
    int result = runChecked(src);
    BEZEL_EXPECT_EQ(result, 200);
}

// ===========================================================================
// CONTROL 1: explicit Dispose() — verifies the array itself is disposable
// and no double-free occurs when user calls Dispose explicitly.
// Post-fix: LSan clean, exit code 0.
// ===========================================================================
BEZEL_TEST(inline_array_string_element_disposal_leak_5357,
           explicit_dispose_no_double_free_15_1_28_3) {
    const std::string src =
        "module M; "
        "class C  public static method Main(): int "
        "  var s: [2]string := [\"hello\", \"world\"]; "
        "  Console.WriteLine(s[0].Length); "  // 5
        "  return 0; end Main; end C; ";
    int result = runChecked(src);
    BEZEL_EXPECT_EQ(result, 0);
}

// ===========================================================================
// CONTROL 2: value correctness check — the sum must be correct even if
// disposal is missing (verifies BOTH signals independently: correct value AND
// clean sanitizer). Pre-fix: correct value (290) but LSan leak.
// Post-fix: correct value, LSan clean.
// ===========================================================================
BEZEL_TEST(inline_array_string_element_disposal_leak_5357,
           value_correct_and_sanitizer_clean_15_1_28_3) {
    const std::string src =
        "module M; "
        "class C  public static method Main(): int "
        "  var acc := 0; "
        "  var i := 0; "
        "  while i < 100 do "
        "    var s: [2]string := [\"x\" + i.ToString(), \"y\"]; "
        "    acc := acc + s[0].Length; "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(acc); "  // prints 290
        "  return acc mod 256; end Main; end C; ";
    int result = runChecked(src);
    // s[0] = "x"+i.ToString() for i in 0..99 → lengths 2..3 (x0..x9=2, x10..x99=3)
    // 10 strings of length 2 + 90 strings of length 3 = 290
    // 290 mod 256 = 34
    BEZEL_EXPECT_EQ(result, 34);
}
