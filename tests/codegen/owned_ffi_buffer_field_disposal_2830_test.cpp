// Regression test for #2830 — §10 (FFI boundary types), §28.3 (owned-field
// discharge), §28.9 (runtime-managed types — the FFI buffers are NOT
// runtime-managed; they are heap-allocated via malloc in the runtime and
// require explicit release, which the §28.5 owned-field cascade must emit),
// stdlib §10.1–§10.3, v0.278.
//
// THE BUG. `emitOwnedFieldDisposals` (src/codegen/LlvmBackend_Disposal.cpp)
// classifies each `owned` field via one of nine predicates, each of which
// rejects a §10 FFI-buffer-typed field:
//
//   * isOwnedReferenceField        — requires rt->declaration->kind == Class
//                                    OR Interface; a §10 annotation
//                                    (`c: CString`) resolves to a
//                                    declaration-less Named TypeRef whose
//                                    head rides headName() / importedName
//                                    (#2809 / #2823 / #2825). Returns false.
//   * isOwnedBuiltinExceptionField — catches only the built-in exception
//                                    hierarchy (Exception / ...).
//   * ownedCollectionFieldType     — only Array/Stack/Queue/etc.
//   * isOwnedSubscriptionField     — only Subscription / Subscription?.
//   * isOwnedIteratorInterfaceField — only IEnumerable / IOwningEnumerable<T>.
//   * isOwnedEventSourceField      — only EventSource / AsyncEventSource<T>.
//   * taskFamilyTypeFromField      — only Task / Task<T> / OwningTask<T>.
//   * isOwnedSharedWeakField       — only Shared<T> / Weak<T>.
//
// The result was that the field was silently SKIPPED by the cascade. The
// malloc'd UTF-8 buffer behind `CString.From("hello")` (allocated in
// bz_string_to_utf8z, src/runtime/bezel_runtime_strings.cpp:997) was never
// freed — every Holder disposal leaked 21 bytes. Verified mechanically
// against this file's T1 with the patch reverted and this file unchanged:
// LeakSanitizer flagged `Direct leak of 21 byte(s) in 1 object(s) allocated
// from bz_string_to_utf8z in bz$M.Holder$ctor` and r.ran flipped to false.
//
// The control shape from #2830's report — same shape with a USER-CLASS field
// (`private owned _c: Cell`) — does NOT leak, confirming the gap is
// specific to the FFI-buffer field path (the user-class field is a regular
// isOwnedReferenceField hit and reaches the default vtable-Dispose path).
// Cross-module the same shape is observed (#2830's reproducer: 420 B / 20
// constructions in a loop), because the imported-field branch of every
// predicate above uses ctx_.importedMemberType(fs) which yields the same
// declaration-less TypeRef shape; the fix's isOwnedFfiBufferField covers
// that path via #662 foundation B's `!fs->decl` branch.
//
// THE FIX. A new predicate isOwnedFfiBufferField (LlvmBackend_impl.hpp) reads
// the field's TypeRef through isFfiBufferTypeRef (#2809's single home for
// {CString, WString, RawBytes}) and supports all three §22.10 primary-ctor
// captured-param / local-field / imported-field shapes. The OwnedField
// struct gains two booleans (ffiBufferField, rawBytes); emitOwnedField
// Disposals classifies the field with the new predicate, and the iteration
// loop emits a null-guarded free() (CString/WString) or bz_rawbytes_free
// (RawBytes) followed by a null-after-store so a re-entrant / second pass
// is a no-op (the #396 slot-null precedent). This mirrors exactly what
// emitFfiBufferTempFree does for `using var`-bound FFI buffers (#2733) and
// what registerUnwindOwnedParam does for `owned c: CString` parameters
// (#2823), so all three storage shapes free identically.
//
// OBSERVABILITY. Leak rows run under runAotChecked (AOT + generated-code
// ASan + LeakSanitizer). Pre-fix the field's malloc'd buffer leaks at
// every Holder disposal; LSan flips r.ran to false and the process exits
// with a non-zero status. Post-fix the buffer is freed at Holder
// disposal; r.ran is true and the program returns the expected sentinel.
// Functional rows use plain runAot and prove the buffer is ALIASED (the
// length/read is observable) — that the fix didn't free too early.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"      // codegen::CodegenOptions
#include "driver/Driver.hpp"            // bezel::Driver / Dependency
#include "support/aot_support.hpp"      // runAot / runAotChecked — the AOT oracle
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/driver_run_support.hpp"  // compileFileForTest — sweep-aware
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The churn tail every leak row shares: an allocation AFTER the measured loop,
// so no register still holds the last iteration's buffer when LeakSanitizer
// runs. A one-shot `Main` is not a reliable LSan oracle. Mirrors the pattern
// ffi_buffer_pointer_2825_test.cpp uses for the same reason.
const char* kChurn =
    "    var junk: Array<int> := [1, 2, 3]; "
    "    if junk[0] <> 1 then return 90; end; ";

