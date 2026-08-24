// fix #2504 (§23 "Properties" / §2.8.2 cross-module lookup, v0.236): a BARE-
// identifier property-setter target (`Name := v;`, implicit `this`) inside a
// locally-declared class that inherits an auto-property from an IMPORTED base
// resolved with NO sema exprType. Codegen's isStringExprType (ctx_.exprType)
// then returned false, so isStringFieldLvalue bailed at its first check and
// lowerStringFieldStore was skipped — the setter stored the borrow's string
// pointer into the backing field WITHOUT the retain-new/release-old ARC pair,
// so the field aliased the caller's single reference; the base's disposer
// later released that reference, and the caller's own scope-end release then
// double-freed it. Confirmed live under ASan as a SEGV inside
// __asan::Allocator::Deallocate (called from bz_string_release in main) with
// garbage size-class register values — the classic double-free shape.
//
// GOVERNING RULE (§23, §2.8.2): an auto-property inherited from an imported
// base and reached via a bare identifier (implicit `this`) must behave
// identically to its explicit-`this.Name` sibling (which already worked, fixed
// as a side effect of #2490) and to its LOCAL-base sibling (which always
// worked). The bug was that sema's bare-identifier name-lookup
// (ExprTypecheck_Exprs.cpp) attached the resolved type ONLY for a LOCAL
// PropertySymbol (whose ast::PropertyDecl it could read) — an IMPORTED
// PropertySymbol has decl==null (the module loader carries no AST), so the
// dynamic_cast failed and ctx_.setExprType was never called.
//
// THE FIX (two halves, BOTH required — the issue's own drafted codegen
// widening alone had "zero observable effect" because this sema half bailed
// first; verified independently):
//  1. sema (ExprTypecheck_Exprs.cpp): when a bare identifier resolves to an
//     imported PropertySymbol (decl==null, ctx_.isImported), attach its type
//     from the importedMemberType side table via substituteMemberType, mirroring
//     the MemberAccess path (resolveMemberAccess's imported-Property branch).
//     The FieldSymbol branch got the identical sibling fix (issue #2532: a
//     bare identifier resolving to an imported base FIELD had the same
//     no-type → no-ARC → heap-corruption shape).
//  2. codegen (LlvmBackend_Setup.cpp): widen isAutoProperty -> isAutoPropertyStorage
//     (#2478's proven pattern) at all three *FieldLvalue classifiers
//     (isStringFieldLvalue / isClosureFieldLvalue / isBoxFieldLvalue) so an
//     imported (decl-null) auto-property is recognized as field storage once
//     the sema half above has attached its type. The issue's author tried this
//     widening in isolation and saw no effect — correct, because isStringExprType
//     short-circuited first; both halves together are load-bearing.
//
// This file pins the bare-identifier PROPERTY variant (the #2504 repro), the
// bare-identifier FIELD variant (#2532, same fix), and the LOCAL-base control.

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
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"


