// Fix verification tests for bug #474.
//
// #474 (CRASH / miscompile): a string Range index `s[a..b]` (spec §3.4 / §4.6,
// stdlib §9.6) did not produce a `string`. Three observable symptoms:
//   * `if s[1..3] = "ell" then …` aborted bezelc at IR-verify:
//       "Both operands to ICmp instruction are not of the same type!"
//   * `s[1..3].Length` returned 0.
//   * `var sub: string := s[1..3]` was rejected ("not implicitly convertible").
//
//   Root cause (confirmed):
//     * Sema (ExprTypecheck_Exprs.cpp): an IndexExpr on a string was ALWAYS typed
//       `char`, regardless of whether the index was an int (→ char, §4.6) or a
//       Range (→ substring string, §3.4). So `s[1..3]` was typed `char`.
//     * Codegen (LlvmBackend_Exprs.cpp): the string-indexer path ALWAYS called
//       loadCodePoint (producing a char i32), even when the index was a Range.
//       Comparing that char to a string literal built an ICmp with one i32 and
//       one ptr operand → verifier abort.
//
//   Fix:
//     * Sema types `s[Range]` as `string` (§3.4).
//     * Codegen lowers `s[Range]` to a fresh, width-preserving `%bz.string`
//       substring (a fresh +1 runtime-managed string per §3.4 / §4.6), honoring
//       the spec's bound semantics.
//
//   Range bound semantics (spec §16.1 / §3.4, AUTHORITATIVE):
//     * `a..b`  is INCLUSIVE on both ends → indices a..b → length b - a + 1.
//     * `a..<b` is half-open (upper excluded) → indices a..b-1 → length b - a.
//     * `..b` / `..<b` start at 0; `a..` runs through the last index; `..` is all.
//     * Out-of-range endpoints raise IndexOutOfRangeException; an empty range is "".
//   So `"hello"[1..3]` == "ell" (len 3), `"hello"[1..<3]` == "el" (len 2).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── #474 core: the exact verified reproducer ────────────────────────────────

// §3.4 / §16.1 / #474: `s[1..3]` (inclusive) is "ell"; equality against a string
// literal must build a STRING `=` (not a malformed ICmp). Before fix: verifier
// abort. Expected: 1.
BEZEL_TEST(bug474_string_slice, inclusive_slice_equals_literal_474) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  if s[1..3] = \"ell\" then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §3.4 / §16.1 / #474: the inclusive slice `[1..3]` of "hello" has length 3
// ('e','l','l'). Before fix: 0. Expected: 3.
BEZEL_TEST(bug474_string_slice, inclusive_slice_length_474) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  return s[1..3].Length; "   // expect 3
        "end Main; end C;"), 3);
}

// §3.4 / §5.1 / #474: assigning a string slice to an explicit `string` local
// must type-check (the slice is a `string`). Before fix: "not implicitly
// convertible". Expected: 1 (the assigned value compares equal to "ell").
BEZEL_TEST(bug474_string_slice, assign_slice_to_string_local_474) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  var sub: string := s[1..3]; "
        "  if sub = \"ell\" then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── R33: the index is a Range *VARIABLE* (not a syntactic range literal) ─────
// `var r := a..b; s[r]`. Before the R33 fix this mistyped the result as `char`
// (sema only recognized a RangeExpr literal as the substring form) AND the
// codegen single-char path sext'd the whole %bz.range struct → an IR-verify
// failure ("SExt only operates on integer"). Fix: sema types `s[Range-value]`
// as `string`, and codegen slices from the struct fields.

// R33: inclusive range value `0..2` → "hel" (len 3). The original repro.
BEZEL_TEST(bug474_string_slice, range_var_inclusive_length_r33) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; var r := 0..2; "
        "  var sub := s[r]; "
        "  return sub.Length; "   // expect 3
        "end Main; end C;"), 3);
}

// R33: inclusive range value `1..3` → content equals "ell".
BEZEL_TEST(bug474_string_slice, range_var_inclusive_content_r33) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; var r := 1..3; "
        "  if s[r] = \"ell\" then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// R33: half-open range value `1..<3` → "el" (the open/inclusive flag bit is read
// from the struct, not the AST).
BEZEL_TEST(bug474_string_slice, range_var_halfopen_content_r33) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; var r := 1..<3; "
        "  if s[r] = \"el\" then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// R33: open-upper range value `2..` → "llo" (the endOpen flag runs to the end).
BEZEL_TEST(bug474_string_slice, range_var_open_upper_content_r33) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; var r := 2..; "
        "  if s[r] = \"llo\" then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// R33: an out-of-range range value raises IndexOutOfRangeException (the bounds
