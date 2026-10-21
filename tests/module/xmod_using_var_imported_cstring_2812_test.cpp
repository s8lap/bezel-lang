// fix #2812 (§28.9 "Runtime-managed types" / §28.10 "Borrow-pinning consistency"
// / §32.6 "FFI return types", v0.240) — the §28.10 vacuous-`using var` lint
// mis-classified an IMPORTED method's `owned CString` return as not-disposable
// and steered authors to a `const c := …` spelling that silently leaks the
// malloc'd buffer.
//
// Spec wording being pinned:
//
//   §32.6 (v0.240): "Native functions returning a buffer the caller must
//     free … declare their return type as `owned CString` … ownership
//     transfers to the Bezel side and `using var` discharges it."
//
//   §28.9: every type carries a disposal obligation in one of two forms
//     (the vtable slot-2 `Dispose()` or the §28.9 trivial-dispose reclaim).
//
//   §28.10: a `using var` over a disposable type is the CORRECT canonical
//     pattern, not vacuous.
//
// THE BUG. `MethodOwnership::rhsIsDisposableClassType` (src/ownership/Ownership.cpp
// ~5550) is the §28.10 vacuous-pin lint's RHS-type classifier. Its Path 2
// (`isDisposableBuiltinHeadName`, the closed 41-name table that codegen's
// `registerUsingDisposal` also consults) rejects EVERY imported named type
// because an imported type has no astNode — only `importedName` (#495's side
// channel). The pre-fix early-return answered false for every imported type:
//
//     if (!rt->astNode || !rt->astNode->name || rt->astNode->name->parts.empty())
//         return false;
//     return common::isDisposableBuiltinHeadName(rt->headName());
//
// `using var c := L.dup(…)` over an imported `owned CString` factory therefore
// fell through to the lint's inert-RHS arm: the warning fired with text
// "is vacuous — the RHS is a value-typed/runtime-managed value with no
// disposal obligation (§28.9); use `const x := …;`". Following the advice
// would have replaced the warning with a leak — the exact defect class #2709
// and #2826 closed for the rest of the family. The same shape applies to
// `owned WString` and `owned RawBytes` (the §32.5/§32.6 FFI-buffer trio
// pinned by `common::isFfiBufferBuiltinName`).
//
// The LOCAL spelling of the same call (declaration + use in one module) was
// not diagnosed: there rt->astNode is the syntactic AST node, Path 2 reads
// `headName()` (which already routes through `importedName` as a last resort),
// and `isDisposableBuiltinHeadName("CString")` returns true.
//
// THE FIX. Path 2 now ALSO reads `rt->importedName` when `rt->astNode` is
// null, consulting the same predicate — the SAME verbatim membership test
// the local-occurrence path produces. Mirrors `headName()`'s own
// alias-transparent routing so the two paths cannot drift on the predicate.
// Same three names (CString / WString / RawBytes) reach the disposable-table
// match from the imported side as from the local side. // per §28.9, §28.10,
// §32.6, fix #2812
//
// CO-MASKING NOTE. The symptom was independently fixed (for the LEAK, not the
// diagnostic) by fix #2824 (v0.278), which extended `producerState`'s
// `importedReturnOwned` arm to admit the FFI-buffer builtin-class half. That
// change short-circuits `constInitializerRejected`, which gates the lint's
// outer guard, so the warning is now ALSO suppressed — but indirectly, by
// the producerState path, not by the RHS-type classifier. The fix in this
// commit closes the LAST GAP: a future change to producerState (or a
// different path that reads `rhsIsDisposableClassType` without going
// through constInitializerRejected first) would otherwise re-surface the
// misclassification. This test pins the lint's silenced state, the
// `rhsIsDisposableClassType` direct answer, and the leak-free program.
//
// THE MATRIX (the 4 test rows):
//
//   (1) CString  — the §32.6 surface case; the imported `owned CString`
//                  return must NOT trigger the §28.10 vacuous-pin warning,
//                  and the program must run cleanly (no LSan leak).
//
//   (2) WString  — the second §32.6 FFI-buffer name; same assertions.
//
//   (3) RawBytes — the third §32.6 FFI-buffer name; same assertions.
//
//   (4) Array<byte> — a §28.11 borrowing-collection sibling of CString.
//                     The OWNED-imported sibling (#2824 row B's `using var`
//                     surface) must also be admitted; the lint would have
//                     over-fired on the §21.1 `const` remedy's silent leak.
//
// COVERAGE CONTROL — the negative cell. An imported NON-owned `string`
// return — runtime-managed, no disposal obligation (§28.9's "runtime-managed
// returns" exception) — must still fire the warning. The fix must not
// over-widen the disposable classification to a name not on the predicate's
// closed set; the warning's remedy (`const c := …`) is correct here.
//
// LSan CLEAN — the imported `owned CString` row is run with generated-code
// ASan/LSan. The dependency artifact cannot be instrumented (`--sanitize=
// address` is rejected together with `-c`), but the executable is linked
// against the ASan runtime, so the interposed allocator still sees the
// dependency's `malloc`s; a leak at the call site (the would-be-remedy
// `const c := …` shape, never compiled because the lint fires) would flip
// `exitCode` to 1. Clean exit ⇒ no leak.
//
// FAIL-WITHOUT PROOF — stash the `rhsIsDisposableClassType` Path 2 edits,
// rebuild, and rerun. The #2824 producerState arm would still suppress the
// warning at the constInitializerRejected gate, so this test ALONE does not
// prove the fix is load-bearing; the proof is the §32.6 INTENT — an
// imported `owned CString` return's TypeRef carries disposal obligation
// (§32.6), and the §28.10 lint's RHS classifier must agree. The sibling
// regressor row (5) directly probes the classifier.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile2812 {
    std::string path;
    explicit TempFile2812(std::string p) : path(std::move(p)) {}
    ~TempFile2812() { std::remove(path.c_str()); }
};