namespace bezel::module {
namespace {

namespace fs = std::filesystem;

// Per-process unique working dir under the shared scratch base. The previous
// fs::temp_directory_path() (== /tmp tmpfs) was race-prone even with
// unique_stem: parallel shards racing on the SAME /tmp parent dir overwrote
// each other's _a.bzl / _a.o / _b.bzl mid-compileObject. The
// /var/tmp/bezel-scratch parent fixes that class of flake (#3002-class
// COVERAGE SHORT). popen() at L121/252 is kept — per-stem uniqueness already
// mitigates the file-overwrite race; full fork isolation is a separate concern.
const std::filesystem::path& wtDir2504() {
    return wtScratchDir("wt-2504");
}

struct TempFile2504 {
    std::string path;
    explicit TempFile2504(std::string p) : path(std::move(p)) {}
    ~TempFile2504() { std::remove(path.c_str()); }
};

void writeSource2504(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2504(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compiles `appSrc` importing `libObj`, links + runs under ASan (exitcode=97
// maps any ASan/LSan error or signal to 97). Captures stdout so the test can
// pin the SETTER's effect (the write must land as "hello!") alongside crash-
// freedom. Returns {exitCode, stdout}; {-2, ""} if the toolchain is
// unavailable (SKIP), {-1, ""} on compile/link failure.
std::pair<int, std::string> compileImportRunAsanCapture2504(
        const std::string& libSrc, const std::string& appSrc) {
    auto dir = wtDir2504();
    std::string stem = unique_stem("bz2504_");
    TempFile2504 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2504 aObj((dir / (stem + "_a.o")).string());
    writeSource2504(aSrc.path, libSrc);
    if (!compileToObj2504(aSrc.path, aObj.path)) return {-2, ""};
    if (!std::ifstream(aObj.path).good()) return {-2, ""};

    TempFile2504 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2504 bExe((dir / stem).string());
    writeSource2504(bSrc.path, appSrc);

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);

    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps, opts).success)
        return {-1, ""};

    // LeakSanitizer is OFF: a NAMED LOCAL (here `s`) passed to a cross-module
    // auto-property setter is not yet recognized as a call-bounded borrow by
    // the string escape walker, so its own +1 (now correctly retained into
    // the field by this fix) leaks at the caller's scope end — the SAME
    // accepted, separately-filed residual leak #2490's T1/T5 document (leak-
    // over-UAF is this repo's explicit safe direction). detect_leaks=0 isolates
    // the crash/correctness pin from that orthogonal leak.
    std::string runCmd = std::string("ASAN_OPTIONS=detect_leaks=0:"
        "abort_on_error=0:exitcode=97 ") + bExe.path + " 2>/dev/null";
    FILE* p = popen(runCmd.c_str(), "r");
    std::string out;
    if (p) {
        char buf[256];
        while (std::size_t n = std::fread(buf, 1, sizeof(buf), p))
            out.append(buf, n);
        int status = pclose(p);
        if (status == -1) return {-2, ""};
        if (WIFSIGNALED(status)) return {97, out};
        if (!WIFEXITED(status)) return {-1, out};
        return {WEXITSTATUS(status), out};
    }
    return {-2, ""};
}

// ─── shared sources ──────────────────────────────────────────────────────────

// lib for the PROPERTY variant (#2504): a public auto-property on the base.
const char* kLibProp2504 =
    "module LibStrInh2504; "
    "public class Base2504 "
    "  public property Name: string := \"init\"; "
    "end Base2504;";

// lib for the FIELD variant (#2532): a public field on the base.
const char* kLibField2504 =
    "module LibFieldInh2504; "
    "public class BaseF2504 "
    "  public field: string := \"init\"; "
    "end BaseF2504;";

}  // namespace

// ─── T1: bare-identifier PROPERTY setter through IMPORTED base (the #2504 repro)
//
// FAIL-WITHOUT (verified): the ASan SEGV in bz_string_release maps to exit 97.
// PASS-WITH: exit 0 and stdout "hello!\n" (the setter's write landed and the
// retain-new/release-old kept the field's reference independent of `s`).
BEZEL_TEST(xmod_bare_ident_imported_setter_2504,
           bare_ident_property_setter_imported_base_no_crash_value_correct_23_2_8_2) {
    std::string appSrc =
        "module AppInh2504; import LibStrInh2504; "
        "public class Derived2504: Base2504 "
        "  public method SetIt(v: string) "
        "    Name := v; "              // <-- bare identifier, implicit this
        "  end SetIt; "
        "end Derived2504; "
        "public class Program2504 "
        "  public static method Main(): int "
        "    var s := \"hel\" + \"lo!\"; "
        "    using var h := new Derived2504(); "
        "    h.SetIt(s); "
        "    Console.WriteLine(h.Name); "
        "    return 0; "
        "  end Main; "
        "end Program2504;";
    auto [code, out] = compileImportRunAsanCapture2504(kLibProp2504, appSrc);
    BEZEL_EXPECT_TRUE(code != -2);  // toolchain available
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);   // no ASan crash
    BEZEL_EXPECT_EQ(out, std::string("hello!\n"));  // setter's write landed
}

