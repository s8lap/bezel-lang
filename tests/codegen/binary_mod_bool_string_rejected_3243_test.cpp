// Regression for #3243 (v0.256 follow-up to #3109 narrow).
//
// Authority: §3.5 and §6.1. §3.5's operator table admits `mod` on the integer
// types and `decimal`; it is invalid on `bool`, `char`, and `string`. §6.1
// confirms "`mod` is defined on integer types and on `decimal` … not defined on
// `float` or `double`".
//
// Pre-fix: the binary `mod` arm in `src/sema/ExprTypecheck_Exprs.cpp`'s
// `typeBinary` rejected ONLY `float`/`double` (always) and `char mod char`
// (narrow #3109). `bool` and `string` on either side fell through to codegen,
// which emitted `srem i1 %lhs, i32 %rhs` (or the `ptr` form for `string`) and
// then aborted with `IR verification failed (malformed IR emitted)`. This
// regression test pins the SHIPPED rejection at the sema layer; the codegen
// pass is no longer reachable for these inputs.
//
// The narrow #3109 cast-pattern invariants must continue to pass:
// `char mod <int>` and `<int> mod char` are admitted via §5.1 widening,
// `char mod char` is rejected (still). The 2 `codegen_string_length_528.*_mod256`
// tests pin those.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bool compileErrored(const std::string& body) {
    return runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;").diagnostics.hasErrors();
}

// True iff the diagnostics list carries a message containing every needle.
bool diagnosticsMentionAll(
    const bezel::DiagnosticEngine& de,
    std::initializer_list<const char*> needles) {
    for (const auto& d : de.diagnostics()) {
        bool allFound = true;
        for (const char* n : needles) {
            if (d.message.find(n) == std::string::npos) {
                allFound = false;
                break;
            }
        }
        if (allFound) return true;
    }
    return false;
}

}  // namespace

// ── NEW: `bool` operand on either side is rejected (§6.1) ────────────────

BEZEL_TEST(binary_mod_bool_string_rejected_3243, bool_lhs_rejected) {
    BEZEL_EXPECT_TRUE(compileErrored(
        "  var b: bool := true; "
        "  return int(b mod 2);"));
}

BEZEL_TEST(binary_mod_bool_string_rejected_3243, bool_rhs_rejected) {
    BEZEL_EXPECT_TRUE(compileErrored(
        "  var b: bool := true; "
        "  return int(2 mod b);"));
}

BEZEL_TEST(binary_mod_bool_string_rejected_3243, bool_both_sides_rejected) {
    BEZEL_EXPECT_TRUE(compileErrored(
        "  var a: bool := true; "
        "  var b: bool := false; "
        "  return int(a mod b);"));
}

// ── NEW: `string` operand on either side is rejected (§6.1) ───────────────

BEZEL_TEST(binary_mod_bool_string_rejected_3243, string_lhs_rejected) {
    BEZEL_EXPECT_TRUE(compileErrored(
        "  var s: string := \"abc\"; "
        "  return int(s mod 2);"));
}

BEZEL_TEST(binary_mod_bool_string_rejected_3243, string_rhs_rejected) {
    BEZEL_EXPECT_TRUE(compileErrored(
        "  var s: string := \"abc\"; "
        "  return int(2 mod s);"));
}

BEZEL_TEST(binary_mod_bool_string_rejected_3243, string_both_sides_rejected) {
    BEZEL_EXPECT_TRUE(compileErrored(
        "  var a: string := \"abc\"; "
        "  var b: string := \"xyz\"; "
        "  return int(a mod b);"));
}

// ── Pin the diagnostic wording (parallels the compound `mod=` arm) ────────

BEZEL_TEST(binary_mod_bool_string_rejected_3243,
           bool_mod_int_diagnostic_mentions_and_or_not) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var b: bool := true; "
        "  return int(b mod 2); "
        "end Main; end C;");
    // fix #3895 (issue #3892): the bare `hasErrors()` claim below was
    // redundant with (and less precise than) this message-content check —
    // removed rather than kept alongside an ambiguous claim a runtime
    // crash/signal could also satisfy.
    BEZEL_EXPECT_TRUE(diagnosticsMentionAll(r.diagnostics,
        {"`mod` is not valid on `bool`", "`and`/`or`/`not`", "§6.1"}));
}

BEZEL_TEST(binary_mod_bool_string_rejected_3243,
           string_mod_int_diagnostic_mentions_section_6_1) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var s: string := \"abc\"; "
        "  return int(s mod 2); "
        "end Main; end C;");
    // fix #3895 (issue #3892): see bool_mod_int_diagnostic_mentions_and_or_not
    // above — the bare `hasErrors()` claim was redundant with this stronger
    // message-content check.
    BEZEL_EXPECT_TRUE(diagnosticsMentionAll(r.diagnostics,
        {"`mod` is not valid on `string`", "§6.1"}));
}

// ── MUST-PASS controls (narrow #3109 / §5.1 cast patterns stay admitted) ──

BEZEL_TEST(binary_mod_bool_string_rejected_3243,
           int_mod_int_still_admitted) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var i: int := 47; "
        "  var j: int := 10; "
        "  return i mod j; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 7);
}

BEZEL_TEST(binary_mod_bool_string_rejected_3243,
           char_mod_int_literal_still_admitted) {
    // The canonical cast pattern from #3109: `s[0] mod 256`. Must remain
    // admitted via §5.1's char→int widening.
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var s := \"\xe6\x97\xa5\xe6\x9c\xac\"; "
        "  return s[0] mod 256; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 26085 % 256);
}

BEZEL_TEST(binary_mod_bool_string_rejected_3243,
           char_mod_char_still_rejected) {
    // The narrow #3109 invariant: `char mod char` is rejected.
    BEZEL_EXPECT_TRUE(compileErrored(
        "  var c: char := 'a'; "
        "  var d: char := 'b'; "
        "  return int(c mod d);"));
}

// ── Compound `mod=` form is unchanged (control: still rejects bool/string
//    on the target; re-pinned here so a future regression that re-orders the
//    binary-vs-compound guards does not silently re-introduce the IR-verify
//    class.) ────────────────────────────────────────────────────────────────

BEZEL_TEST(binary_mod_bool_string_rejected_3243,
           bool_compound_mod_assign_rejected) {
    BEZEL_EXPECT_TRUE(compileErrored(
        "  var b: bool := true; "
        "  b mod= 2; "
        "  return b ? 1 : 0;"));
}

BEZEL_TEST(binary_mod_bool_string_rejected_3243,
           string_compound_mod_assign_rejected) {
    BEZEL_EXPECT_TRUE(compileErrored(
        "  var s: string := \"abc\"; "
        "  s mod= 2; "
        "  return 0;"));
}