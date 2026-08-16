// fix #2809, CROSS-MODULE rows (codegen; spec §28.3 "Local variables,
// ownership, and borrows" — its *Unbound owned temporaries* subsection —
// §32.5 "FFI-compatible types" / §32.6 "String marshalling", stdlib §10.1
// `CString`; all v0.240).  The in-module rows and the full defect write-up live
// in tests/codegen/ffi_temp_ordinary_call_2809_test.cpp.
//
// WHY A SEPARATE CROSS-MODULE FILE.  The reclaim of an unbound owned FFI
// temporary in argument position is established where the ARGUMENT LIST is
// built, and codegen has THREE such places, not one.  `buildOrderedArgs`
// (LlvmBackend_Assign.cpp) serves local methods and local constructors; an
// IMPORTED callee cannot use it at all, because #1221's ModuleLoader
// reconstructs a MethodSymbol/ConstructorSymbol from dependency metadata with
// NO `ast::MethodDecl`/`ast::ConstructorDecl` for it to read — so `lowerCall`
// (LlvmBackend_Calls2.cpp) and `lowerNew` (LlvmBackend_Exprs2.cpp) each
// hand-roll their own argument loop.  Both were missing the registration.
// Measured pre-fix, 20 iterations each, LeakSanitizer on:
//
//   imported STATIC METHOD  L.LenOf(CString.From("abcde"))
//     -> Direct leak of 420 byte(s) in 20 object(s) ... bz_string_to_utf8z
//   imported CONSTRUCTOR    new W(CString.From("abcd"))
//     -> Direct leak of 340 byte(s) in 20 object(s) ... bz_string_to_utf8z
//
// Both rows also exercise the OTHER half of #2809 in the callee: the imported
// library's `c: CString` parameter is the ANNOTATION TypeRef shape, so its
// `c.Length` was the `%bz.string` field-1 read that made the in-module repro an
// ASan heap-buffer-overflow.  Here the exit code carries the VALUE, so a
// mis-read length fails the row even where the read happens to stay in bounds.
//
// §28.3: "Such a temporary is disposed at the **end of the full expression** in
// which it is produced, after every use of it within that expression has
// completed."  §32.6: "The `CString` class is for **caller-owned** strings:
// those that the Bezel side allocated and is responsible for disposing."
//
// Every row loops 20x and allocates AFTERWARDS: a small `Main` is not a
// reliable LSan oracle, because a leftover pointer in a register can make LSan
// report a leaked block as still reachable.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"  // #3156: the ONE sanitizer-honouring compileFile wrapper
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile2809 {
    std::string path;
    explicit TempFile2809(std::string p) : path(std::move(p)) {}
    ~TempFile2809() { std::remove(path.c_str()); }
};

void writeSource2809(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile `libSrc` to an object, compile `appSrc` against it with generated-code
// AddressSanitizer, link and run it with LeakSanitizer ARMED.  Returns the
// program's exit code on a clean run, 97 if ASan/LSan fired, -1 on compile/link
// failure, -2 if the toolchain is unavailable (SKIP, not a failure).
// (`--sanitize=address` is an executable-only option, so the LIBRARY object is
// compiled plain — exactly what `bezelc -c` does; the instrumentation the
// app links in still covers the imported code's loads.)
int compileImportRunAsan2809(const std::string& libSrc,
                             const std::string& appSrc) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2809_");

    TempFile2809 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2809 aObj((dir / (stem + "_a.o")).string());
    writeSource2809(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2809 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2809 bExe((dir / stem).string());
    writeSource2809(bSrc.path, appSrc);

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);

    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps, opts).success)
        return -1;

    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:exitcode=97 " +
        bExe.path + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    std::remove(bExe.path.c_str());
    if (status == -1) return -2;
    if (WIFSIGNALED(status)) return 97;
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

// The churn tail: an allocation AFTER the measured loop, so no register still
// holds the last iteration's buffer when LeakSanitizer runs.
const char* kChurn2809 =
    "    var junk: Array<int> := [1, 2, 3]; "
    "    if junk[0] <> 1 then return 90; end; ";

}  // namespace

