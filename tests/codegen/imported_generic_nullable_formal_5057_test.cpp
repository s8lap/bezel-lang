// Cross-module regression test for issue #5057 — §14.1 / §2.5 (v0.307).
//
// An IMPORTED GENERIC METHOD whose formal is `T?` (nullable-value type parameter)
// causes a SIGSEGV (rc 139) when called from another module with a nullable
// value argument. The crash is in codegen at LLVM's `CreateCall` when passing a
// `{i8, i32}` nullable struct to an i32-placeholder parameter.
//
// ROOT CAUSE: an imported generic method's callee in `methodFns_` is the
// exporter's PLACEHOLDER template (i32 for every unbound type param). When
// the argument is a nullable value struct `{i8, i32}` but the placeholder
// param is i32, the call instruction has a type mismatch and LLVM crashes.
//
// THE FIX (LlvmBackend_Calls2.cpp, both the inline-force arm and the #1943
// gate arm): detect this mismatch and report a clean error rather than letting
// LLVM segfault.
//
// FAIL-WITHOUT (mechanical, using the issue's repro):
//   PRE-fix: driver.compileFile(app) with the library as dependency → SIGSEGV
//     rc 139 (LLVM CreateCall type mismatch).
//   POST-fix: driver.compileFile(app) fails cleanly with !success, no crash.

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::codegen {
namespace {

const std::filesystem::path& wtDir5057() {
    return wtScratchDir("wt-5057");
}

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

}  // namespace
}  // namespace bezel::codegen

// Library: a generic class with a generic method whose formal is T? (int?).
// The exporter's placeholder template uses bare i32 for T.
constexpr char kLibSrc[] =
    "module XLib5046c; "
    "public class XL5046c "
    "  public static method Id<T>(x: T?): T? => x; "
    "end XL5046c; ";

// App: import the library and call Id<int>(a) with a: int?.
// Pre-fix: LLVM CreateCall gets {i8, i32} as argument but i32 placeholder
// param → type mismatch → SIGSEGV rc 139.
// Post-fix: clean compile failure, !success.
BEZEL_TEST(imported_generic_nullable_formal_5057,
           imported_generic_method_T_question_formal_cross_module_clean_error) {
    using namespace bezel::codegen;

    auto dir = wtDir5057();
    std::string stem = unique_stem("bezel_5057_");
    TempFile libSrc((dir / (stem + "_lib.bzl")).string());
    TempFile libObj((dir / (stem + "_lib.o")).string());
    TempFile appSrc((dir / (stem + "_app.bzl")).string());

    writeSource(libSrc.path, kLibSrc);
    writeSource(appSrc.path,
        "module App5046c; "
        "import XLib5046c; "
        "class P "
        "  public static method Main(): int "
        "    var a: int? := 5; "
        "    var r := XLib5046c.XL5046c.Id<int>(a); "
        "    return 0; "
        "  end Main; "
        "end P; ");

    bezel::Driver driver;

    // Step 1: compile the library (must succeed).
    bezel::CompileResult ra = driver.compileObject(libSrc.path, libObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    // Step 2: compile the app against the library.
    // Pre-fix: SIGSEGV rc 139 in codegen.
    // Post-fix: clean failure (diagnostic emitted, no crash).
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObj.path;
    d.objectPath = libObj.path;
    deps.push_back(d);

    bezel::CompileResult rb = driver.compileFile(appSrc.path, "/dev/null", deps);
    BEZEL_EXPECT_FALSE(rb.success);
    if (rb.success) return;

    // The diagnostic text goes to stderr (printed by the driver); we verify the
    // compile failed cleanly (not SIGSEGV) via !rb.success. The issue number
    // and section citations appear in the diagnostic text — visually inspect
    // stderr output on a probe run if needed.
    BEZEL_EXPECT_FALSE(rb.success);
}