// check fires on the dynamically-extracted endpoints too).
BEZEL_TEST(bug474_string_slice, range_var_out_of_range_throws_r33) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hi\"; var r := 0..10; "
        "  try return s[r].Length; "
        "  catch IndexOutOfRangeException => return 55; end; "
        "end Main; end C;"), 55);
}

// ── #474: half-open vs inclusive bound semantics (spec §16.1) ────────────────

// §3.4 / §16.1 / #474: the half-open form `[1..<3]` EXCLUDES the upper bound →
// "el" (len 2).
BEZEL_TEST(bug474_string_slice, halfopen_slice_value_474) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  if s[1..<3] = \"el\" then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

BEZEL_TEST(bug474_string_slice, halfopen_slice_length_474) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  return s[1..<3].Length; "   // expect 2
        "end Main; end C;"), 2);
}

// ── #474: open-ended slice forms (spec §16.2 / §3.4) ────────────────────────

// §3.4 / §16.2 / #474: `s[..2]` (inclusive) runs from start through index 2 →
// "hel" (len 3).
BEZEL_TEST(bug474_string_slice, open_lower_inclusive_474) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  if s[..2] = \"hel\" then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §3.4 / §16.2 / #474: `s[..<2]` (half-open) → "he" (len 2).
BEZEL_TEST(bug474_string_slice, open_lower_halfopen_474) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  if s[..<2] = \"he\" then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §3.4 / §16.2 / #474: `s[2..]` runs from index 2 through the last char → "llo".
BEZEL_TEST(bug474_string_slice, open_upper_474) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  if s[2..] = \"llo\" then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §3.4 / §16.2 / #474: `s[..]` is the whole string → "hello" (len 5).
BEZEL_TEST(bug474_string_slice, open_full_474) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  return s[..].Length; "   // expect 5
        "end Main; end C;"), 5);
}

// §3.4 / §16.1 / #474: a single-element inclusive slice `[2..2]` → "l" (len 1).
BEZEL_TEST(bug474_string_slice, single_element_inclusive_474) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  if (s[2..2] = \"l\") and (s[2..2].Length = 1) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §3.4 / §16.1 / #474: an empty half-open slice `[2..<2]` → "" (len 0).
BEZEL_TEST(bug474_string_slice, empty_halfopen_slice_474) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  return s[2..<2].Length; "   // expect 0
        "end Main; end C;"), 0);
}

// §3.4 / #474: the empty slice equals the empty string literal.
BEZEL_TEST(bug474_string_slice, empty_slice_equals_empty_string_474) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  if s[2..<2] = \"\" then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── #474: computed bounds (the slice is fully runtime) ──────────────────────

// §3.4 / §16.1 / #474: bounds computed from locals, not literals.
BEZEL_TEST(bug474_string_slice, computed_bounds_474) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"abcdef\"; var a := 1; var b := 4; "
        "  if s[a..b] = \"bcde\" then return 1; end; "   // inclusive 1..4
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── #474: the slice composes with string operations ─────────────────────────

// §3.4 / §4.6 / #474: a slice is a real `string` — usable in interpolation.
// "[ell]" has length 5.
BEZEL_TEST(bug474_string_slice, slice_in_interpolation_474) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  var t := $\"[{s[1..3]}]\"; "
        "  return t.Length; "   // "[ell]" → 5
        "end Main; end C;"), 5);
}

// §3.4 / §4.6 / #474: the interpolated slice has the expected content.
BEZEL_TEST(bug474_string_slice, slice_interpolation_content_474) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  var t := $\"[{s[1..3]}]\"; "
        "  if t = \"[ell]\" then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §3.4 / §4.6 / #474: a slice is a real `string` — concatenation works.
BEZEL_TEST(bug474_string_slice, slice_concat_474) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  var t := s[0..1] + s[3..4]; "   // "he" + "lo" = "helo"
        "  if t = \"helo\" then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §3.4 / stdlib §9.6 / #474: a slice supports chained string methods. ToUpper
// uppercases — "ELL".
BEZEL_TEST(bug474_string_slice, slice_chained_method_474) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  if s[1..3].ToUpper() = \"ELL\" then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §3.4 / §4.6 / #474: a slice indexed again yields the right char (proving the
// slice's storage is a proper, indexable string). s[1..3] == "ell"; [0] == 'e'.
BEZEL_TEST(bug474_string_slice, slice_then_char_index_474) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  return s[1..3][0]; "   // 'e' = 101
        "end Main; end C;"), 101);
}