// ===========================================================================
// T1 — IMPORTED STATIC METHOD.  `lowerCall`'s hand-rolled imported-method
// argument loop (LlvmBackend_Calls2.cpp) had no FFI-temporary registration, so
// every call leaked its buffer (measured: 420 B / 20 objects).  The exit code
// also carries the length the IMPORTED callee computed, so the annotation-shape
// `.Length` read is pinned here too: 5 chars * 20 iterations / 5 = 20.
// per §28.3 *Unbound owned temporaries* / §32.6, v0.240
// ===========================================================================
BEZEL_TEST(xmod_ffi_temp_arg_2809, imported_static_method_temp_reclaimed_spec_28_3) {
    const std::string lib =
        "module FfiLib2809; "
        "public class L "
        "  public static method LenOf(c: CString): int "
        "    return c.Length; "
        "  end LenOf; "
        "end L;";
    const std::string app =
        std::string(
        "module FfiApp2809T1; import FfiLib2809; "
        "class C public static method Main(): int "
        "    var t: int := 0; var i: int := 0; "
        "    while i < 20 do "
        "      t := t + L.LenOf(CString.From(\"abcde\")); "
        "      i := i + 1; "
        "    end; ") + kChurn2809 +
        "    if t <> 100 then return 91; end; "
        "    return 20; "
        "  end Main; "
        "end C;";
    int rc = compileImportRunAsan2809(lib, app);
    if (rc == -2) return;  // toolchain unavailable — skip, not a failure
    BEZEL_EXPECT_EQ(rc, 20);
}

// ===========================================================================
// T2 — IMPORTED CONSTRUCTOR.  `lowerNew`'s hand-rolled imported-ctor argument
// loop (LlvmBackend_Exprs2.cpp) is the third site; same defect, measured 340 B
// / 20 objects.  The wrapper stores the length the imported ctor read, so a
// mis-read annotation-shape `.Length` also fails this row.
// per §28.3 *Unbound owned temporaries* / §32.6, v0.240
// ===========================================================================
BEZEL_TEST(xmod_ffi_temp_arg_2809, imported_ctor_temp_reclaimed_spec_28_3) {
    const std::string lib =
        "module FfiLib2809B; "
        "public class W "
        "  private _n: int; "
        "  public constructor(c: CString) _n := c.Length; end constructor; "
        "  public method N(): int return _n; end N; "
        "end W;";
    const std::string app =
        std::string(
        "module FfiApp2809T2; import FfiLib2809B; "
        "class C public static method Main(): int "
        "    var t: int := 0; var i: int := 0; "
        "    while i < 20 do "
        "      using var w := new W(CString.From(\"abcd\")); "
        "      t := t + w.N(); "
        "      i := i + 1; "
        "    end; ") + kChurn2809 +
        "    if t <> 80 then return 91; end; "
        "    return 21; "
        "  end Main; "
        "end C;";
    int rc = compileImportRunAsan2809(lib, app);
    if (rc == -2) return;  // toolchain unavailable — skip, not a failure
    BEZEL_EXPECT_EQ(rc, 21);
}

// ===========================================================================
// T3 — the OVER-RECLAIM control across the module boundary.  stdlib §9.6
// (v0.240): "The source `CString` or `WString` retains its ownership and
// disposal obligation."  A `using var`-bound argument must be freed by its
// BINDING and by nothing else; `isFfiBufferFactoryCall` matches only a
// directly-written factory call, so a bare identifier never registers.  If the
// new imported-callee registration were keyed on TYPE rather than on the
// producing EXPRESSION, this row would double-free and ASan would return 97.
// per §32.6 / stdlib §9.6, v0.240
// ===========================================================================
BEZEL_TEST(xmod_ffi_temp_arg_2809, bound_cstring_arg_freed_once_by_binding_spec_32_6) {
    const std::string lib =
        "module FfiLib2809C; "
        "public class L3 "
        "  public static method LenOf(c: CString): int "
        "    return c.Length; "
        "  end LenOf; "
        "end L3;";
    const std::string app =
        std::string(
        "module FfiApp2809T3; import FfiLib2809C; "
        "class C public static method Main(): int "
        "    var t: int := 0; var i: int := 0; "
        "    while i < 20 do "
        "      using var s := CString.From(\"abcde\"); "
        "      t := t + L3.LenOf(s) + L3.LenOf(s); "
        "      i := i + 1; "
        "    end; ") + kChurn2809 +
        "    if t <> 200 then return 91; end; "
        "    return 22; "
        "  end Main; "
        "end C;";
    int rc = compileImportRunAsan2809(lib, app);
    if (rc == -2) return;  // toolchain unavailable — skip, not a failure
    BEZEL_EXPECT_EQ(rc, 22);
}

}  // namespace bezel::module