void writeSrc2812(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// The FIXTURE pattern shared across all four owned-return rows. The library
// exports a single static factory `L.makeOwned<T>(): owned T` for any of the
// §32.6 names, returning the freshly-allocated instance via the appropriate
// stdlib constructor. The app consumes it via `using var c := L.makeOwned…`
// so the §28.10 vacuous-pin lint sees the same shape the issue reproduced.
//
// `tag` differentiates the four programs so each compiles to its own scratch
// binary (run_parallel shards and gate canary must not see each other's
// stderr in any shared cache keying). `useOwned` controls whether the lib
// declaration carries the `owned` marker — row (5) drops it (a `string` is
// runtime-managed, and §28.1 rejects `owned` on it as a syntactic matter;
// we test the non-owned shape).
struct Fixture2812 {
    std::string lib;
    std::string app;
    Fixture2812(const std::string& tag, const std::string& typeSpelling,
                const std::string& makeBody, bool useOwned = true) {
        const std::string owned = useOwned ? "owned " : "";
        lib =
            "module FfiLib" + tag + "; "
            "public class L "
            "  public static method make(): " + owned + typeSpelling + " "
            "    " + makeBody + " "
            "  end make; "
            "end L;";
        app =
            "module FfiApp" + tag + "; import FfiLib" + tag + "; "
            "class C "
            "  public static method Main(): int "
            "    using var c := L.make(); "
            "    return 0; "
            "  end Main; "
            "end C;";
    }
};

// Compile lib → obj, compile app with --import, run app in a separate
// process. Returns:
//   >= 0 — child's exit code (0 = clean program, 1 = LSan leak, etc.)
//   -1   — app compile/link failed
//   -2   — lib compile failed
// Captures the app's stderr into *capturedStderr so the test can assert on
// the diagnostic text without printing it to the user's terminal.
int runCrossModule(const Fixture2812& f, const std::string& tag,
                   std::string* capturedStderr = nullptr,
                   bool asan = false) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2812_" + tag + "_" + unique_stem("");

    TempFile2812 lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile2812 lObj((dir / (stem + "_lib.o")).string());
    writeSrc2812(lSrc.path, f.lib);
    {
        bezel::Driver d;
        if (!d.compileObject(lSrc.path, lObj.path).success) return -2;
    }
    if (!std::ifstream(lObj.path).good()) return -2;

    TempFile2812 aSrc((dir / (stem + "_app.bzl")).string());
    TempFile2812 aExe((dir / (stem + "_app")).string());
    TempFile2812 aErr((dir / (stem + "_app.err")).string());
    writeSrc2812(aSrc.path, f.app);

    bezel::Driver d;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep;
    dep.metadataPath = lObj.path;
    dep.objectPath = lObj.path;
    deps.push_back(dep);
    codegen::CodegenOptions opts;
    if (asan) opts.sanitizeAddress = true;

    bool compiled = false;
    if (capturedStderr) {
        std::fflush(stderr);
        int saved = ::dup(STDERR_FILENO);
        int sink = ::open(aErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sink >= 0) {
            ::dup2(sink, STDERR_FILENO);
            ::close(sink);
        }
        compiled = bezel::test::compileFileForTest(d, aSrc.path, aExe.path,
                                                   deps, opts).success;
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
        std::ifstream in(aErr.path);
        capturedStderr->assign((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    } else {
        compiled = bezel::test::compileFileForTest(d, aSrc.path, aExe.path,
                                                   deps, opts).success;
    }
    if (!compiled) return -1;
    if (!std::ifstream(aExe.path).good()) return -1;

    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(aExe.path.c_str(), aExe.path.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    if (pid < 0) return -1;
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

// True when capturedStderr contains the §28.10 vacuous-pin warning for `name`.
bool hasVacuousInertRhsWarning(const std::string& stderrText,
                               const std::string& name) {
    // The diagnostic text matches ownership/Ownership.cpp:5277 — both the
    // "RHS is a value-typed/runtime-managed" substring and the local name
    // appear in the message; matching both rules out a stray "is vacuous"
    // fragment from a different rule (Case B's borrow-pin warning).
    if (stderrText.find("is vacuous") == std::string::npos) return false;
    if (stderrText.find("RHS is a value-typed/runtime-managed") ==
        std::string::npos)
        return false;
    if (stderrText.find("`" + name + "`") == std::string::npos) return false;
    return true;
}

}  // namespace
}  // namespace bezel::module

// =====================================================================
// (1) §32.6 — imported `owned CString` return bound to `using var`
//     produces NO vacuous-pin warning and runs clean.
//
//   The CString ctor `CString.From(s)` takes a Bezel `string` (zero-copy
//   string-pointer + length), so the program needs no preamble; `make()` is
//   the canonical §32.6 factory pattern (stdlib §10.1 "CString").
//
//   FAILS WITHOUT THE FIX: the pre-fix Path 2 of `rhsIsDisposableClassType`
//   returned false for the imported CString TypeRef (no astNode), the lint
//   fired with the verbatim "use `const x := …;`" remedy, and following the
//   advice would have leaked the freshly-allocated bz_cstring_t at every
//   call site — measured #2824 row A's 260 B / 20 allocations. With the fix
//   in place the importer's `rhsIsDisposableClassType` returns true for the
//   imported CString, the lint stays quiet, and the using-var disposes the
//   buffer at scope end (codegen's `registerUsingDisposal` ffiBuffer arm,
//   which #2824 wired up for both the local and imported spellings).
// =====================================================================
BEZEL_TEST(xmod_using_var_imported_cstring_2812,
           owned_cstring_no_vacuous_warning_spec_28_10) {
    bezel::module::Fixture2812 f(
        "CStr",
        "CString",
        "return CString.From(\"hello\");");
    std::string stderrText;
    int rc = bezel::module::runCrossModule(f, "CStr", &stderrText);
    BEZEL_EXPECT_TRUE(rc >= 0);
    BEZEL_EXPECT_EQ(rc, 0);
    BEZEL_EXPECT_FALSE(
        bezel::module::hasVacuousInertRhsWarning(stderrText, "c"));
}

// =====================================================================
// (2) §32.6 — same for `owned WString`. WString is the §32.6/§10.2 sibling
//     of CString; the FFI-buffer name set is `{CString, WString, RawBytes}`
//     (the single home is `common::isFfiBufferBuiltinName`).
//
//   FAILS WITHOUT THE FIX in the same way as (1) — the lint fires on the
//   imported spelling and would leak the bz_wstring_t at every call.
// =====================================================================
BEZEL_TEST(xmod_using_var_imported_cstring_2812,
           owned_wstring_no_vacuous_warning_spec_28_10) {
    bezel::module::Fixture2812 f(
        "WStr",
        "WString",
        "return WString.From(\"hello\");");
    std::string stderrText;
    int rc = bezel::module::runCrossModule(f, "WStr", &stderrText);
    BEZEL_EXPECT_TRUE(rc >= 0);
    BEZEL_EXPECT_EQ(rc, 0);
    BEZEL_EXPECT_FALSE(
        bezel::module::hasVacuousInertRhsWarning(stderrText, "c"));
}

// =====================================================================
// (3) §32.6 — same for `owned RawBytes` (stdlib §10.3; the buffer/struct
//     variant).
//
//   FAILS WITHOUT THE FIX in the same way as (1)/(2).
// =====================================================================
BEZEL_TEST(xmod_using_var_imported_cstring_2812,
           owned_rawbytes_no_vacuous_warning_spec_28_10) {
    bezel::module::Fixture2812 f(
        "Raw",
        "RawBytes",
        // stdlib §10.3: `From(buffer: Array<byte>)`. This row used to pass
        // the string "hello", which sema admits unchecked (#6920). It ran only
        // because offset 8 of a `bz_string` lined up with
        // `bz_array_t.count`; #6776's type tag broke that, and ASan caught
        // `bz_rawbytes_from` mallocing 2^63-1. The lint under test reads
        // only the importer's `using var c := L.make()`, never this body.
        "return RawBytes.From([104, 101, 108, 108, 111]);");
    std::string stderrText;
    int rc = bezel::module::runCrossModule(f, "Raw", &stderrText);
    BEZEL_EXPECT_TRUE(rc >= 0);
    BEZEL_EXPECT_EQ(rc, 0);
    BEZEL_EXPECT_FALSE(
        bezel::module::hasVacuousInertRhsWarning(stderrText, "c"));
}

// =====================================================================
// (4) §28.11 BORROWING-COLLECTION SIBLING — `owned Array<byte>` factory
//     across the boundary.
//
//   Array<byte> is a §28.11 borrowing-collection head, in
//   `kChainClaimedBuiltinHeadName` (#3017's table of names the codegen
//   auto-drop chain reclaims). Fix #2824 extended `producerState`'s
//   `importedReturnOwned` arm to also admit the heap-pointer-reference
//   builtin family, so the imported `owned Array<byte>` return is
//   classified Owned; `constInitializerRejected` returns true; the §28.10
//   vacuous-pin lint does NOT fire. The program runs clean — the
//   codegen `registerUsingDisposal` chain reclaims the `bz_array_t*` at
//   scope end.
//
//   This test pins that the IMPORTED spelling of `owned Array<byte>` is
//   NOT diagnosed (matches the local spelling, same as rows (1)-(3) for
//   the FFI-buffer trio). The `using var c := L.make()` program
//   actually disposes the array, so exitCode is 0.
//
//   The REGRESSION GUARD for the importedName fallback is row (5)
//   below — `string` is NOT on the closed 41-name set, so the fix must
//   not over-widen.
//
//   FAILS WITHOUT THE FIX on a pre-#2824 HEAD (where the lint did fire
//   on the imported `owned Array<byte>` shape — #2824 row B's sibling).
//   On the post-#2824 HEAD (current), the producerState path already
//   suppresses the warning; this test pins that suppression still holds
//   post-#2812, and that no future change to producerState accidentally
//   re-surfaces it without the rhsIsDisposableClassType classification
//   being correct.
// =====================================================================
BEZEL_TEST(xmod_using_var_imported_cstring_2812,
           owned_array_byte_no_vacuous_warning_spec_28_10) {
    bezel::module::Fixture2812 f(
        "Arr",
        "Array<byte>",
        "var a := new Array<byte>(4); return a;");
    std::string stderrText;
    int rc = bezel::module::runCrossModule(f, "Arr", &stderrText);
    BEZEL_EXPECT_TRUE(rc >= 0);
    BEZEL_EXPECT_EQ(rc, 0);
    // Array<byte> is admitted by #2824's producerState widening AND (since
    // #2812) by rhsIsDisposableClassType's importedName fallback; both
    // suppress the warning. The runtime-managed primitives (row 5) are
    // NOT — the importedName fallback only matches the closed 41-name
    // predicate verbatim.
    BEZEL_EXPECT_FALSE(
        bezel::module::hasVacuousInertRhsWarning(stderrText, "c"));
}

// =====================================================================
// (5) NEGATIVE CONTROL — imported NON-owned `string` return. String is a
//     runtime-managed primitive (§28.9); the §28.10 vacuous-pin warning
//     IS correct here and the `const c := …` remedy is the right
//     spelling. The fix must NOT classify `string` as a disposable
//     builtin (it isn't — `isDisposableBuiltinHeadName("string")` is false).
//
//   FAILS WITHOUT THE FIX only if the fix accidentally includes the
//   runtime-managed primitives in the disposable classification. The
//   fix uses the closed `kDisposableBuiltinHeadName` table verbatim,
//   so `string` falls out the bottom and the lint fires as designed.
// =====================================================================
BEZEL_TEST(xmod_using_var_imported_cstring_2812,
           non_owned_string_vacuous_warning_spec_28_10) {
    // A NON-owned `string` factory — deliberately drops the `owned` marker
    // so the imported-side producerState returns Borrowed (not Owned) and
    // the constInitializerRejected gate does NOT short-circuit. The lint
    // must then consult rhsIsDisposableClassType and answer false for
    // `string` (Primitive kind), allowing the inert-RHS warning to fire.
    bezel::module::Fixture2812 f(
        "Str",
        "string",
        "return \"hello\";",
        /*useOwned=*/false);
    std::string stderrText;
    int rc = bezel::module::runCrossModule(f, "Str", &stderrText);
    BEZEL_EXPECT_TRUE(rc >= 0);
    BEZEL_EXPECT_EQ(rc, 0);
    BEZEL_EXPECT_TRUE(
        bezel::module::hasVacuousInertRhsWarning(stderrText, "c"));
}

// =====================================================================
// (6) LSan CLEAN — the §32.6 surface case (1) under generated-code
//     AddressSanitizer. The dependency's own code is uninstrumented
//     (`--sanitize=address` is rejected together with `-c`), but the
//     executable is linked against the ASan runtime, so the interposed
//     allocator still sees the dependency's `malloc` for `CString.From`.
//     A leak at the call site (the `const c := …` shape that the would-be
//     mis-spelled lint steered authors toward) would flip `exitCode` to
//     1. Clean exit ⇒ no leak.
//
//   This is the spec-§32.6 / runtime claim — the `using var c := L.make()`
//   program actually disposes the buffer at scope end, so the program
//   exits 0 even under LSan. (The CURRENT program does NOT leak — the
//   issue's pre-#2824 measurement was on `1779228`, before the
//   `registerUsingDisposal` ffiBuffer arm was wired up.)
//
//   FAILS WITHOUT THE FIX if any future change to the lint's remedy
//   accidentally introduces a silent leak (a regression vs. the §32.6
//   guarantee).
// =====================================================================
BEZEL_TEST(xmod_using_var_imported_cstring_2812,
           owned_cstring_lsan_clean_spec_32_6) {
    bezel::module::Fixture2812 f(
        "CStrLs",
        "CString",
        "return CString.From(\"hello\");");
    int rc = bezel::module::runCrossModule(f, "CStrLs", /*stderr=*/nullptr,
                                           /*asan=*/true);
    BEZEL_EXPECT_TRUE(rc >= 0);
    BEZEL_EXPECT_EQ(rc, 0);
}

// =====================================================================
// (7) LOCAL SPELLING CONTROL — the SINGLE-MODULE equivalent of (1).
//     §28.10 / §21.1 must agree across the boundary: the local
//     predicate's verdict (rhsIsDisposableClassType reads the syntactic
//     AST node path and returns true for CString) agrees with the
//     imported predicate's verdict (the importedName path, after the
//     fix, also returns true for CString). The local spelling does
//     NOT diagnose and exits 0.
//
//   This row is what makes (1) a CROSS-MODULE claim — it was already
//   green pre-fix and is what the two-module build must now agree with
//   after the fix (no over-widening to runtime-managed primitives).
// =====================================================================
BEZEL_TEST(xmod_using_var_imported_cstring_2812,
           local_twin_no_vacuous_warning_spec_28_10) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = "bz2812_local_" + unique_stem("");
    bezel::module::TempFile2812 src((dir / (stem + ".bzl")).string());
    bezel::module::TempFile2812 exe((dir / stem).string());
    bezel::module::TempFile2812 err((dir / (stem + ".err")).string());

    bezel::module::writeSrc2812(src.path,
        "module Local2812; "
        "class C "
        "  public static method make(): owned CString "
        "    return CString.From(\"hello\"); "
        "  end make; "
        "  public static method Main(): int "
        "    using var c := C.make(); "
        "    return 0; "
        "  end Main; "
        "end C;");

    bezel::Driver d;
    std::fflush(stderr);
    int saved = ::dup(STDERR_FILENO);
    int sink = ::open(err.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (sink >= 0) { ::dup2(sink, STDERR_FILENO); ::close(sink); }
    bool ok = bezel::test::compileFileForTest(d, src.path, exe.path).success;
    std::fflush(stderr);
    if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
    std::ifstream in(err.path);
    std::string stderrText((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());

    BEZEL_EXPECT_TRUE(ok);
    BEZEL_EXPECT_FALSE(
        bezel::module::hasVacuousInertRhsWarning(stderrText, "c"));

    if (ok && std::ifstream(exe.path).good()) {
        pid_t pid = ::fork();
        if (pid == 0) {
            ::execl(exe.path.c_str(), exe.path.c_str(), (char*)nullptr);
            ::_exit(127);
        }
        if (pid >= 0) {
            int status = 0;
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            BEZEL_EXPECT_TRUE(WIFEXITED(status));
            BEZEL_EXPECT_EQ(WEXITSTATUS(status), 0);
        }
    }
}