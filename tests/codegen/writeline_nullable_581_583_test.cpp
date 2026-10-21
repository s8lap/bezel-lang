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

// #6778 (§7.1, §22.6, v0.340): true iff `src` is rejected with the §7 text at
// the argument of the first `call` (the position just past its `(`).
bool rejectsNullableConsoleArg(const std::string& src, const std::string& call,
                               const std::string& callee) {
    const std::size_t at = src.find(call);
    if (at == std::string::npos) return false;
    const std::size_t arg = at + call.find('(') + 1;
    std::size_t line = 1, lineStart = 0;
    for (std::size_t i = 0; i < arg; ++i)
        if (src[i] == '\n') { ++line; lineStart = i + 1; }
    const std::string want =
        ":" + std::to_string(line) + ":" + std::to_string(arg - lineStart + 1) +
        ": the argument to `" + callee +
        "` is nullable but the target type is non-nullable (\xC2\xA7" "7)";
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(want) != std::string::npos)
            return true;
    return false;
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

// A null int? at `Console.WriteLine` used to print "null\n". Owner ruling on
// #6778 (§7.1, §22.6, v0.340; stdlib §2.1): `WriteLine`'s parameter is
// non-nullable, so an un-narrowed `int?` is a compile error; the
// interpolation hole `$"{x}"` is the remedy that renders `null` (§18.5).
BEZEL_TEST(codegen_writeline_nullable_583, int_nullable_null_rejected_6778_7_1) {
    BEZEL_EXPECT_TRUE(rejectsNullableConsoleArg(
        wrap("var x: int? := null; Console.WriteLine(x); return 0;"),
        "Console.WriteLine(x)", "Console.WriteLine"));
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

// A null bool? at `Console.WriteLine` used to print "null\n"; it is now a
// compile error (owner ruling on #6778; §7.1, §22.6, v0.340).
BEZEL_TEST(codegen_writeline_nullable_581, bool_nullable_null_rejected_6778_7_1) {
    BEZEL_EXPECT_TRUE(rejectsNullableConsoleArg(
        wrap("var y: bool? := null; Console.WriteLine(y); return 0;"),
        "Console.WriteLine(y)", "Console.WriteLine"));
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
