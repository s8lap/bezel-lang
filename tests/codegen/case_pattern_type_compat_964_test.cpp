// Codegen + sema — #964: a `case` pattern whose constant has a type
// INCOMPATIBLE with the scrutinee must be a clean §19 diagnostic, not an LLVM
// `ICmpInst::AssertOK` abort (SIGABRT, rc 134).
//
// Root cause: codegen's pattern-equality folds the pattern constant and emits an
// ICmp / string content-compare against the scrutinee. `reconcile` bridges only
// integer/float WIDTHS, so a ptr-vs-scalar (string const vs int scrutinee),
// i1-vs-wider-int (bool), or fp-vs-int residual mismatch reached the ICmp with
// operands of different LLVM types → hard abort. Sema did not reject it up front.
//
// Fix (two layers):
//   - sema (ExprTypecheck_Exprs.cpp typeCase arm loop): a LITERAL ConstantPattern
//     whose primitive category (string / bool) mismatches the scrutinee's is
//     rejected with a §19 diagnostic up front.
//   - codegen (LlvmBackend_Exprs.cpp patternMatches NamedPattern branch): a
//     NAMED-CONST pattern (`C.K`) whose const type is incompatible (its type is
//     not resolved in sema) is caught at the equality site — report §19 and
//     never-match instead of crashing.
//
// Numeric widths (int/long/char/byte) and the matching cases are unaffected.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
// A compile-time error was reported, whether at sema (literal — runJit returns
// before codegen) or at codegen (named-const — codegen reports + never-matches,
// so the program may still "run"). Either way diagnostics carry the §19 error.
bool compileErrored(const std::string& src) {
    return runJit(src).diagnostics.hasErrors();
}
}  // namespace

// §19 (#964): the issue's literal repro — int scrutinee, string-literal pattern.
// Clean reject, NOT an LLVM abort.
BEZEL_TEST(case_pattern_type_compat_964, int_scrutinee_string_literal_rejected) {
    BEZEL_EXPECT_EQ(compileErrored(
        "module M; class C public static method Main(): int "
        "  var x := 5; "
        "  return case x of \"foo\" => 0, _ => 1 end; "
        "end Main; end C;"), true);
}

// §19 (#964): named-const variant — int scrutinee, string named-const pattern.
BEZEL_TEST(case_pattern_type_compat_964, int_scrutinee_string_named_const_rejected) {
    BEZEL_EXPECT_EQ(compileErrored(
        "module M; class C "
        "  public const S: string := \"hi\"; "
        "  public static method Main(): int "
        "    var x := 5; "
        "    return case x of C.S => 0, _ => 1 end; "
        "  end Main; end C;"), true);
}

// §19 (#964): bool scrutinee, int-literal pattern (i1 vs i32) — also a clean
// reject, not an abort.
BEZEL_TEST(case_pattern_type_compat_964, bool_scrutinee_int_literal_rejected) {
    BEZEL_EXPECT_EQ(compileErrored(
        "module M; class C public static method Main(): int "
        "  var b := true; "
        "  return case b of 5 => 0, _ => 1 end; "
        "end Main; end C;"), true);
}

// Regression: a matching int-literal pattern over an int scrutinee compiles and
// matches (no over-rejection).
BEZEL_TEST(case_pattern_type_compat_964, int_literal_match_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x := 5; "
        "  return case x of 5 => 42, _ => 1 end; "  // 42
        "end Main; end C;"), 42);
}

// Regression: a string-literal pattern over a string scrutinee still works.
BEZEL_TEST(case_pattern_type_compat_964, string_literal_match_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hi\"; "
        "  return case s of \"hi\" => 7, _ => 0 end; "  // 7
        "end Main; end C;"), 7);
}

// Regression: a TYPE-MATCHING named const (int const over int scrutinee) compiles
// and matches — the codegen guard only fires on a genuine type mismatch.
BEZEL_TEST(case_pattern_type_compat_964, matching_named_const_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public const K: int := 5; "
        "  public static method Main(): int "
        "    var x := 5; "
        "    return case x of C.K => 99, _ => 0 end; "  // 99
        "  end Main; end C;"), 99);
}

// Regression: a long scrutinee with an int-literal pattern (numeric width
// difference — bridged by reconcile) is NOT rejected and matches.
BEZEL_TEST(case_pattern_type_compat_964, long_scrutinee_int_literal_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n := 5L; "
        "  return case n of 5 => 33, _ => 0 end; "  // 33
        "end Main; end C;"), 33);
}

// Regression: enum patterns over an enum scrutinee are unaffected (excluded from
// the constant-compatibility check — they resolve by name).
BEZEL_TEST(case_pattern_type_compat_964, enum_pattern_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum Color Red, Green; end Color; "
        "class C public static method Main(): int "
        "  var col := Color.Green; "
        "  return case col of Red => 1, Green => 2 end; "  // 2
        "end Main; end C;"), 2);
}