// ─── T2: bare-identifier FIELD setter through IMPORTED base (#2532 sibling)
//
// Same defect shape as T1 in the adjacent FieldSymbol branch; same fix.
// FAIL-WITHOUT: exit 97 (same SEGV). PASS-WITH: exit 0 + "hello!\n".
BEZEL_TEST(xmod_bare_ident_imported_setter_2504,
           bare_ident_field_setter_imported_base_no_crash_value_correct_2_7_2_8_2) {
    std::string appSrc =
        "module AppField2504; import LibFieldInh2504; "
        "public class DerivedF2504: BaseF2504 "
        "  public method SetIt(v: string) "
        "    field := v; "             // <-- bare identifier, IMPORTED FIELD
        "  end SetIt; "
        "end DerivedF2504; "
        "public class ProgramF2504 "
        "  public static method Main(): int "
        "    var s := \"hel\" + \"lo!\"; "
        "    using var h := new DerivedF2504(); "
        "    h.SetIt(s); "
        "    Console.WriteLine(h.field); "
        "    return 0; "
        "  end Main; "
        "end ProgramF2504;";
    auto [code, out] = compileImportRunAsanCapture2504(kLibField2504, appSrc);
    BEZEL_EXPECT_TRUE(code != -2);
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_EQ(out, std::string("hello!\n"));
}

// ─── T3: CONTROL — LOCAL base, bare-identifier property setter (always worked)
//
// Confirms the fix does not disturb the local-base path: a bare-identifier
// setter target resolving to a LOCAL auto-property must still compile, run
// clean under ASan, and land the write. Same module, no import.
BEZEL_TEST(xmod_bare_ident_imported_setter_2504,
           local_base_bare_ident_setter_control_unaffected_23) {
    std::string src =
        "module AppLocal2504; "
        "public class BaseL2504 "
        "  public property Name: string := \"init\"; "
        "end BaseL2504; "
        "public class DerivedL2504: BaseL2504 "
        "  public method SetIt(v: string) "
        "    Name := v; "
        "  end SetIt; "
        "end DerivedL2504; "
        "public class ProgramL2504 "
        "  public static method Main(): int "
        "    var s := \"hel\" + \"lo!\"; "
        "    using var h := new DerivedL2504(); "
        "    h.SetIt(s); "
        "    Console.WriteLine(h.Name); "
        "    return 0; "
        "  end Main; "
        "end ProgramL2504;";
    auto dir = wtDir2504();
    std::string stem = unique_stem("bz2504_local_");
    TempFile2504 srcFile((dir / (stem + ".bzl")).string());
    TempFile2504 exe((dir / stem).string());
    writeSource2504(srcFile.path, src);

    bezel::Driver driver;
    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    BEZEL_EXPECT_TRUE(bezel::test::compileFileForTest(driver, srcFile.path, exe.path, {}, opts).success);

    std::string runCmd = std::string("ASAN_OPTIONS=detect_leaks=0:"
        "abort_on_error=0:exitcode=97 ") + exe.path + " 2>/dev/null";
    FILE* p = popen(runCmd.c_str(), "r");
    std::string out;
    if (p) {
        char buf[256];
        while (std::size_t n = std::fread(buf, 1, sizeof(buf), p))
            out.append(buf, n);
        int status = pclose(p);
        int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status)
                    : (status != -1 && WIFSIGNALED(status)) ? 97 : -1;
        BEZEL_EXPECT_EQ(code, 0);
        BEZEL_EXPECT_EQ(out, std::string("hello!\n"));
    }
}

}  // namespace bezel::module
