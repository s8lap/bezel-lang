// Codegen — async method returning Task<T?> with a literal (#493).
//
// §30.1: `return <expr>` in an async method completes the outer Task with the
// given value. When T is a NULLABLE VALUE TYPE (int?, double?, bool?, enum?),
// the returned plain value (e.g. literal 42) must be WRAPPED into the full
// {hasValue=1, value} struct before being stored into the task result buffer.
//
// Bug: the async return lowering stored the raw primitive directly into an
// alloca typed for the nullable struct ({i8, T}). Because the store's value
// type drove the write width (not the alloca's type), for int? ({i8, i32}):
//   - the i32 was stored at offset 0 (4 bytes), leaving the hasValue byte
//     (offset 0) equal to the low byte of the literal and the i32 value field
//     (offset 4) zero — so the caller saw hasValue!=0 but value=0.
// For double?/enum? the hasValue byte was 0 (first byte of an IEEE double or
// the value 0) so the caller treated the result as null.
//
// Tests are AOT/driver native-executable style (same harness as async_test.cpp
// and async_451_test.cpp — async emitter state accumulates across JIT runs).
// Each test encodes the repro scenario and the expected exit code; the fix is
// confirmed by all tests changing from "wrong result" to the correct value.
//
// Derived from: §30.1 (async return completes task), §7.1/§7.2 (nullable
// value types lower to {i8 hasValue; T value}), §30.1/§30.5 (result buffer
// sizing and copying).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
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
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir493() {
    return wtScratchDir("wt-493");
}

// Compile `source` to a temp native executable via the real Driver and run it.
// Returns the process exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    auto dir = wtDir493();
    std::string stem = unique_stem("bezel_async493");
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

}  // namespace

// ===========================================================================
// §30.1 / §7.1 / §7.2: Task<int?> return with a literal
// ===========================================================================

// §30.1 / §7.2 (#493): `return 42` in an async Task<int?> method must produce
// a non-null int? whose value is 42. The caller awaits and uses `?? 99` as the
// fallback; only the correct path (hasValue=1, value=42) yields exit code 42.
// Before the fix: hasValue=1 but value=0 → exit 0. After fix: exit 42.
BEZEL_TEST(async_nullable_return_493, int_nullable_literal_42) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method GetInt(): Task<int?> "
        "    return 42; "
        "  end GetInt; "
        "  public static async method Main(): Task<int> "
        "    var v := await GetInt(); "
        "    return v ?? 99; "
        "  end Main; end C;"),
        42);
}

// §30.1 / §7.2 (#493): `return null` in an async Task<int?> method must
// produce a null int?. The caller uses `?? 7` as fallback → exit 7.
BEZEL_TEST(async_nullable_return_493, int_nullable_null_return) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method GetNull(): Task<int?> "
        "    return null; "
        "  end GetNull; "
        "  public static async method Main(): Task<int> "
        "    var v := await GetNull(); "
        "    return v ?? 7; "
        "  end Main; end C;"),
        7);
}

// §30.1 / §7.2 (#493): the workaround path (`var n: int? := 42; return n;`)
// must continue working — confirms the variable-return path is unaffected by
// the fix. Expected: exit 42.
BEZEL_TEST(async_nullable_return_493, int_nullable_variable_return) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method GetViaVar(): Task<int?> "
        "    var n: int? := 42; "
        "    return n; "
        "  end GetViaVar; "
        "  public static async method Main(): Task<int> "
        "    var v := await GetViaVar(); "
        "    return v ?? 99; "
        "  end Main; end C;"),
        42);
}

// ===========================================================================
// §30.1 / §7.1 / §7.2: Task<double?> return with a literal
// ===========================================================================

// §30.1 / §7.2 (#493): `return 3.0` in async Task<double?> — the 16-byte
// {i8, pad, double} struct must be fully written with hasValue=1, value=3.0.
// Main casts v! to int → exit 3. Before fix: hasValue=0 → treated null →
// takes the `return 1` branch → exit 1. After fix: exit 3.
BEZEL_TEST(async_nullable_return_493, double_nullable_literal) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method GetD(): Task<double?> "
        "    return 3.0; "
        "  end GetD; "
        "  public static async method Main(): Task<int> "
        "    var v := await GetD(); "
        "    if v = null then return 1; end; "
        "    return int(v!); "
        "  end Main; end C;"),
        3);
}

// ===========================================================================
// §30.1 / §7.1 / §7.2: Task<bool?> return — control case
// ===========================================================================

// §30.1 / §7.2 (#493): `return true` in async Task<bool?> must produce a
// non-null bool? with value=true. Main: if v <> null and v! then 5 else 1.
// bool? is {i8, i1} — the hasValue byte and a 1-bit value. The fix must
// handle this shape too (the first byte of i1 true is 1, which would land in
// the wrong field without the wrap). Expected: exit 5.
BEZEL_TEST(async_nullable_return_493, bool_nullable_literal_true) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method GetBool(): Task<bool?> "
        "    return true; "
        "  end GetBool; "
        "  public static async method Main(): Task<int> "
        "    var v := await GetBool(); "
        "    if v = null then return 1; end; "
        "    if v! then return 5; end; "
        "    return 2; "
        "  end Main; end C;"),
        5);
}

// ===========================================================================
// §30.1 control: Task<int> (non-nullable) — must not regress
// ===========================================================================

// §30.1 control case: Task<int> (non-nullable) returning a literal.
// Confirms the fix doesn't break the common non-nullable case. Expected: 42.
BEZEL_TEST(async_nullable_return_493, int_nonull_control) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method GetPlain(): Task<int> "
        "    return 42; "
        "  end GetPlain; "
        "  public static async method Main(): Task<int> "
        "    var v := await GetPlain(); "
        "    return v; "
        "  end Main; end C;"),
        42);
}

// ===========================================================================
// §30.1 / §7.2 / §10: Task<enum?> — nullable enum return
// ===========================================================================

// §30.1 / §10 / §7.2 (#493): `return` an enum variant from async Task<enum?>
// must wrap it into {hasValue=1, value=<int>}. Color.Green := 1 (the explicit
// underlying integer). Main reads v! and casts to int → exit = 1.
BEZEL_TEST(async_nullable_return_493, enum_nullable_literal) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "enum Color Red := 0, Green := 1, Blue := 2; end Color; "
        "class C "
        "  public static async method GetColor(): Task<Color?> "
        "    return Color.Green; "
        "  end GetColor; "
        "  public static async method Main(): Task<int> "
        "    var v := await GetColor(); "
        "    if v = null then return 9; end; "
        "    return int(v!); "
        "  end Main; end C;"),
        1);
}