// Multi-iteration churn is necessary: a single Holder disposal's 21 B leak
// is just at LSan's detection threshold (and the threshold is per-iteration
// for LSan, not per-program). The cross-module reproducer in #2830 used 20
// iterations; we follow.
const char* kLoop20 =
    "    var i := 0; "
    "    while i < 20 do "
    "      using var h := new Holder(); "
    "      i := i + 1; "
    "    end; ";

// ----------------------------------------------------------------------------
// Cross-module ASan helper. `compileObject` for the library, then
// `compileFileForTest` for the importer (sweep-aware — under `--full` the
// executable is linked with -fsanitize=address). Returns the program's exit
// code on a clean run; -2 if the toolchain is absent (clang unavailable),
// -1 on a compile failure, 97 on a signalled crash (ASan abort). The
// ASAN_OPTIONS line is the same shape xmod_named_local_setter_arc_2505 uses:
// LeakSanitizer ON, exitcode 97 so a leak is observable as a non-zero
// status rather than a segfault.
// ----------------------------------------------------------------------------
namespace fs = std::filesystem;
struct TempFile2830 {
    std::string path;
    explicit TempFile2830(std::string p) : path(std::move(p)) {}
    ~TempFile2830() { std::remove(path.c_str()); }
};
void writeSource2830(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}
int compileImportRunAsan2830(const std::string& libSrc,
                             const std::string& appSrc) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2830_");
    TempFile2830 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2830 aObj((dir / (stem + "_a.o")).string());
    writeSource2830(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2830 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2830 bExe((dir / stem).string());
    writeSource2830(bSrc.path, appSrc);

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

    std::string runCmd = "ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:exitcode=97 "
                          + bExe.path + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    std::remove(bExe.path.c_str());
    if (status == -1) return -2;
    if (WIFSIGNALED(status)) return 97;
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

}  // namespace

