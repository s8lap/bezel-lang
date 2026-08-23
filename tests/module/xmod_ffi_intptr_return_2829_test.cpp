// fix #2829: an IMPORTED method whose declared return type is `IntPtr` (spec
// §32.7 / §2.5, stdlib §10.0, v0.278) crashed the compiler at the caller with
// two distinct LLVM assertions, depending on what the caller did with the
// value:
//   * `L.Zed() = IntPtr.Zero`   → ICmpInst::AssertOK  ("Both operands to ICmp
//     instruction are not of the same type!"). The imported return lowered to
//     `ptr` (heap-pointer rep) while `IntPtr.Zero` lowered to `i64`.
//   * `long(L.Zed())`           → cast<IntegerType>(Value*) on the `ptr` form.
// Both fail in the `importedName` TypeRef shape — the ModuleLoader reconstructs
// the return TypeRef with no `astNode` and no `declaration`, leaving only
// `importedName` ("IntPtr"). The importer-side `lowerTypeRef` branch
// (src/codegen/LlvmBackend_Layout.cpp ~1076) lumped `IntPtr`/`UIntPtr` with
// `CString`/`WString`/`RawBytes` and returned `ptrTy()` for all five; the
// astNode-side mirror (~944) routes IntPtr/UIntPtr to `i64()` (per §32.7:
// pointer-sized integer; on the host target that is 64-bit). stdlib §10.0 is
// explicit: "IntPtr and UIntPtr are compiler-intrinsic value types representing
// pointer-sized integers" — they are NOT reference types.
//
// WHY A CROSS-MODULE FILE. The same row reproduced locally (no `import`) before
// the fix, so the bug has an in-module face too, but the IMPORTER-side return
// is the original repro in #2829 and the only place where the importedName
// fallback is exercised. Three rows verify both ABORT paths AND a non-zero
// value path through the importer:
//   A — comparison (`L.Zed() = IntPtr.Zero`), the exact #2829 ABORT.
//   B — long-cast (`long(L.Zed())`), the sibling ABORT.
//   C — non-zero value (`L.Zed() = IntPtr(42)`), the type-mismatch row that
//       a zero-only test would have silently passed. With a wrong `i32`
//       declare (an intermediate-fix state that returned IntPtr/UIntPtr to
//       the heap-pointer disjunction's complement but did NOT add them to
//       the value-types branch), the caller's `IntPtr(42)` is sign- or
//       zero-extended from 32 to 64 bits in the calling convention and the
//       equality test against `IntPtr(42)` (a 64-bit constant) fails.
//       This row is the regression test for the "IntPtr/UIntPtr fall through
//       to i32 default" defect that the v1 of this fix introduced.
// All three rows return a Bezel integer exit code (the answer the programmer
// would observe at runtime once the compiler stops aborting), so a no-ABORT
// but wrong-value bug also fails the row.
//
// FAIL-WITHOUT, MEASURED: with `src/codegen/LlvmBackend_Layout.cpp` reverted to
// its pre-fix state, rows A and B ABORT the compiler (each at the named
// assertion in the row's comment) and `r.ran` is false; row C fails because
// the `i32` vs `i64` declared type mismatch trips IR verification.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"


namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile2829 {
    std::string path;
    explicit TempFile2829(std::string p) : path(std::move(p)) {}
    ~TempFile2829() { std::remove(path.c_str()); }
};

