// fix #5242 / codegen soundness (§14.1, v0.313): a generic record instantiated
// with a builtin class (`Box<Stopwatch>`, `Box<StringBuilder>`, `Box<CancellationTokenSource>`)
// triggered an ASan heap-buffer-overflow on field store/read.
//
// Root cause: lowerNew's generic record path (LlvmBackend_Exprs2.cpp, the
// `allUnbound` check) treated a builtin class argument (Stopwatch) as "unbound"
// because builtin types have a non-null declaration (builtinClassSym).
// With allUnbound=true, no substitution was installed, so recordStruct
// used the unspecialized {i32} placeholder layout for T, causing an 8-byte
// Stopwatch ptr to be stored/read from a 4-byte slot — ASan buffer overflow.
//
// Fix: recognize builtin type names as concrete in the allUnbound check.
// The same builtin-check fix is applied to RecordSubstGuard and the
// !allUnbound branch for completeness.
//
// Observable: process exit code (Main's returned int).
// ASan: runAotChecked uses --sanitize=address; confirms no overflow after fix.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── §14.1: builtin class as generic type argument — layout must use ptr. --------
// ASan: without the fix, a heap-buffer-overflow on the 8-byte field write
// (builtin ptr) into a 4-byte slot ({i32} placeholder layout).
BEZEL_TEST(box_builtin_class_soundness_5242, record_box_stopwatch_field_read) {
    // ASan oracle: runAotChecked uses --sanitize=address.
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System.Diagnostics; "
        "record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  using var s := new Stopwatch(); "
        "  var b := new Box<Stopwatch>(s); "
        "  s.Start(); "
        "  var sw := b.value; "
        "  if sw.IsRunning then return 7; end; "
        "  return 1; "
        "end Main; end C;"), 7);
}

// ── §14.1: Box<StringBuilder> — another builtin class. -------------------------
BEZEL_TEST(box_builtin_class_soundness_5242, record_box_stringbuilder_field_read) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System.Text; "
        "record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"hello\"); "
        "  var b := new Box<StringBuilder>(sb); "
        "  var g := b.value; "
        "  if g.Length > 0 then return 7; end; "
        "  return 1; "
        "end Main; end C;"), 7);
}

// ── §14.1: Box<CancellationTokenSource>. --------------------------------------
BEZEL_TEST(box_builtin_class_soundness_5242, record_box_cancellation_token_source_field_read) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System.Threading; "
        "record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  using var cts := new CancellationTokenSource(); "
        "  var b := new Box<CancellationTokenSource>(cts); "
        "  var g := b.value; "
        "  if g.Token.IsCancellationRequested = false then return 7; end; "
        "  return 1; "
        "end Main; end C;"), 7);
}
