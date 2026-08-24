// Codegen regression tests for issue #496 — extern "c" FFI name collision.
//
// When a user declares `extern "c" malloc(size: int): IntPtr`, the compiler
// internally has already declared `malloc(i64) -> ptr` (the canonical libc
// signature used by the codegen runtime). LLVM renames the second declaration
// to `malloc.1`, producing a linker error ("undefined reference to malloc.1").
//
// Fix (§32): when a user extern name collides with an already-declared internal
// C function, REUSE the existing LLVM Function and coerce arguments/return at
// the call site instead of creating a second declaration that LLVM would rename.
//
// Test strategy: native-exe tests (compileAndRun) catch the linker error.
// JIT tests (runJit) catch the IR-level rename. Both must link and run.

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDirFfi496() {
    return wtScratchDir("wt-ffi496");
}

// Compile `source` to a temp exe and return the exit code, or -1 on failure.
// Uses the same stem prefix as the ffi_test.cpp helpers to avoid collisions.
int compileAndRun496(const std::string& source) {
    auto dir = wtDirFfi496();
    std::string stem = unique_stem("bezel_ffi496");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }

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

} // namespace

// ---------------------------------------------------------------------------
// §32 / issue #496: user extern "c" collides with an internal C declaration
// ---------------------------------------------------------------------------

// CONTROL: a non-colliding extern (atoi) is unaffected.
// atoi("9") == 9. Proves the FFI path still works for non-colliding names.
BEZEL_TEST(codegen_ffi_496, control_atoi_non_colliding_jit) {
    // per §32 / CG26: non-colliding names must continue to work after the fix.
    auto r = runAot(
        "module M; "
        "extern \"c\" public method atoi(s: CString): int; end; "
        "class C public static method Main(): int "
        "  return atoi(\"9\"); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 9);
}

// CONTROL: strlen (another non-colliding name) still works.
BEZEL_TEST(codegen_ffi_496, control_strlen_non_colliding_jit) {
    // per §32 / CG26: non-colliding extern still resolves correctly.
    auto r = runAot(
        "module M; "
        "extern \"c\" public method strlen(s: CString): int; end; "
        "class C public static method Main(): int "
        "  return strlen(\"hello\"); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// BUG #496: user declares malloc(size: int): IntPtr.
// The compiler internally declares malloc(i64) -> ptr.
// Before the fix: LLVM renames the user decl to "malloc.1" → link error.
// After the fix: the existing declaration is reused; the i32 arg is
// zero-extended to i64 at the call site; the binary links and returns 1.
BEZEL_TEST(codegen_ffi_496, malloc_int_param_no_rename_jit) {
    // per §32 / CG26: extern "c" whose name collides with an internal decl
    // must reuse the existing LLVM Function (no "malloc.1" rename).
    auto r = runAot(
        "module M; "
        "extern \"c\" public method malloc(size: int): IntPtr; end; "
        "class C public static method Main(): int "
        "  var p := malloc(8); "
        "  return 1; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// Same but as a native exe — catches linker errors that the JIT doesn't.
// Before the fix: "undefined reference to malloc.1" at link time.
BEZEL_TEST(codegen_ffi_496, malloc_int_param_no_rename_native) {
    // per §32 / CG26: native link must not produce "undefined reference to
    // malloc.1" when the user declares extern "c" malloc(size: int): IntPtr.
    int code = compileAndRun496(
        "module M; "
        "extern \"c\" public method malloc(size: int): IntPtr; end; "
        "class C public static method Main(): int "
        "  var p := malloc(8); "
        "  return 1; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(code, 1);
}

// User declares free(p: IntPtr) (no return). free is also used internally.
// Before the fix: a second "free" or "free.1" declaration causes rename/error.
BEZEL_TEST(codegen_ffi_496, free_intptr_param_no_rename_jit) {
    // per §32 / CG26: extern "c" free with IntPtr param reuses the internal
    // free declaration (void @free(ptr)) without creating a renamed alias.
    auto r = runAot(
        "module M; "
        "extern \"c\" public method free(p: IntPtr); end; "
        "class C public static method Main(): int "
        "  return 5; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// User declares calloc(n: int, size: int): IntPtr.
// The compiler internally uses calloc(i64, i64) -> ptr.
BEZEL_TEST(codegen_ffi_496, calloc_int_params_no_rename_jit) {
    // per §32 / CG26: calloc collision resolved by reusing internal declaration.
    auto r = runAot(
        "module M; "
        "extern \"c\" public method calloc(n: int, size: int): IntPtr; end; "
        "class C public static method Main(): int "
        "  var p := calloc(1, 8); "
        "  return 2; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}

// malloc+free round-trip: allocate, then free via user extern "c" decls.
// Exercises both the malloc coerce path and the free coerce path together.
BEZEL_TEST(codegen_ffi_496, malloc_free_round_trip_native) {
    // per §32 / CG26: malloc collision + free collision both resolved;
    // native binary links and runs cleanly (no malloc.1 or free.1).
    int code = compileAndRun496(
        "module M; "
        "extern \"c\" public method malloc(size: int): IntPtr; end; "
        "extern \"c\" public method free(p: IntPtr); end; "
        "class C public static method Main(): int "
        "  var p := malloc(16); "
        "  free(p); "
        "  return 3; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(code, 3);
}
