// Driver robustness — parse errors must stop the pipeline before sema/ownership
// (#1622, §2.6 build phases).
//
// `Driver::compileFile` / `Driver::compileObject` used to run symbol
// collection, type resolution, type-check, and ownership checking
// UNCONDITIONALLY after parsing, gating only the entry-point check and codegen
// on `!diagnostics.hasErrors()`. The parser returns an error-recovery AST even
// after queueing parse diagnostics, and the front-end passes unconditionally
// deref AST children that the grammar guarantees non-null but a partial
// recovery leaves null. Running them on such a tree crashed the compiler — a
// `unique_ptr::operator*()` abort (SIGABRT) — before the already-queued parse
// diagnostic was ever printed. #1615 was one such deref (a null
// `MemberAccessExpr::target`); this is the class.
//
// A minimal reproducer is a `const` whose initializer is a malformed binary
// expression, e.g. `public const K: int := 1 + ;`. The parser reports
// "expected expression" and recovers with a `BinaryExpr` whose right operand is
// null; that node survives into the `ConstDecl`, and the type-check pass
// dereferenced the null `unique_ptr<Expr>` operand, aborting the process.
//
// Per §2.6 (build phases) the pipeline is staged: parse → resolve → type-check
// → codegen. The fix gates the whole front-end on a clean parse (exactly as
// codegen already gated on a clean front-end), so a parse error now surfaces
// the parser's own diagnostic and stops, instead of proceeding to the later
// phases and derefing an incomplete AST. Valid programs are unaffected.
//
// The primary regression signal is that these calls RETURN at all — before the
// fix each aborted the test process on the `unique_ptr` assertion. They are
// still expected to be genuine compile failures (success == false), not
// silently-accepted programs.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir1622() {
    return wtScratchDir("wt-1622");
}

// Write `source` to a temp .bzl file and compile it to `outExe` via the
// executable path (Driver::compileFile). Returns the CompileResult. Removes the
// source file; the caller removes `outExe` if it was created.
bezel::CompileResult compileToExe(const std::string& source,
                                  const std::string& outExe) {
    auto dir = wtDir1622();
    std::string src = (dir / ("bezel_1622_" + unique_stem("") + ".bzl")).string();
    {
        std::ofstream out(src);
        out << source;
    }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, outExe);
    std::remove(src.c_str());
    return r;
}

// Same as compileToExe but exercises the library/object path
// (Driver::compileObject, the `-c` route), which had the same unconditional
// front-end-on-recovery-AST bug.
bezel::CompileResult compileToObj(const std::string& source,
                                  const std::string& outObj) {
    auto dir = wtDir1622();
    std::string src = (dir / ("bezel_1622_" + unique_stem("") + ".bzl")).string();
    {
        std::ofstream out(src);
        out << source;
    }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileObject(src, outObj);
    std::remove(src.c_str());
    return r;
}

// Remove `path` if it exists (stale-output + cleanup helper).
void scrub(const std::string& path) {
    if (fs::exists(path)) std::remove(path.c_str());
}

} // namespace

// ── #1622: a parse error no longer crashes the executable path ───────────────

// The issue's minimal reproducer: a malformed `const` initializer (`1 + ;`)
// leaves a null `BinaryExpr` operand in the recovery AST. Before the fix the
// driver ran type-check on that AST and aborted the process on the null
// `unique_ptr` deref. Must now merely report the parser's own diagnostic and
// return failure status — not abort.
BEZEL_TEST(codegen_driver_no_crash_after_parse_error_1622,
           malformed_const_initializer_does_not_crash_exe_path) {
    auto dir = fs::temp_directory_path();
    std::string exePath = (dir / "bezel_1622_const_crash_out").string();
    scrub(exePath);

    bezel::CompileResult r = compileToExe(
        "module M; class C public const K: int := 1 + ; end C;",
        exePath);

    // Returning here — rather than aborting on the pre-fix unique_ptr
    // assertion — is the primary regression signal. The malformed input is
    // still a genuine compile error, not a silently-accepted program.
    BEZEL_EXPECT_FALSE(r.success);

    // No output binary may be produced for a failing compilation.
    bool exists = fs::exists(exePath);
    scrub(exePath);
    BEZEL_EXPECT_FALSE(exists);
}

// A second malformed-initializer shape (an unclosed parenthesized sub-
// expression) recovers to the same null-operand BinaryExpr and must likewise
// stop cleanly at the parse diagnostic instead of aborting.
BEZEL_TEST(codegen_driver_no_crash_after_parse_error_1622,
           malformed_const_initializer_paren_variant_does_not_crash) {
    auto dir = fs::temp_directory_path();
    std::string exePath = (dir / "bezel_1622_const_paren_crash_out").string();
    scrub(exePath);

    bezel::CompileResult r = compileToExe(
        "module M; class C public const K: int := (1 + ; end C;",
        exePath);

    BEZEL_EXPECT_FALSE(r.success);
    bool exists = fs::exists(exePath);
    scrub(exePath);
    BEZEL_EXPECT_FALSE(exists);
}

// ── #1622: the library/object path (`-c`) is fixed too ───────────────────────

// Driver::compileObject had the same unconditional front-end-on-recovery-AST
// run as compileFile; the same malformed const must stop cleanly here too.
BEZEL_TEST(codegen_driver_no_crash_after_parse_error_1622,
           malformed_const_initializer_does_not_crash_object_path) {
    auto dir = fs::temp_directory_path();
    std::string objPath = (dir / "bezel_1622_obj_crash_out.o").string();
    scrub(objPath);

    bezel::CompileResult r = compileToObj(
        "module M; class C public const K: int := 1 + ; end C;",
        objPath);

    BEZEL_EXPECT_FALSE(r.success);
    bool exists = fs::exists(objPath);
    scrub(objPath);
    BEZEL_EXPECT_FALSE(exists);
}

// ── Control: valid programs still compile exactly as before ──────────────────

// §2.6: a program with NO parse errors must still run the full front-end and
// produce a runnable binary — the parse gate must not suppress legitimate
// downstream compilation of valid programs.
BEZEL_TEST(codegen_driver_no_crash_after_parse_error_1622,
           valid_program_still_compiles_exe_path) {
    auto dir = fs::temp_directory_path();
    std::string exePath = (dir / "bezel_1622_valid_exe_out").string();
    scrub(exePath);

    bezel::CompileResult r = compileToExe(
        "module M; class C public const K: int := 42; "
        "public static method Main(): int return C.K; end Main; end C;",
        exePath);

    BEZEL_EXPECT_TRUE(r.success);
    scrub(exePath);
}

// A valid library (no `Main`, §33.6) must still compile to an object via the
// `-c` path.
BEZEL_TEST(codegen_driver_no_crash_after_parse_error_1622,
           valid_library_still_compiles_object_path) {
    auto dir = fs::temp_directory_path();
    std::string objPath = (dir / "bezel_1622_valid_obj_out.o").string();
    scrub(objPath);

    bezel::CompileResult r = compileToObj(
        "module M; class C public const K: int := 42; end C;",
        objPath);

    BEZEL_EXPECT_TRUE(r.success);
    scrub(objPath);
}
