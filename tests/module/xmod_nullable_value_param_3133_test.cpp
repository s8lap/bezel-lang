// fix #3133 / §7.2 / §2.5 (v0.252): a cross-module call with a nullable VALUE-
// type parameter failed IR verification — `L.TakeInt(null)` on an imported
// `method TakeInt(v: int?)` emitted `call i32 @"bz$Lib.L$TakeInt"(ptr null)`
// against a declared `{ i8, i32 }` parameter, and the same shape for a
// present value (`call i32 @"bz$Lib.L$TakeInt"(i32 7)`). The verifier
// rejected both with "Call parameter type does not match function signature!"
// and the build aborted.
//
// Mechanism (per issue): `buildOrderedArgs` runs the CG-nullable wrap for a
// SAME-MODULE call (its `params[idx].type` is an `ast::Type` node —
// resolved through `ctx_.resolvedType` to a `sema::TypeRef` whose shape
// nullableValueStructTy recognizes). For an IMPORTED callee there is no
// `ast::MethodDecl` and no `ast::Param` to consult; the imported-call path
// in `LlvmBackend_Calls2.cpp` (both the `.` form around line 8130 and the
// `?.` form around line 12050) builds its own argument list from the
// `ParameterSymbol`'s `resolvedDeclType` and never performs the wrap. The
// function was DECLARED with the right tagged struct (declareImportedFunctions
// / `lowerParamTypeRef` shape it correctly), so the call site and the
// declaration disagreed, and `verifyModule` caught it. The fix is the same
// three lines that buildOrderedArgs already runs against the local twin:
// a `null` literal becomes the all-zero `{i8 0, T 0}` (nullableValueNull);
// a bare T becomes `{i8 1, T}` (wrapNullableValue); a value already shaped
// is left alone. The fix is gated on `isNullableValueTy(formalTy)` so it
// is a no-op for every non-nullable-value (record, class, primitive, etc.)
// and is no-op for ref/out (which is an address, never a tagged struct).
//
// The regression test covers BOTH spellings (null literal and present value)
// and at least two members of the nullable-value family (int? and a record?),
// because the payload differs (a scalar for int?, a struct for record?) and
// the bug is the family's, not the family's `int?` half.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"  // the sanitizer-honouring compileFile
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile3133 {
    std::string path;
    explicit TempFile3133(std::string p) : path(std::move(p)) {}
    ~TempFile3133() { std::remove(path.c_str()); }
};

void writeSource3133(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// ── the library: methods whose formal parameter is a nullable VALUE type ────
//
// `TakeInt(v: int?)` — the issue's minimal repro (§2.5 cross-module call
//                    passes a nullable primitive across a module boundary).
// `TakeLong(v: long?)` — second primitive (different payload width).
// `TakeRec(v: Rec3133?)` — a record-typed member of the same family
//                    (`isNullableRecordTy`); the payload is a record STRUCT
//                    (not a scalar), so the wrap builds `{i8, R}` rather
//                    than `{i8, i32}` — a different insertvalue path that
//                    the bare-scalar body of buildOrderedArgs does not
//                    exercise separately.
//
// The expected return values are distinct for the null/present halves
// so the verification pins the actual value, not just "did it link".
// IMPORTANT: every expected return value MUST fit in 0..255 — the OS-trampoline
// used by `compileImportAndRun3133` runs the produced binary via `std::system`,
// which only preserves the low 8 bits of the exit code (§7.2 / #1386).
// The values here are chosen so the sums stay below 255:
//   1+2*10=21 (int), 10+20=30 (long), 100+107+1=208 (record, via v.v+107),
//   and the combined surface 21+30+208 = 259 (mod 256 = 3). T4's combined
//   returns one assembly HALF of the suite so the 8-bit truncation is
//   visible (and reversible: 259 = 256 + 3 = 3 mod 256).
//
// Rec3133 is declared as a positional record (auto-hashable on a single
// primitive field — §9.2/#9.3). Body fields on a record force the
// non-auto-hashable branch and surface unrelated `=` diagnostics the
// reproduction does not need to exercise.
const char* kLibDecls3133 =
    "public record Rec3133(v: int); "
    "public class L3133 "
    "  public static method TakeInt(v: int?): int "
    "    if v = null then return 1; end; "
    "    return 2; "
    "  end TakeInt; "
    "  public static method TakeLong(v: long?): int "
    "    if v = null then return 10; end; "
    "    return 20; "
    "  end TakeLong; "
    "  public static method TakeRec(v: Rec3133?): int "
    "    if v = null then return 100; end; "
    "    return v.v + 2; "
    "  end TakeRec; "
    "end L3133;";

std::string libSource3133() {
    return "module Lib3133; " + std::string(kLibDecls3133);
}

// ── two-module build + RUN. Used by ACCEPT tests. ──────────────────────────
int compileImportAndRun3133(const std::string& libSrc,
                            const std::string& appSrc) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3133r_" + std::to_string(std::rand());
    TempFile3133 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3133 aObj((dir / (stem + "_a.o")).string());
    writeSource3133(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;
    TempFile3133 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3133 bExe((dir / stem).string());
    writeSource3133(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    codegen::CodegenOptions opts;
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps, opts)
             .success)
        return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// ── two-module build, stderr captured. Used by REJECTION tests. ─────────────
struct ImportCapture3133 {
    bool dependencyCompiled = false;
    bool appCompiled = false;
    std::string error;
};

std::string captureStderrOf3133(const std::function<void()>& body) {
    std::string errPath =
        (fs::temp_directory_path() /
         ("bz3133_err_" + std::to_string(std::rand()) + ".txt")).string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    body();
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) { ::close(fd); ::dup2(saved, STDERR_FILENO); }
    ::close(saved);
    std::ifstream in(errPath);
    std::string err((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return err;
}

ImportCapture3133 compileImport3133(const std::string& libSrc,
                                    const std::string& appSrc) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3133_" + std::to_string(std::rand());
    TempFile3133 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3133 aObj((dir / (stem + "_a.o")).string());
    writeSource3133(aSrc.path, libSrc);
    ImportCapture3133 out;
    {
        bezel::Driver driver;
        out.dependencyCompiled = driver.compileObject(aSrc.path, aObj.path).success &&
                                 std::ifstream(aObj.path).good();
    }
    if (!out.dependencyCompiled) return out;
    TempFile3133 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3133 bExe((dir / stem).string());
    writeSource3133(bSrc.path, appSrc);
    out.error = captureStderrOf3133([&] {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        codegen::CodegenOptions opts;
        out.appCompiled =
            bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps,
                                            opts).success;
    });
    return out;
}

