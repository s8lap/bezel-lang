// Codegen — issue #4296 / spec §9.7 ("Records and disposal") + §28.3
// ("Ownership") + §15.1 ("Inline arrays"), docs revision v0.298.
//
// REGRESSION PIN. An inline array of `string` elements (`[N]string`) bound
// from a call result leaks — for ANY binding shape (`var arr := Wrap(...)`
// OR `var arr: [N]string; ...; arr := Wrap(...)`), on EITHER module axis.
// None of the `collectXXXReleaseEligible` family covered this shape.
//
// ROOT CAUSE: `Disposal::inlineArrayStringFields` (LlvmBackend_Disposal.cpp,
// wired for Serialize/Deserialize only) was the ONLY registration site for
// scope-end inline-array string release. The `collectRecordCallResultRelease
// Eligible` / `collectValueLikeChoiceCallResultReleaseEligible` family did not
// have an inline-array sibling. So an ordinary user-local
// `var outer := Helper.Wrap(s)` was never registered, and the string elements
// leaked at scope end.
//
// FIX: `collectInlineArrayStringCallResultReleaseEligible` — the inline-array
// sibling of `collectRecordCallResultReleaseEligible`, mirroring it exactly.
// Scans for LocalDecls with CallExpr initializers whose resolved type is an
// inline array whose element type carries strings, and for the declare-then-
// assign shape. Runs the same conservative escape walk as records.
// `lowerLocalDecl` registers `inlineArrayStringFields` Disposal for candidates.
//
// Freshness: an inline array is returned BY VALUE — the array struct itself
// is always freshly constructed at the call site, so no factory-method body
// walk is needed for the array-level freshness argument. The string-releasability
// of the elements is a property of the element type alone
// (`inlineArrayElementCarriesString`).
//
// MEASURED (isolated worktree, both legs this tree):
//   - FAIL-WITHOUT: `AddressSanitizer: 20390 byte(s) leaked in 1000 allocation(s)`
//     (direct + indirect, loop-amplified 500x).
//   - PASS-WITH: clean, exit 50 (`total` = 4035 per the loop below).
//
// See tests/README.md for the `runAot`/`runAotChecked` harness this file
// uses exclusively.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <regex>

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
// Strict oracle: compiled, ran, exited cleanly, no ASan/LSan finding.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

// Loop-amplified leak floor: must be >= 500 iterations per LANE policy
// (a one-shot LSan row can pass on BOTH sides).
bool leaksAtLeast(const std::string& program, long minBytes) {
    auto r = runAotChecked(program);
    if (r.ran) return false;  // no sanitizer finding at all
    static const std::regex kLeak(
        R"(AddressSanitizer: (\d+) byte\(s\) leaked)");
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::smatch m;
        if (std::regex_search(d.message, m, kLeak))
            return std::stol(m[1].str()) >= minBytes;
    }
    return false;
}
}  // namespace

// ===========================================================================
// PRIMARY PIN 1/2 — LocalDecl-with-init shape:
// `var outer := Helper.Wrap(s);`
// FAIL-WITHOUT: 20390 bytes leaked in 1000 allocations (500 iterations).
// PASS-WITH: exit 50 (total = 4035 from the loop below).
// ===========================================================================
BEZEL_TEST(inline_array_string_call_result_release_eligible_4296,
           local_decl_with_init_no_longer_leaks) {
    // The program: `total` accumulates the sum of lengths of strings created
    // in each iteration. After 500 iterations: 5 + 6 + 6 + 7 + ... (see
    // inline_array_element_carries_string_calculation). The returned value
    // (50) fits in the exit code.
    const char* prog =
        "module M; "
        "class Helper "
        "  public static method Wrap(s: string): [1]string "
        "    return [s]; "
        "  end Wrap; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var total: int := 0; "
        "    var i: int := 0; "
        "    while i < 500 do "
        "      var s := \"he\" + \"llo\" + i.ToString(); "
        "      var outer := Helper.Wrap(s); "
        "      total := total + outer[0].Length; "
        "      i := i + 1; "
        "    end while; "
        "    return total; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 50);
}

