// Codegen — Console.WriteLine(T?) where T is a nullable value type (§7 /
// stdlib §2.1).
//
// Bug #583 (HIGH): Console.WriteLine(x) where x: int? prints "1" (the i8
// hasValue tag) instead of "42".  Root cause: tryLowerConsoleCall saw
// argTy->kind == Primitive && argTy->primitive == Int (with nullable=true)
// and called emitPrimitiveToString(arg, Int) directly, passing the {i8,i32}
// tagged struct as if it were a plain i32.  snprintf then read the struct's
// bit-pattern as an integer and printed the first byte (hasValue = 1) as "%d".
//
// Bug #581 (CRITICAL): Console.WriteLine(y) where y: bool? → IR verification
// failure "Trunc only operates on integer: trunc {i8,i1} to i1".  Root cause:
// the same code path called emitPrimitiveToString(arg, Bool) on the {i8,i1}
// struct; emitPrimitiveToString's bool branch does
//   builder_.CreateTrunc(val, i1())
// on a struct type, which the IR verifier rejects.
//
// Fix (src/codegen/LlvmBackend_Calls.cpp, Emitter::tryLowerConsoleCall):
// detect isNullableValueTy(argTy) with a struct-valued arg BEFORE the
// existing primitive/named dispatch.  Branch on the hasValue tag:
//   - hasValue true  → extract the inner value, format via emitValueToString
//                      with the non-nullable inner TypeRef.
//   - hasValue false → format as the literal "null".
// This mirrors the nullable-value handling already present in the string-
// interpolation path (LlvmBackend_Exprs.cpp, ~line 127) and in
// emitValueToString (LlvmBackend_String.cpp).
//
// Spec derivation:
//   §7   — nullable types; nullable value types carry {i8 hasValue; T value}
//   §7.2 — null literal
//   §18.5 — interpolated strings (reference for null rendering = "null")
//   stdlib §2.1 — Console.WriteLine overloads

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::JitResult;

namespace {

// Run program with JIT; return exit code or -999 on compile/JIT failure.
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir581583() {
    return wtScratchDir("wt-581583");
}

// Compile `source` to a temp exe and capture its stdout.
// Returns the captured output, or empty on compile failure.
std::string compileAndCapture(const std::string& source) {
    auto dir = wtDir581583();
    std::string stem = unique_stem("bezel_581_583");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }

    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    std::string out;
    if (r.success) {
        if (FILE* pipe = popen(exe.c_str(), "r")) {
            std::array<char, 256> buf;
            while (std::size_t n = std::fread(buf.data(), 1, buf.size(), pipe))
                out.append(buf.data(), n);
            pclose(pipe);
        }
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return out;
}

// Minimal module wrapper for single-statement programs.
const char* kModulePrefix =
    "module M; class C public static method Main(): int ";
const char* kModuleSuffix = " end Main; end C;";

std::string wrap(const std::string& body) {
    return std::string(kModulePrefix) + body + kModuleSuffix;
}

}  // namespace

// --- #583: Console.WriteLine(int? = 42) must print "42\n" ---

// §7 / §18.5 example: a non-null int? must print the wrapped integer value,
// not the has-value tag byte.  Before the fix this printed "1\n".
BEZEL_TEST(codegen_writeline_nullable_583, int_nullable_non_null_prints_value) {
    std::string out = compileAndCapture(wrap(
        "var x: int? := 42; Console.WriteLine(x); return 0;"));
    BEZEL_EXPECT_EQ(out, std::string("42\n"));
}

// A null int? must print "null\n" (matching string interpolation §18.5 /
// emitValueToString's null branch).
BEZEL_TEST(codegen_writeline_nullable_583, int_nullable_null_prints_null) {
    std::string out = compileAndCapture(wrap(
        "var x: int? := null; Console.WriteLine(x); return 0;"));
    BEZEL_EXPECT_EQ(out, std::string("null\n"));
}

// JIT regression: exit code must be reachable (program must not crash / IR-fail).
BEZEL_TEST(codegen_writeline_nullable_583, int_nullable_jit_does_not_crash) {
    BEZEL_EXPECT_EQ(run(wrap(
        "var x: int? := 42; Console.WriteLine(x); return 0;")), 0);
}

// --- #581: Console.WriteLine(bool? = true) must not IR-crash ---

// §7 / §18.5: a non-null bool? must print "True\n" (matching
// emitPrimitiveToString's bool path which uses "True"/"False" globals).
// Before the fix this aborted the compiler with "Trunc only operates on
// integer: trunc {i8,i1} to i1".
BEZEL_TEST(codegen_writeline_nullable_581, bool_nullable_true_prints_true) {
    std::string out = compileAndCapture(wrap(
        "var y: bool? := true; Console.WriteLine(y); return 0;"));
    BEZEL_EXPECT_EQ(out, std::string("True\n"));
}

BEZEL_TEST(codegen_writeline_nullable_581, bool_nullable_false_prints_false) {
    std::string out = compileAndCapture(wrap(
        "var y: bool? := false; Console.WriteLine(y); return 0;"));
    BEZEL_EXPECT_EQ(out, std::string("False\n"));
}

// A null bool? must print "null\n".
BEZEL_TEST(codegen_writeline_nullable_581, bool_nullable_null_prints_null) {
    std::string out = compileAndCapture(wrap(
        "var y: bool? := null; Console.WriteLine(y); return 0;"));
    BEZEL_EXPECT_EQ(out, std::string("null\n"));
}

// JIT: bool? must not IR-crash.
BEZEL_TEST(codegen_writeline_nullable_581, bool_nullable_jit_does_not_crash) {
    BEZEL_EXPECT_EQ(run(wrap(
        "var y: bool? := true; Console.WriteLine(y); return 0;")), 0);
}

// --- double? regression: must remain correct ---

// double? was already working before the fix; this is a regression guard.
BEZEL_TEST(codegen_writeline_nullable_double_regression, double_nullable_non_null_still_works) {
    BEZEL_EXPECT_EQ(run(wrap(
        "var d: double? := 3.5; "
        "var ok := (if d <> null then 1 else 0); return ok;")), 1);
}

// --- non-nullable int regression: must remain correct (§7) ---

// Console.WriteLine(non-nullable int) must not be disturbed by the new
// nullable check.
BEZEL_TEST(codegen_writeline_nullable_regression, non_nullable_int_still_works) {
    std::string out = compileAndCapture(wrap(
        "var n := 7; Console.WriteLine(n); return 0;"));
    BEZEL_EXPECT_EQ(out, std::string("7\n"));
}

// Non-nullable bool must not be disturbed.
BEZEL_TEST(codegen_writeline_nullable_regression, non_nullable_bool_still_works) {
    std::string out = compileAndCapture(wrap(
        "var b := true; Console.WriteLine(b); return 0;"));
    BEZEL_EXPECT_EQ(out, std::string("True\n"));
}