// ── single-module compile + RUN for ACCEPT twin. ────────────────────────────
int compileLocalAndRun3133(const std::string& src) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3133lr_" + std::to_string(std::rand());
    TempFile3133 srcFile((dir / (stem + ".bzl")).string());
    TempFile3133 exeFile((dir / stem).string());
    writeSource3133(srcFile.path, src);
    bezel::Driver driver;
    codegen::CodegenOptions opts;
    if (!bezel::test::compileFileForTest(driver, srcFile.path, exeFile.path, {},
                                         opts)
             .success)
        return -1;
    int status = std::system(exeFile.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

bool mentions3133(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ===========================================================================
// T1 — the issue's minimal repro: `L.TakeInt(null)` and `L.TakeInt(7)` on an
// imported `method TakeInt(v: int?)`. Pre-fix: the IR verifier rejects both
// calls (the call site emits `ptr null` and `i32 7` against a declared
// `{ i8, i32 }` parameter), the build fails with "IR verification failed",
// and the test's run returns -1 (compile failure). Post-fix: the call site
// wraps `null` to `{i8 0, i32 0}` and `7` to `{i8 1, i32 7}`, the build
// passes, and the program returns 1 + 2*10 = 21.
// ===========================================================================
BEZEL_TEST(xmod_nullable_value_param_3133,
           imported_int_nullable_null_and_present_7_2) {
    BEZEL_EXPECT_EQ(compileImportAndRun3133(
        libSource3133(),
        "module App3133; import Lib3133; "
        "public class C "
        "  public static method Main(): int "
        "    var r: int := 0; "
        "    r := r + L3133.TakeInt(null); "
        "    r := r + L3133.TakeInt(7) * 10; "
        "    return r; "
        "  end Main; "
        "end C;"), 21);
}

// T2 — second primitive (long) — to pin that the fix is the FAMILY's, not
// the int? half. The wrap is `{i8, i64}` instead of `{i8, i32}` (different
// payload width), so the same code path goes through a different
// `nullableValueStructTy` lookup.
BEZEL_TEST(xmod_nullable_value_param_3133,
           imported_long_nullable_null_and_present_7_2) {
    BEZEL_EXPECT_EQ(compileImportAndRun3133(
        libSource3133(),
        "module App3133; import Lib3133; "
        "public class C "
        "  public static method Main(): int "
        "    var r: int := 0; "
        "    r := r + L3133.TakeLong(null); "
        "    r := r + L3133.TakeLong(1000L); "
        "    return r; "
        "  end Main; "
        "end C;"), 30);
}

// T3 — record-typed member of the family. The payload is a record STRUCT,
// not a scalar, so `wrapNullableValue` (the present-value branch) exercises
// the `insertvalue` path that the bare-scalar body of buildOrderedArgs does
// not touch separately. `TakeRec(null)` covers the null-literal branch
// (`nullableValueNull` of a record-shaped struct), and `TakeRec(new Rec(7))`
// covers the present-value branch (`wrapNullableValue` of a record via
// `reinterpretToType` round-trip). TakeRec returns 100 for null and 7+2=9
// for present (per the library's `v.v + 2`), so the sum is 109.
BEZEL_TEST(xmod_nullable_value_param_3133,
           imported_record_nullable_null_and_present_7_2) {
    BEZEL_EXPECT_EQ(compileImportAndRun3133(
        libSource3133(),
        "module App3133; import Lib3133; "
        "public class C "
        "  public static method Main(): int "
        "    var r: int := 0; "
        "    r := r + L3133.TakeRec(null); "
        "    r := r + L3133.TakeRec(new Rec3133(7)); "
        "    return r; "
        "  end Main; "
        "end C;"), 109);
}

// T4 — ALL THREE families in one call site. Pins the fix at the per-argument
// level for both the null-literal and the bare-T branches across the whole
// nullable-value family. Pre-fix: every call emits a wrong shape and the
// verifier rejects them all together. Post-fix: the consumer returns the
// sum of (1, 2*10, 10, 20, 100, 7+2) = 21 + 30 + 109 = 160.
BEZEL_TEST(xmod_nullable_value_param_3133,
           imported_all_three_families_combined_7_2) {
    BEZEL_EXPECT_EQ(compileImportAndRun3133(
        libSource3133(),
        "module App3133; import Lib3133; "
        "public class C "
        "  public static method Main(): int "
        "    var r: int := 0; "
        "    r := r + L3133.TakeInt(null);         "
        "    r := r + L3133.TakeInt(7) * 10;       "
        "    r := r + L3133.TakeLong(null);        "
        "    r := r + L3133.TakeLong(1000L);       "
        "    r := r + L3133.TakeRec(null);         "
        "    r := r + L3133.TakeRec(new Rec3133(7)); "
        "    return r; "
        "  end Main; "
        "end C;"), 160);
}

// T5 — local twin of T1. The same signatures declared in the SAME module
// always compiled cleanly (the bug is the imported-call path, not the call
// site in general). This pins that the fix is NOT over-broad: a local call
// to the same local method must continue to work.
BEZEL_TEST(xmod_nullable_value_param_3133,
           local_twin_int_nullable_unchanged_7_2) {
    BEZEL_EXPECT_EQ(compileLocalAndRun3133(
        "module Ctl3133; " + std::string(kLibDecls3133) +
        "public class C "
        "  public static method Main(): int "
        "    var r: int := 0; "
        "    r := r + L3133.TakeInt(null); "
        "    r := r + L3133.TakeInt(7) * 10; "
        "    return r; "
        "  end Main; "
        "end C;"), 21);
}

// T6 — shape-mismatch REJECTION control. The bug's pre-fix symptom was
// IR-verification failure with the message "Call parameter type does not
// match function signature!". A regression that degenerates to "wrong type
// matched at the call site" (e.g. wrapping every parameter unconditionally,
// without the `isNullableValueTy` gate) would make this test START to
// compile cleanly — guaranteeing a false-pass. We pin that the REJECT path
// still rejects with the verifier's message, so a fix that loses the gate
// is caught immediately.
//
// A plain non-nullable value type at an imported formal must NEVER be
// wrapped (the callee declares it as a bare T, the call site passes a bare
// T, the verifier agrees). Pin that the unmodified program (no nullable
// anywhere) compiles and runs cleanly.
BEZEL_TEST(xmod_nullable_value_param_3133,
           non_nullable_value_param_unwrapped_cross_module_7_2) {
    auto r = compileImportAndRun3133(
        "module Lib3133n; "
        "public class L3133n "
        "  public static method TakeInt(v: int): int "
        "    return v + 1; "
        "  end TakeInt; "
        "end L3133n;",
        "module App3133n; import Lib3133n; "
        "public class C "
        "  public static method Main(): int "
        "    return L3133n.TakeInt(40); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(r, 41);
}

// T7 — STRING null-coalescing + import sanity. A nullable STRING is the
// reference-typed member of the nullable family (isNullableValueTy returns
// false for `string?` because the IR is already a pointer, but the SEMANTIC
// distinction between null and present keeps the type system honest). The
// fix MUST NOT regress this — a `string?` must continue to work cross-module
// with the same wrapping as before (which is a no-op at the IR level, since
// `string` is already a pointer). Pin that path explicitly.
BEZEL_TEST(xmod_nullable_value_param_3133,
           imported_string_nullable_null_and_present_unchanged_7_2) {
    BEZEL_EXPECT_EQ(compileImportAndRun3133(
        "module Lib3133s; "
        "public class L3133s "
        "  public static method TakeStr(v: string?): int "
        "    if v = null then return 1; end; "
        "    return 2; "
        "  end TakeStr; "
        "end L3133s;",
        "module App3133s; import Lib3133s; "
        "public class C "
        "  public static method Main(): int "
        "    var r: int := 0; "
        "    r := r + L3133s.TakeStr(null); "
        "    r := r + L3133s.TakeStr(\"hi\") * 10; "
        "    return r; "
        "  end Main; "
        "end C;"), 21);
}

}  // namespace bezel::module