// §3.4 / §4.6 / #474: a slice passed as a `string` parameter.
BEZEL_TEST(bug474_string_slice, slice_as_string_param_474) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Len(x: string): int return x.Length; end Len; "
        "  public static method Main(): int "
        "    var s := \"hello\"; "
        "    return Len(s[1..3]); "   // expect 3
        "  end Main; "
        "end C;"), 3);
}

// ── #474: out-of-range endpoints raise IndexOutOfRangeException (§3.4) ───────

// §3.4 / #474: an upper endpoint past the last index raises
// IndexOutOfRangeException. A `try`/`catch` proves the throw is catchable.
BEZEL_TEST(bug474_string_slice, oor_upper_throws_474) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; var r := 5; "
        "  try var t := s[1..9]; r := t.Length; "   // 9 is out of range (len 5)
        "  catch IndexOutOfRangeException => r := 7; end; "
        "  return r; "                              // catch fires → 7
        "end Main; end C;"), 7);
}

// §3.4 / #474: a negative lower endpoint raises IndexOutOfRangeException.
BEZEL_TEST(bug474_string_slice, oor_negative_lower_throws_474) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; var a := -1; var b := 2; var r := 5; "
        "  try var t := s[a..b]; r := t.Length; "
        "  catch IndexOutOfRangeException => r := 7; end; "
        "  return r; "                              // catch fires → 7
        "end Main; end C;"), 7);
}

// ── #474 regression: the single-int index path is unchanged (§4.6) ──────────

// §4.6 / #474: `s[0]` still returns the first CHAR (not a 1-length string).
BEZEL_TEST(bug474_string_slice, regression_single_char_index_474) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  return s[0]; "   // 'h' = 104
        "end Main; end C;"), 104);
}

// §4.6 / #474: a single-int index compared to a char literal still works.
BEZEL_TEST(bug474_string_slice, regression_char_index_equals_474) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  if s[1] = 'e' then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §4.1 / §4.6 / §31.2 (R33): a far out-of-range single-int string index raised
// no bounds check and SEGFAULTed; it must raise IndexOutOfRangeException.
BEZEL_TEST(bug474_string_slice, single_char_index_out_of_range_throws_r33) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hi\"; "
        "  try var c := s[1000000]; return 1; "
        "  catch IndexOutOfRangeException => return 55; end; "
        "end Main; end C;"), 55);
}

// ════════════════════════════════════════════════════════════════════════════
// #474 allocation correctness — native build + LeakSanitizer.
//
// A string slice allocates a fresh, runtime-managed `string` (§3.4 / §4.6). As
// a runtime-managed value its memory is reclaimed automatically; the producer
// holds the +1 reference and `bz_string_release` frees it when the last
// reference drops. This leg proves no leak / no double-free for the slice
// allocation. Mirrors bug_465_test.cpp's LSan harness; the JIT harness cannot
// host LSan (its in-process ORC state is intentionally never freed).
// ════════════════════════════════════════════════════════════════════════════

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir474() {
    return wtScratchDir("wt-474");
}

int compileAndRun474(const std::string& source) {
    auto dir = wtDir474();
    std::string stem = unique_stem("bezel_474");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    {
        std::ofstream out(src);
        out << source;
    }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    int code = -1;
    if (r.success) {
        int status = std::system(exe.c_str());
        if (status != -1 && WIFEXITED(status)) code = WEXITSTATUS(status);
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return code;
}

int runUnderLeakSanitizer474(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = wtDir474();
    std::string stem = unique_stem("bezel_474_lsan");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();

    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return -1;
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return -1;
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return -1;
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err)) return -1;
    }

    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };

    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -3; }

    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -3;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer
    return 0;                                                      // clean
}

void expectRunAndLeakFree474(const std::string& source, int expectedExit) {
    BEZEL_EXPECT_EQ(compileAndRun474(source), expectedExit);
    int lsan = runUnderLeakSanitizer474(source);
    if (lsan == -1) return;       // toolchain unavailable: skip the LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);     // 0 == LSan clean (no leak, no UAF/DF)
}

// §3.4 / §4.6 / #474: a slice bound to a local then used — the fresh string is
// allocated and reclaimed once. Exit code is the slice length (3).
const char* kSliceLocalNoLeak =
    "module M; class C "
    "  public static method Main(): int "
    "    var s := \"hello\"; "
    "    var sub := s[1..3]; "
    "    return sub.Length; "
    "  end Main; "
    "end C;";

}  // namespace

BEZEL_TEST(bug474_string_slice, slice_local_no_leak_474) {
    expectRunAndLeakFree474(kSliceLocalNoLeak, 3);
}