void writeSource2829(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile `libSrc` to an object, compile `appSrc` against it, link and run.
// Returns the program's exit code, -1 on compile/link failure, -2 if the
// toolchain is unavailable (SKIP, not a failure). The library body here is
// pure Bezel (returns `IntPtr.Zero`), so no foreign extern / libc / linker
// dance — both the library compile and the app compile are ordinary
// `bezelc` invocations. LeakSanitizer is deliberately NOT armed: a `using
// var` bound over an imported value's no-scope-end-dispose gap is unrelated
// to #2829 and the rows here do not exercise that gap. The VALUE assertion
// (rc == 24 / 26) is what pins the fix; the assertion is what the program
// would observe at runtime once the compiler stops aborting.
int compileImportRun2829(const std::string& libSrc, const std::string& appSrc) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2829_");

    TempFile2829 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2829 aObj((dir / (stem + "_a.o")).string());
    writeSource2829(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2829 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2829 bExe((dir / stem).string());
    writeSource2829(bSrc.path, appSrc);

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
// A — #2829's exact repro: an imported `Zed(): IntPtr` consumed by an `=` test
// against `IntPtr.Zero`. Pre-fix the importer-side return TypeRef lowered to
// `ptr` (heap-pointer rep) while `IntPtr.Zero` lowered to `i64`, so the
// generated `icmp` had mismatched operand types and tripped
// `ICmpInst::AssertOK`. Post-fix the imported return is `i64` (the §32.7
// pointer-sized integer), matching the local path.
// per spec §32.7 / stdlib §10.0, v0.278
// ===========================================================================
BEZEL_TEST(xmod_ffi_intptr_return_2829, imported_method_returning_intptr_compare_spec_32_7) {
    const std::string lib =
        "module FfiIntPtrLib2829A; "
        "public class L "
        "  public static method Zed(): IntPtr "
        "    return IntPtr.Zero; "
        "  end Zed; "
        "end L;";
    const std::string app =
        "module FfiIntPtrApp2829A; import FfiIntPtrLib2829A; "
        "class C "
        "  public static method Main(): int "
        "    if L.Zed() = IntPtr.Zero then return 24; end; "
        "    return 25; "
        "  end Main; "
        "end C;";
    int rc = compileImportRun2829(lib, app);
    if (rc == -2) return;  // toolchain unavailable — skip, not a failure
    BEZEL_EXPECT_EQ(rc, 24);
}

// ===========================================================================
// B — the sibling ABORT: `long(L.Zed())` over the same imported `Zed(): IntPtr`
// return. Pre-fix the generated `ptrtoint` was applied to a `ptr` operand and
// tripped `cast<IntegerType>(Value*)`. Post-fix the imported return is `i64`,
// matching the local-path return and the `i64` operand expected by `long`.
// per spec §32.7 / §5.2 (explicit-cast to `long`), v0.278
// ===========================================================================
BEZEL_TEST(xmod_ffi_intptr_return_2829, imported_method_returning_intptr_long_cast_spec_32_7) {
    const std::string lib =
        "module FfiIntPtrLib2829B; "
        "public class L "
        "  public static method Zed(): IntPtr "
        "    return IntPtr.Zero; "
        "  end Zed; "
        "end L;";
    const std::string app =
        "module FfiIntPtrApp2829B; import FfiIntPtrLib2829B; "
        "class C "
        "  public static method Main(): int "
        "    if long(L.Zed()) = 0 then return 26; end; "
        "    return 27; "
        "  end Main; "
        "end C;";
    int rc = compileImportRun2829(lib, app);
    if (rc == -2) return;  // toolchain unavailable — skip, not a failure
    BEZEL_EXPECT_EQ(rc, 26);
}

// ===========================================================================
// C — the non-zero-value row, with a value that exceeds 32 bits so a
// truncated i32 declare is OBSERVABLY wrong (not merely "happens to match").
// The body returns `IntPtr(5000000000)` (via the §5.2 explicit-cast / §32.7
// forging conversion from a `long` constant). The caller compares
// `L.Zed() = IntPtr(5000000000)`. With the imported declare at i64 (the
// fix), the value round-trips; with the imported declare at i32 (the
// half-fix that returned IntPtr/UIntPtr to the heap-pointer disjunction's
// complement but did NOT add them to the value-types branch), the i64
// constant `IntPtr(5000000000)` is stored at full 64 bits on the caller
// side, the i32 return sign-extends the truncated low 32 bits to 64 bits
// in the calling convention (verified: `movslq %ebx,%rax`), and the
// equality test fails — `rc == 31` instead of 30. A small value like 42
// (rows A and B test equality against IntPtr.Zero) would have silently
// passed the half-fix; this row deliberately picks a value whose low 32
// bits do NOT equal the original, so a wrong type is not compensated by
// a coincidental match.
// per spec §32.7 / §5.2 (explicit-cast forging), v0.278
// ===========================================================================
BEZEL_TEST(xmod_ffi_intptr_return_2829, imported_method_returning_intptr_nonzero_value_spec_32_7) {
    const std::string lib =
        "module FfiIntPtrLib2829C; "
        "public class L "
        "  public static method Zed(): IntPtr "
        "    var big: long := 5000000000L; "
        "    return IntPtr(big); "
        "  end Zed; "
        "end L;";
    const std::string app =
        "module FfiIntPtrApp2829C; import FfiIntPtrLib2829C; "
        "class C "
        "  public static method Main(): int "
        "    var big: long := 5000000000L; "
        "    if L.Zed() = IntPtr(big) then return 30; end; "
        "    return 31; "
        "  end Main; "
        "end C;";
    int rc = compileImportRun2829(lib, app);
    if (rc == -2) return;  // toolchain unavailable — skip, not a failure
    BEZEL_EXPECT_EQ(rc, 30);
}

}  // namespace bezel::module