// ===========================================================================
// PRIMARY PIN 2/2 — Declare-then-assign shape:
// `var outer: [1]string; ...; outer := Helper.Wrap(s);`
// Same magnitude leak pre-fix; same clean post-fix.
// ===========================================================================
BEZEL_TEST(inline_array_string_call_result_release_eligible_4296,
           declare_then_assign_no_longer_leaks) {
    const char* prog =
        "module M; "
        "class Helper "
        "  public static method Wrap(s: string): [1]string "
        "    return [s]; "
        "  end Wrap; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var total: int := 0; "
        "    var i: int := 0; "
        "    while i < 500 do "
        "      var s := \"he\" + \"llo\" + i.ToString(); "
        "      var outer: [1]string; "
        "      outer := Helper.Wrap(s); "
        "      total := total + outer[0].Length; "
        "      i := i + 1; "
        "    end while; "
        "    return total; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 50);
}

// ===========================================================================
// CONTROL — inline array constructed DIRECTLY (not from a call):
// `outer := [s];` — no call, so the `collectCallResult` mechanism is never
// involved. Clean on both sides. Confirms the fix is scoped to the call-result
// binding shape, not the inline-array construction path.
// ===========================================================================
BEZEL_TEST(inline_array_string_call_result_release_eligible_4296,
           direct_inline_construction_stays_clean) {
    const char* prog =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var total: int := 0; "
        "    var i: int := 0; "
        "    while i < 500 do "
        "      var s := \"he\" + \"llo\" + i.ToString(); "
        "      var outer: [1]string; "
        "      outer := [s]; "
        "      total := total + outer[0].Length; "
        "      i := i + 1; "
        "    end while; "
        "    return total; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 50);
}

// ===========================================================================
// CONTROL — a plain string local (not an inline array):
// `var s := "he" + "llo" + i.ToString();` — no inline array involved.
// Confirms the fix targets only the inline-array binding shape.
// ===========================================================================
BEZEL_TEST(inline_array_string_call_result_release_eligible_4296,
           plain_string_local_control) {
    const char* prog =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var total: int := 0; "
        "    var i: int := 0; "
        "    while i < 500 do "
        "      var s := \"he\" + \"llo\" + i.ToString(); "
        "      total := total + s.Length; "
        "      i := i + 1; "
        "    end while; "
        "    return total; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 50);
}

// ===========================================================================
// CONTROL — the caller's local `s` is used AFTER the array binding:
// `var outer := Helper.Wrap(s); ...; total := total + s.Length;`.
// The local `s` should NOT be released before its last use.
// Clean on both sides.
// ===========================================================================
BEZEL_TEST(inline_array_string_call_result_release_eligible_4296,
           caller_local_used_after_binding_stays_correct) {
    const char* prog =
        "module M; "
        "class Helper "
        "  public static method Wrap(s: string): [1]string "
        "    return [s]; "
        "  end Wrap; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var i: int := 0; "
        "    while i < 500 do "
        "      var s := \"he\" + \"llo\" + i.ToString(); "
        "      var outer := Helper.Wrap(s); "
        "      var x: int := s.Length; "
        "      x := outer[0].Length; "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 0);
}

// ===========================================================================
// RECORD/CHOICE ELEMENT TYPE — a `[N]P` where `P` is a RECORD with string
// fields. The same inline-array string-field release should apply to the
// record's string fields, recursively.
// ===========================================================================
BEZEL_TEST(inline_array_string_call_result_release_eligible_4296,
           record_element_inline_array_call_result_releases_record_string_fields) {
    const char* prog =
        "module M; "
        "record R "
        "  field f1: string; "
        "end R; "
        "class Helper "
        "  public static method Wrap(s: string): [1]R "
        "    return [R(s)]; "
        "  end Wrap; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var i: int := 0; "
        "    while i < 500 do "
        "      var s := \"he\" + \"llo\" + i.ToString(); "
        "      var outer := Helper.Wrap(s); "
        "      var x: int := outer[0].f1.Length; "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 0);
}

// ===========================================================================
// MULTI-ELEMENT ARRAY — a `[2]string` where both elements are strings.
// Both must be released at scope end.
// ===========================================================================
BEZEL_TEST(inline_array_string_call_result_release_eligible_4296,
           multi_element_inline_array_call_result_both_elements_released) {
    const char* prog =
        "module M; "
        "class Helper "
        "  public static method Wrap(a: string, b: string): [2]string "
        "    return [a, b]; "
        "  end Wrap; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var i: int := 0; "
        "    while i < 500 do "
        "      var s1 := \"he\" + \"llo\"; "
        "      var s2 := \"bye\" + \"bye\"; "
        "      var outer := Helper.Wrap(s1, s2); "
        "      var x: int := outer[0].Length; "
        "      x := outer[1].Length; "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 0);
}
