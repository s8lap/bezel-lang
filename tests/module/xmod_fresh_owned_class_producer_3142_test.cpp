// Spec-derived regression test for issue #3142 — family F5 of the
// imported-symbol classification seam (umbrella #3152).
//
// §2.8.3 rule 4 (imported artifacts) / §28.3 (Local variables, ownership, and
// borrows) / §15.3, v0.252. Audit:
// docs/proposals/papers/imported-symbol-audit-2026-08-13.md, family F5.
//
// ── THE DEFECT ─────────────────────────────────────────────────────────────
//
// `isFreshOwnedClassProducer` (src/common/OwnershipFacts.cpp) — the SHARED
// sema/ownership/codegen core that answers "is this expression a fresh owned
// class temporary?" — decided the class question with
//
//     dynamic_cast<const ast::TypeDecl*>(rt->declaration->decl)
//
// `ModuleLoader` never assigns `Symbol::decl` for an imported type; there is no
// `ast::TypeDecl` to assign, because the importing compilation has no AST for a
// dependency's types at all. So that cast is null for EVERY imported type and
// the predicate answered "not a fresh owned class producer" — silently, with no
// diagnostic, and only across a module boundary. The §28.3 disposal obligation
// was therefore never registered and the temporary LEAKED.
//
// The fact is carried on the symbol on both sides: `TypeSymbol::kind`, written
// by `SymbolCollection::collectTypeDecl` locally and by `fromMetaTypeDeclKind`
// at `Loader::reconstructTypeSymbol`, ModuleLoader.cpp:793 —
// `t->kind = fromMetaTypeDeclKind(td.kind);` — and its NESTED-shell twin,
// `registerNestedTypeShells` at :6207 —
// `t->kind = fromMetaTypeDeclKind(ntd.kind);` — for an import.
// The fix reads that.
//
// ── MEASURED, on this base, with a real two-module build ───────────────────
//   (`bezelc -c dep.bzl` then `bezelc --sanitize=address --import dep.o app.bzl`)
//
//   T1  arr.Add(new Res(i)) x200, `Res` IMPORTED  3072 bytes / 192 allocations
//   T1c the SAME program, `Res` declared LOCALLY  clean, exit 0
//   T2  (new Res(i)).v      x200, `Res` IMPORTED  3184 bytes / 199 allocations
//
// T1c is the point of the whole file: the two programs are otherwise identical,
// so what leaks is not the shape but the MODULE BOUNDARY. It is also the
// differential control — it passed before the fix and must keep passing after,
// which is what pins that the change did not alter the local answer.
//
// Two independent call paths reach the one predicate (LlvmBackend_Calls.cpp's
// borrowing-collection `Add` argument, #1881; LlvmBackend_Calls2.cpp's
// fresh-producer receiver temp, #1053/#415), so a fix that repaired only one
// would leave the other red.
//
// Generated-code ASan/LSan is enabled on the importer, and LeakSanitizer forces
// the process exit code to 1 on any leak — so `exit == 0` IS the leak assertion.
// Loop-amplified to 200 iterations because a one-shot `Main` is not a reliable
// LSan oracle (a leftover register copy makes a leaked block read as reachable).

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

struct TempFile3142 {
    std::string path;
    explicit TempFile3142(std::string p) : path(std::move(p)) {}
    ~TempFile3142() { std::remove(path.c_str()); }
};

void writeSource3142(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// -2 = the dependency toolchain is unavailable (test skips); -1 = the app did
// not build or did not exit normally; otherwise the program's exit code.
int compileImportAndRunAsan3142(const std::string& libSrc,
                                const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3142_");
    TempFile3142 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3142 aObj((dir / (stem + "_a.o")).string());
    writeSource3142(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile3142 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3142 bExe((dir / stem).string());
    writeSource3142(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps, opts)
             .success)
        return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// The single-module build of one source, same ASan settings — for the control.
int compileAndRunAsan3142(const std::string& src) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3142l_");
    TempFile3142 aSrc((dir / (stem + ".bzl")).string());
    TempFile3142 aExe((dir / stem).string());
    writeSource3142(aSrc.path, src);
    bezel::Driver driver;
    codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    if (!bezel::test::compileFileForTest(driver, aSrc.path, aExe.path, {}, opts)
             .success)
        return -1;
    int status = std::system(aExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

const char* kDep =
    "module DepF5_3142; "
    "public class Res3142 "
    "  public v: int := 0; "
    "  public constructor(x: int) v := x; end constructor; "
    "end Res3142; ";

}  // namespace

// ===========================================================================
// T1 — a fresh `new` of an IMPORTED class as the argument of a BORROWING
// `Array<T>.Add`. §28.3: the collection borrows, so the caller keeps the
// obligation and the full-expression boundary is the call's own end (#1881).
// Pre-fix: 3072 bytes leaked in 192 allocations. // per §28.3 / §2.8.3 rule 4
// ===========================================================================
BEZEL_TEST(xmod_fresh_owned_class_producer_3142,
           imported_class_temp_in_borrowing_add_is_disposed) {
    int r = compileImportAndRunAsan3142(
        kDep,
        "module AppF5_3142a; import DepF5_3142; "
        "public class C "
        "  public static method Main(): int "
        "    var arr: Array<Res3142> := new Array<Res3142>(); "
        "    var i: int := 0; "
        "    while i < 200 do "
        "      i := i + 1; "
        "      arr.Add(new Res3142(i)); "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T2 — the second call path: a member read directly on the fresh temporary, so
// the receiver is the producer (#1053 case 1 / #415). A different site in a
// different file consults the same predicate, which is why unifying the
// predicate — rather than patching one caller — is the fix.
// Pre-fix: 3184 bytes leaked in 199 allocations. The program also checks the
// SUM, so a "fix" that stopped the loop doing its work would not pass.
// // per §28.3 / §15.3 / §2.8.3 rule 4
// ===========================================================================
BEZEL_TEST(xmod_fresh_owned_class_producer_3142,
           imported_class_temp_as_member_receiver_is_disposed) {
    int r = compileImportAndRunAsan3142(
        kDep,
        "module AppF5_3142b; import DepF5_3142; "
        "public class C "
        "  public static method Main(): int "
        "    var t: int := 0; "
        "    var i: int := 0; "
        "    while i < 200 do "
        "      i := i + 1; "
        "      t := t + (new Res3142(i)).v; "
        "    end while; "
        "    if t <> 20100 then return 3; end if; "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// ★ THE DIFFERENTIAL CONTROL. T1's program with `Res3142` declared in the SAME
// module. It was already clean before the fix — the AST cast succeeds for a
// local symbol — so it pins that the module boundary was the whole difference,
// and that reading `TypeSymbol::kind` did not change the local answer.
// // per §28.3
// ===========================================================================
BEZEL_TEST(xmod_fresh_owned_class_producer_3142,
           local_class_temp_in_borrowing_add_still_disposed_control) {
    int r = compileAndRunAsan3142(
        "module LocalF5_3142; "
        "public class Res3142L "
        "  public v: int := 0; "
        "  public constructor(x: int) v := x; end constructor; "
        "end Res3142L; "
        "public class C "
        "  public static method Main(): int "
        "    var arr: Array<Res3142L> := new Array<Res3142L>(); "
        "    var i: int := 0; "
        "    while i < 200 do "
        "      i := i + 1; "
        "      arr.Add(new Res3142L(i)); "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(r, 0);
}

}  // namespace bezel::module