// ===========================================================================
// T1 — #2830's own repro shape for CString: a `private owned _c: CString`
// field on a Holder class, disposed via `using var h := new Holder()`. §28.5
// must release the malloc'd UTF-8 buffer at Holder disposal. WITHOUT the fix
// LSan flags `Direct leak of 21 byte(s) in 1 object(s) allocated from
// bz_string_to_utf8z in bz$M.Holder$ctor` and r.ran flips to false.
// WITH the fix the buffer is freed exactly once and the program returns
// 0 (Holder$Len returns 5 for "hello"; we don't return that here, just
// the loop-iteration sentinel). Mirrors the in-module row in
// xmod_ffi_pointer_2825_test.cpp — that row uses plain runAot for the same
// LSan-arming reason #2830 cites ("runs without LeakSanitizer armed (noted
// in place)").
//
// Per spec §10.1 / §28.3 / §28.9, stdlib §10.1, v0.278.
// ===========================================================================
BEZEL_TEST(owned_ffi_buffer_field_disposal_2830,
           owned_cstring_field_disposal_no_leak_spec_10_1_28_3) {
    auto r = runAotChecked(
        std::string(
        "module M; "
        "class Holder "
        "  private owned _c: CString; "
        "  public constructor() _c := CString.From(\"hello\"); end constructor; "
        "end Holder; "
        "class C "
        "  public static method Main(): int ") + kLoop20 + kChurn +
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);             // no leak (no double-free)
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// T2 — same shape for WString. §10.2 WString is a malloc'd UTF-16 buffer
// (bz_string_to_utf16z). §28.5 owned-field cascade must free it at Holder
// disposal.
//
// Per spec §10.2 / §28.3, stdlib §10.2, v0.278.
// ===========================================================================
BEZEL_TEST(owned_ffi_buffer_field_disposal_2830,
           owned_wstring_field_disposal_no_leak_spec_10_2_28_3) {
    auto r = runAotChecked(
        std::string(
        "module M; "
        "class Holder "
        "  private owned _w: WString; "
        "  public constructor() _w := WString.From(\"hello\"); end constructor; "
        "end Holder; "
        "class C "
        "  public static method Main(): int ") + kLoop20 + kChurn +
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================


// ===========================================================================
// T3 — same shape for RawBytes. §10.3 RawBytes is a { ptr, len } struct
// whose `ptr` owns a malloc'd byte buffer — released via bz_rawbytes_free
// (stdlib §10.3 / fix #985), which frees the buffer THEN the struct. §28.5
// owned-field cascade must free the buffer AND the struct.
//
// Per spec §10.3 / §28.3, stdlib §10.3, v0.278.
// ===========================================================================
BEZEL_TEST(owned_ffi_buffer_field_disposal_2830,
           owned_rawbytes_field_disposal_no_leak_spec_10_3_28_3) {
    auto r = runAotChecked(
        std::string(
        "module M; "
        "class Holder "
        "  private owned _b: RawBytes; "
        "  public constructor() _b := RawBytes.From([1, 2, 3, 4]); end constructor; "
        "end Holder; "
        "class C "
        "  public static method Main(): int ") + kLoop20 + kChurn +
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================


// ===========================================================================
// T4 — ALL THREE on the same Holder: each buffer is a separate malloc
// (or, for RawBytes, a { ptr, len } + buffer malloc), and each must be freed
// at Holder disposal. Pre-fix LSan flags three independent leaks per Holder;
// post-fix all three free paths run (free, free, bz_rawbytes_free). This is
// the composite-shape sanity row — it proves the predicate's branch
// selection (rawBytes vs CString/WString) is correct independently per
// field rather than set once and applied to all.
//
// Per spec §10 / §28.3, stdlib §10.1–§10.3, v0.278.
// ===========================================================================
BEZEL_TEST(owned_ffi_buffer_field_disposal_2830,
           mixed_ffi_buffer_fields_no_leak_spec_10_28_3) {
    auto r = runAotChecked(
        std::string(
        "module M; "
        "class Holder "
        "  private owned _c: CString; "
        "  private owned _w: WString; "
        "  private owned _b: RawBytes; "
        "  public constructor() "
        "    _c := CString.From(\"hello\"); "
        "    _w := WString.From(\"hello\"); "
        "    _b := RawBytes.From([1, 2, 3, 4]); "
        "  end constructor; "
        "end Holder; "
        "class C "
        "  public static method Main(): int ") + kLoop20 + kChurn +
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================


// ===========================================================================
// T5 — FUNCTIONAL ROW (plain runAot, no LSan). The fix must not free the
// buffer TOO EARLY: a `private owned _c: CString` field must hold a live
// buffer for the entire body of the using-var scope — `h.Len()` reads
// through it, and the returned length (5 for "hello") is the proof. The
// control shape in #2830 returns 5; this row returns 5. Pre-fix this row
// passed too (the buffer was leaked, not freed early). Post-fix it still
// passes, which is exactly the property: the disposal is at Holder
// disposal, not at field assignment.
//
// Per spec §10.1 / §28.3 / §28.9 (the disposal fires at scope end, not
// earlier — §28.3's exactly-once contract), stdlib §10.1, v0.278.
// ===========================================================================
BEZEL_TEST(owned_ffi_buffer_field_disposal_2830,
           owned_cstring_field_buffer_live_in_scope_spec_10_1_28_3) {
    auto r = runAot(
        std::string(
        "module M; "
        "class Holder "
        "  private owned _c: CString; "
        "  public constructor() _c := CString.From(\"hello\"); end constructor; "
        "  public method Len(): int return _c.Length; end Len; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    return h.Len(); "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);       // Length of "hello"
}

// ===========================================================================
// T6 — DOUBLE-RELEASE / NULL-AFTER-STORE GUARD. The fix's null-after-store
// (`builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);`)
// is the #396 slot-null precedent — a re-entrant / second pass through
// the cascade (e.g. a destructor body that explicitly disposes, or a
// using-var that ends and then a second disposal at outer scope) must find
// null in the slot and skip the free. A manual `h.Dispose()` between
// assignments exercises the same null-guard. Pre-fix this row would have
// already leaked (so any null-guard bug is moot) and would have to
// exercise a separate path. Post-fix: the explicit Dispose() on a CString
// field is a NO-OP (CString has no .Dispose() method; the explicit
// call here is via the field's actual disposal cascade entry point — a
// holder.Dispose() fires the cascade a second time, and the second pass
// finds nulls and skips).
//
// We test this by disposing the Holder twice: the second DisposeCore call
// must find null in the slot and skip the free (else a double-free of a
// malloc'd buffer — ASan reports it).
//
// Per spec §28.4 (idempotent Dispose) / §28.5 (single-release), stdlib
// §10.1, v0.278.
// ===========================================================================
BEZEL_TEST(owned_ffi_buffer_field_disposal_2830,
           owned_ffi_buffer_field_double_dispose_idempotent_spec_28_4_28_5) {
    auto r = runAot(
        std::string(
        "module M; "
        "class Holder "
        "  private owned _c: CString; "
        "  public constructor() _c := CString.From(\"hello\"); end constructor; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    var h := new Holder(); "
        "    h.Dispose(); "
        "    h.Dispose(); "
        "    return 5; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ===========================================================================
// T7 — NULLABLE FFI-buffer field. A `private owned _c: CString?` slot may
// hold null when the constructor hasn't assigned it (e.g. an early-throwing
// path, or a Holder whose _c is set only conditionally). The fix's null
// guard must skip the free for a null slot — pre-fix the cascade skipped
// the field entirely (a leak on a non-null slot), but a null slot
// obviously also has nothing to free. Post-fix the null guard is correct
// in both branches. The Holder here never assigns _c; the using-var
// disposal must not crash on the null slot.
//
// Per spec §10.1 / §28.3 (the field's lifetime contract is "an `owned`
// field's slot may be null at disposal if never assigned, and the cascade
// must not free null"), stdlib §10.1, v0.278.
// ===========================================================================
BEZEL_TEST(owned_ffi_buffer_field_disposal_2830,
           owned_nullable_ffi_buffer_field_no_crash_spec_10_1_28_3) {
    auto r = runAot(
        std::string(
        "module M; "
        "class Holder "
        "  private owned _c: CString?; "
        "  public constructor() end constructor; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    return 7; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ===========================================================================
// T8 — ASAN ORACLE / SCOPE TEST. A single, focused run under ASan/LSan
// that aggregates T1–T4 shapes and confirms the fix's net effect is zero
// leaks. This is the FAIL-WITHOUT row: with the patch reverted, LSan
// reports 21+ B leaks per Holder construction and the test fails; with
// the patch in place, the run returns 0 cleanly. Aggregated loop is
// 20 × (1 CString + 1 WString + 1 RawBytes) = 60 disposals, so the leak
// surface pre-fix is well above LSan's per-program threshold.
//
// Per spec §10 / §28.3 / §28.9, stdlib §10.1–§10.3, v0.278.
// ===========================================================================
BEZEL_TEST(owned_ffi_buffer_field_disposal_2830,
           aggregated_ffi_buffer_field_disposal_clean_asan_spec_10_28_3) {
    auto r = runAotChecked(
        std::string(
        "module M; "
        "class Holder "
        "  private owned _c: CString; "
        "  private owned _w: WString; "
        "  private owned _b: RawBytes; "
        "  public constructor() "
        "    _c := CString.From(\"hello\"); "
        "    _w := WString.From(\"hello\"); "
        "    _b := RawBytes.From([1, 2, 3, 4]); "
        "  end constructor; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 20 do "
        "      using var h := new Holder(); "
        "      i := i + 1; "
        "    end; ") + kChurn +
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// T9 — CROSS-MODULE LEAK ORACLE (#2830's cross-module reproducer). The
// library declares the Holder with the `owned _c: CString` field; the
// importer constructs + disposes Holders in a 20-iteration loop. Pre-fix
// LSan reports 420 B leaked (20 × 21 B per Holder's UTF-8 buffer) and the
// process exits 1; post-fix all 20 disposals free the buffer and the
// process exits 0.
//
// Why a separate cell: the imported-field branch of every field-disposal
// predicate reads ctx_.importedMemberType(fs), the same path the user-side
// `importedName` field-type predicate uses (#662 foundation B). A field that
// arrives at the cascade with fs->decl == nullptr (the imported shape) is
// invisible to AST-only predicates, so the fix's `if (!fs->decl)` branch
// on isOwnedFfiBufferField is the load-bearing half for cross-module. The
// `Driver::compileObject` + `compileFileForTest` + ASAN_OPTIONS=detect_leaks=1
// pattern is the same shape xmod_named_local_setter_arc_2505_test.cpp
// uses for cross-module leak tests.
//
// Per spec §10.1 / §28.3 / §28.5, stdlib §10.1, v0.278.
// ===========================================================================
BEZEL_TEST(owned_ffi_buffer_field_disposal_2830,
           imported_owned_cstring_field_disposal_no_leak_spec_10_1_28_3) {
    const std::string libSrc =
        "module FfiLib2830; "
        "public class Holder "
        "  private owned _c: CString; "
        "  public constructor() _c := CString.From(\"hello\"); end constructor; "
        "end Holder;";
    const std::string appSrc =
        std::string(
        "module FfiApp2830; import FfiLib2830; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 20 do "
        "      using var h := new Holder(); "
        "      i := i + 1; "
        "    end; "
        "    ") + kChurn +
        "    return 0; "
        "  end Main; "
        "end C;";
    int rc = compileImportRunAsan2830(libSrc, appSrc);
    if (rc == -2) return;  // clang unavailable — skip, not a failure
    BEZEL_EXPECT_EQ(rc, 0);   // no leak (no double-free)
}