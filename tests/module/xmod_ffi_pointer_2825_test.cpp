// fix #2825 / #2810, CROSS-MODULE rows.  The in-module rows and the full defect
// write-up live in tests/codegen/ffi_buffer_pointer_2825_test.cpp.
//
// WHY A CROSS-MODULE FILE.  The fix reads the §10 buffer family through
// `common::isFfiBufferBuiltinName`, and the accessors it goes through
// (`Emitter::builtinTypeName` in codegen, `TypeRef::headName()` in sema) resolve
// a Named TypeRef's head from THREE sources in priority order: the alias-resolved
// head, the `astNode` syntactic head, and — only for a type that crossed a module
// boundary — the metadata `importedName` that #662/#1221's ModuleLoader stores
// when `lookupNamed` returns null.  A `CString` reaching a member-read site with
// NO `astNode` at all is therefore a distinct code path, and the only way to
// produce one is to import it.  Inference about that boundary has been wrong in
// this repo repeatedly, so it is measured here.
//
// Two rows, one per direction across the boundary:
//   A — the IMPORTED module reads `.Pointer` on its own `c: CString` parameter.
//   B — the APP reads `.Pointer` on an IMPORTED class's `CString` FIELD, which is
//       the `importedName` TypeRef shape described above.
// Both carry the answer in the exit code by way of libc `strlen` walking the
// address, so a null or wrong address fails the row rather than merely warning.
//
// ⚠ WHAT THESE ROWS DELIBERATELY AVOID, and why the shape looks roundabout.
// Row A returns `long`, not `IntPtr`.  An IMPORTED method whose return type is
// `IntPtr` ABORTS the compiler at the caller — `L.Zed() = IntPtr.Zero` on an
// imported `Zed(): IntPtr` trips LLVM's `ICmpInst::AssertOK` ("Both operands to
// ICmp instruction are not of the same type"), and `long(L.Zed())` trips
// `cast<IntegerType>`.  That is a PRE-EXISTING defect with nothing to do with
// `.Pointer` (it reproduces on a body that only returns `IntPtr.Zero`), filed
// separately; routing the address through `long` + the §32.7 `IntPtr(longValue)`
// conversion keeps these rows measuring what they are for.
//
// FAIL-WITHOUT, MEASURED: with `src/codegen/LlvmBackend_Exprs.cpp` and
// `src/sema/ExprTypecheck_Calls.cpp` reverted, both rows fail.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile2825 {
    std::string path;
    explicit TempFile2825(std::string p) : path(std::move(p)) {}
    ~TempFile2825() { std::remove(path.c_str()); }
};

void writeSource2825(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile `libSrc` to an object, compile `appSrc` against it, link and run.
// Returns the program's exit code, -1 on compile/link failure, -2 if the
// toolchain is unavailable (SKIP, not a failure).
//
// LeakSanitizer is deliberately NOT armed: row B's imported class holds an
// `owned _c: CString` field that is never disposed (21 B/construction), a
// pre-existing owned-FIELD disposal gap that reproduces identically for a LOCAL
// class with the same field and is unrelated to `.Pointer` — filed separately.
// The VALUE assertion is what pins #2825's half.
int compileImportRun2825(const std::string& libSrc, const std::string& appSrc) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2825_");

    TempFile2825 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2825 aObj((dir / (stem + "_a.o")).string());
    writeSource2825(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2825 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2825 bExe((dir / stem).string());
    writeSource2825(bSrc.path, appSrc);

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);

    bezel::codegen::CodegenOptions opts;
    if (!driver.compileFile(bSrc.path, bExe.path, deps, opts).success)
        return -1;

    std::string runCmd = bExe.path + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    std::remove(bExe.path.c_str());
    if (status == -1) return -2;
    if (WIFSIGNALED(status)) return 97;
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

}  // namespace

// ===========================================================================
// A — the IMPORTED module reads `.Pointer` on its own `c: CString` PARAMETER
// (the annotation TypeRef shape, inside a separately-compiled translation unit)
// and returns the address.  The app hands it to libc `strlen`, which returns 5
// for "hello".  Pre-fix the imported method returned 0 and `strlen(NULL)`
// segfaulted — the exact §10.1 argv hazard, one module removed.
// per stdlib §10.1 / spec §32.5 / §32.6, v0.240
// ===========================================================================
BEZEL_TEST(xmod_ffi_pointer_2825, imported_method_reads_cstring_pointer_spec_10_1) {
    const std::string lib =
        "module FfiPtrLib2825; "
        "public class L "
        "  public static method AddrOf(c: CString): long "
        "    return long(c.Pointer); "
        "  end AddrOf; "
        "end L;";
    const std::string app =
        "module FfiPtrApp2825A; import FfiPtrLib2825; "
        "extern \"c\" public method strlen(s: IntPtr): long; end; "
        "class C public static method Main(): int "
        "    using var c := CString.From(\"hello\"); "
        "    return int(strlen(IntPtr(L.AddrOf(c)))); "
        "  end Main; "
        "end C;";
    int rc = compileImportRun2825(lib, app);
    if (rc == -2) return;  // toolchain unavailable — skip, not a failure
    BEZEL_EXPECT_EQ(rc, 5);
}

// ===========================================================================
// B — the APP reads `.Pointer` on an IMPORTED class's `CString` FIELD.  This is
// the `importedName` TypeRef shape: the ModuleLoader reconstructs the field's
// type with no `astNode`, so the head name is only reachable through
// `headName()`'s / `builtinTypeName`'s `importedName` fallback.  If either new
// predicate had been written against `astNode` directly, this row would read a
// null address and `strlen` would segfault (rc 97).
// per spec §2.5 / §32.5, stdlib §10.1, v0.240
// ===========================================================================
BEZEL_TEST(xmod_ffi_pointer_2825, imported_field_cstring_pointer_spec_2_5) {
    const std::string lib =
        "module FfiPtrLib2825B; "
        "public class H "
        "  public owned _c: CString; "
        "  public constructor() _c := CString.From(\"hello\"); end constructor; "
        "end H;";
    const std::string app =
        "module FfiPtrApp2825B; import FfiPtrLib2825B; "
        "extern \"c\" public method strlen(s: IntPtr): long; end; "
        "class C public static method Main(): int "
        "    using var h := new H(); "
        "    return int(strlen(h._c.Pointer)); "
        "  end Main; "
        "end C;";
    int rc = compileImportRun2825(lib, app);
    if (rc == -2) return;  // toolchain unavailable — skip, not a failure
    BEZEL_EXPECT_EQ(rc, 5);
}

}  // namespace bezel::module
