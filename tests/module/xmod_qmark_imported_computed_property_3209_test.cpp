// Spec-derived regression test for issue #3209 — the cross-module residual of
// #3015 for `?.` on an imported COMPUTED PROPERTY (§7.5 / §23.2 / §2.5,
// v0.254). The same shape, one module boundary out.
//
// MEASURED on 226aca1 (pre-fix, real two-module build):
//   `imported?.Computed` on a non-null receiver — exit 0, `internal:` note
//   (#3079), the WHOLE enclosing statement dropped — not even the line
//   terminator. The `Console.WriteLine` call that USES the result prints
//   NOTHING; only an earlier `.` form (which reads through the exporter's
//   accessor stub) is observable.
//
// ROOT CAUSE (LlvmBackend_Exprs.cpp, the #3015 `NullCondMemberExpr` arm,
// pre-fix). `computedGetterOf(ctx_.exprSymbol(e))` and
// `blockGetterOf(ctx_.exprSymbol(e))` are BOTH AST-only — they cast
// `PropertySymbol::decl` (which ModuleLoader never assigns; there is no
// AST on the importer side) to `ast::PropertyDecl` / `ast::PropertyAccessor`
// and read the body off that. Both return null for every imported
// PropertySymbol, so the `if (ncGetter || ncBlockAcc)` gate never fires and
// control falls through to the storage path. For a COMPUTED property there
// is no storage (auto-properties are gated separately), so `storageMemberOf`
// returns null and the `!ms` guard returns nullptr — the #3015 mechanism
// verbatim, one module boundary out. The `.` form has the same import
// fallback via `importedPropGetterFns_[ps]` at ~L5296-L5405 (the §2.5/§23
// arm added by #2504), so its read of the same property through the same
// module boundary works.
//
// The fix adds a parallel arm: when the property is imported, non-virtual,
// non-interface, and NOT auto-storage
// (`sema::isAutoPropertyStorageSymbol`, the both-sources predicate — so an
// imported AUTO-property keeps its working direct-field path below), call
// the external getter stub `bz$<C>$<P>$propget` that
// `declareImportedPropertyGetters` (LlvmBackend_Setup.cpp:2776) declared,
// with the null-checked receiver, and lift the result to nullable exactly
// as the local block-form-getter branch above does. The throw edge gets the
// same `emitPendingCheck` (a block-form getter's `throw` propagates through
// the pending-exception global, §31.1/§31.2) and the same
// `registerFreshNewReceiverTempUnwind` / `disposeFreshNewReceiverTemp`
// reclaim discipline (§28.3, #2816 / #2751).
//
// AUTO-PROPERTY CARVE-OUT (measured, important). An imported
// `public property X: int := v;` is still routed to the LOWER `storageMemberOf`
// path, which correctly resolves the backing FieldSymbol through #2478's
// wire-bit fallback (`isAutoPropertyStorageSymbol`'s `importedPropertyIsAutoStorage`
// side). T3 below pins that this carve-out is doing its job: an
// auto-property still lifts via the direct-field path, and the imported
// getter stub for it (which `emitExportedPropertyGetters` does NOT emit —
// there is no body to wrap) is never invoked.
//
// SCOPE DELIBERATE. (a) The virtual / interface / indexer case is a
// separate path (#3016 at ~L2333 of the same file) and untouched here —
// the issue explicitly excludes it. (b) The `?.` form on a NON-NULLABLE
// generic is rejected at sema by #2884 (decline-with-reasons); no extra
// gate is needed here. (c) `declareImportedPropertyGetters` filters
// virtual / interface / indexer, so the `importedPropGetterFns_` map is
// only populated for the shape this fix handles — a `find` miss is the
// correct fall-through.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <utility>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
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
// COVERAGE SHORT). popen() at L96 is kept — per-stem uniqueness already
// mitigates the file-overwrite race; full fork isolation is a separate concern.
const std::filesystem::path& wtDir3209() {
    return wtScratchDir("wt-3209");
}

struct TempFile3209 {
    std::string path;
    explicit TempFile3209(std::string p) : path(std::move(p)) {}
    ~TempFile3209() { std::remove(path.c_str()); }
};

void writeSource3209(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj3209(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

std::string runCapture3209(const std::string& exe, int& code) {
    std::string runCmd = exe + " 2>/dev/null";
    FILE* p = popen(runCmd.c_str(), "r");
    std::string out;
    if (!p) { code = -2; return out; }
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p))
        out.append(buf, n);
    int status = pclose(p);
    if (status == -1)             code = -2;
    else if (WIFSIGNALED(status)) code = -3;
    else if (!WIFEXITED(status))  code = -1;
    else                          code = WEXITSTATUS(status);
    return out;
}

// {exitCode, stdout}. -2 = dependency/toolchain unavailable (SKIP — the
// ANTI-VACUITY guard); -1 = the importer failed to compile — which is
// precisely the pre-fix behaviour this test pins, so it must be
// distinguishable from a pass.
std::pair<int, std::string> compileImportRun3209(const std::string& libSrc,
                                                 const std::string& appSrc) {
    auto dir = wtDir3209();
    std::string stem = unique_stem("bz3209_");
    TempFile3209 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3209 aObj((dir / (stem + "_a.o")).string());
    writeSource3209(aSrc.path, libSrc);
    if (!compileToObj3209(aSrc.path, aObj.path)) return {-2, ""};
    if (!std::ifstream(aObj.path).good()) return {-2, ""};

    TempFile3209 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3209 bExe((dir / stem).string());
    writeSource3209(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
             .success)
        return {-1, ""};
    int code = 0;
    std::string out = runCapture3209(bExe.path, code);
    return {code, out};
}

std::pair<int, std::string> compileLocalRun3209(const std::string& src) {
    auto dir = wtDir3209();
    std::string stem = unique_stem("bz3209_loc_");
    TempFile3209 sFile((dir / (stem + ".bzl")).string());
    TempFile3209 eFile((dir / stem).string());
    writeSource3209(sFile.path, src);
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, sFile.path, eFile.path, {})
             .success)
        return {-1, ""};
    int code = 0;
    std::string out = runCapture3209(eFile.path, code);
    return {code, out};
}

const char* kLibExpr3209 =
    "module Dep3209Expr; "
    "public class Box3209Expr "
    "  public property C: int => 5; "
    "  public constructor() end constructor; "
    "end Box3209Expr;";

const char* kLibBlock3209 =
    "module Dep3209Block; "
    "public class Box3209Block "
    "  public property S: int "
    "    get => do return 11; end; "
    "  end S; "
    "  public constructor() end constructor; "
    "end Box3209Block;";

const char* kLibAuto3209 =
    "module Dep3209Auto; "
    "public class Box3209Auto "
    "  public property A: int := 7; "
    "  public constructor() end constructor; "
    "end Box3209Auto;";

const char* kLibStr3209 =
    "module Dep3209Str; "
    "public class Box3209Str "
    "  public property N: string => \"hello\"; "
    "  public constructor() end constructor; "
    "end Box3209Str;";

}  // namespace

// ===========================================================================
// ★ T1 — CORE REPRO FROM #3209. `b?.C ?? 99` on an IMPORTED class with an
// EXPRESSION-BODIED computed property `C`. Pre-fix: exit 0, only the `.`
// line printed, the `?.` line dropped (internal: … #3079). Post-fix:
// exit 0, BOTH lines printed, the `?.` line prints `qdot_comp=5`.
//
// Asserts the PRINTED VALUE, not just exit code — a test that only
// checks exit 0 passes today, which is exactly the failure mode the
// issue calls out. // per §7.5 / §23.2 / §2.5, v0.254
// ===========================================================================
BEZEL_TEST(xmod_qmark_imported_computed_property_3209,
           t1_imported_expr_bodied_property_qmark_prints_value_7_5_23_2) {
    auto [code, out] = compileImportRun3209(
        kLibExpr3209,
        "module App3209T1; import Dep3209Expr; "
        "public class C3209T1 "
        "  public static method Main(): int "
        "    using var b := new Box3209Expr(); "
        "    Console.WriteLine(\"dot_comp=\" + (b.C).ToString()); "
        "    Console.WriteLine(\"qdot_comp=\" + (b?.C ?? 99).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C3209T1;");
    BEZEL_EXPECT_TRUE(code != -2);  // anti-vacuity: the dependency compiled
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    // The exact line content is the assertion — pre-fix, the `qdot_comp=`
    // line is missing ENTIRELY (no value, no newline, no error).
    BEZEL_EXPECT_TRUE(out.find("dot_comp=5\n") != std::string::npos);
    BEZEL_EXPECT_TRUE(out.find("qdot_comp=5\n") != std::string::npos);
}

// ===========================================================================
// T2 — NULL-RECEIVER BRANCH. A null `BoxP6?` typed local must yield null
// (§7.5 — "returns null if x is null") and the `?? 99` must provide the
// fallback. Pre-fix the line was dropped entirely so neither -1 nor 99
// was observable. Post-fix the `qdot_null=-1` line prints. // per §7.5
// ===========================================================================
BEZEL_TEST(xmod_qmark_imported_computed_property_3209,
           t2_null_receiver_branch_yields_null_then_coalesce_7_5) {
    auto [code, out] = compileImportRun3209(
        kLibExpr3209,
        "module App3209T2; import Dep3209Expr; "
        "public class C3209T2 "
        "  public static method Main(): int "
        "    using var b := new Box3209Expr(); "
        "    using var bn: Box3209Expr? := null; "
        "    Console.WriteLine(\"qdot_null=\" + (bn?.C ?? -1).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C3209T2;");
    BEZEL_EXPECT_TRUE(code != -2);
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(out.find("qdot_null=-1\n") != std::string::npos);
}

// ===========================================================================
// T3 — AUTO-PROPERTY CARVE-OUT (the both-sources predicate's branch). An
// imported `public property A: int := 7;` is still routed to the lower
// `storageMemberOf` path (NOT the new imported-getter arm), because
// `sema::isAutoPropertyStorageSymbol` answers true through #2478's
// wire-bit fallback. Without the carve-out, this test would call
// `bz$<C>$<A>$propget` — which `emitExportedPropertyGetters` does NOT
// emit for an auto-property (no body to wrap) — and link would fail.
// Pinning the PRINTED VALUE proves the carve-out is in place and the
// direct-field path is intact. // per §23.1 / §9.2 / §22.8, v0.254
// ===========================================================================
BEZEL_TEST(xmod_qmark_imported_computed_property_3209,
           t3_imported_auto_property_still_uses_storage_path_23_1) {
    auto [code, out] = compileImportRun3209(
        kLibAuto3209,
        "module App3209T3; import Dep3209Auto; "
        "public class C3209T3 "
        "  public static method Main(): int "
        "    using var b := new Box3209Auto(); "
        "    Console.WriteLine(\"dot_auto=\" + (b.A).ToString()); "
        "    Console.WriteLine(\"qdot_auto=\" + (b?.A ?? 99).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C3209T3;");
    BEZEL_EXPECT_TRUE(code != -2);
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(out.find("dot_auto=7\n") != std::string::npos);
    BEZEL_EXPECT_TRUE(out.find("qdot_auto=7\n") != std::string::npos);
}

// ===========================================================================
// T4 — BLOCK-FORM IMPORTED PROPERTY (`get => do … end;`). The local
// computed-getter `?.` path has a SPECIALIZED block-form arm (the
// `ncBlockAcc` branch at ~L2279-L2288 of LlvmBackend_Exprs.cpp — it calls
// `ensurePropertyBlockGetter` because a block-form getter is a standalone
// function, so `return` inside its body terminates only the getter, not
// the enclosing method, §21.7). The imported fallback has no
// `ncBlockAcc` to detect the form — `declareImportedPropertyGetters`
// declares the same `bz$<C>$<P>$propget` stub for every imported
// non-virtual computed property, and the body that gets emitted by
// `emitExportedPropertyGetters` already wraps `ensurePropertyBlockGetter`
// (or its sibling for expression-bodied). So both expression-bodied and
// block-form imported computed properties route through the same imported
// getter stub at the call site, and the `?.` arm must handle them both.
//
// 11 is the value the block getter returns; the value is unobservable from
// a passing exit code, so it is asserted explicitly. // per §7.5 / §23.3
// / §2.5, v0.254
// ===========================================================================
BEZEL_TEST(xmod_qmark_imported_computed_property_3209,
           t4_imported_block_form_property_qmark_prints_value_7_5_23_3) {
    auto [code, out] = compileImportRun3209(
        kLibBlock3209,
        "module App3209T4; import Dep3209Block; "
        "public class C3209T4 "
        "  public static method Main(): int "
        "    using var b := new Box3209Block(); "
        "    Console.WriteLine(\"dot_block=\" + (b.S).ToString()); "
        "    Console.WriteLine(\"qdot_block=\" + (b?.S ?? 99).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C3209T4;");
    BEZEL_EXPECT_TRUE(code != -2);
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(out.find("dot_block=11\n") != std::string::npos);
    BEZEL_EXPECT_TRUE(out.find("qdot_block=11\n") != std::string::npos);
}

// ===========================================================================
// T5 — REFERENCE-TYPED IMPORTED PROPERTY (`string =>`). The §7.5 lift's
// three branches (§7.5 always-nullable result): reference-typed (null
// pointer when receiver is null, else the pointer), already-nullable value
// (passes through with the receiver's edge), and bare value (wraps into
// the tagged {i8 hasValue; T} struct). T1 covers the value-typed bare
// case. T5 covers the reference-typed case. Same null-check, same call
// shape; only the lift branch differs. // per §7.5, v0.254
// ===========================================================================
BEZEL_TEST(xmod_qmark_imported_computed_property_3209,
           t5_imported_reference_property_qmark_null_check_7_5) {
    auto [code, out] = compileImportRun3209(
        kLibStr3209,
        "module App3209T5; import Dep3209Str; "
        "public class C3209T5 "
        "  public static method Main(): int "
        "    using var b := new Box3209Str(); "
        "    Console.WriteLine(\"dot_str=\" + b.N); "
        "    var bs: Box3209Str? := b; "
        "    var s := bs?.N ?? \"fallback\"; "
        "    Console.WriteLine(\"qdot_str=\" + s); "
        "    using var bsn: Box3209Str? := null; "
        "    var sn := bsn?.N ?? \"fallback\"; "
        "    Console.WriteLine(\"qdot_str_null=\" + sn); "
        "    return 0; "
        "  end Main; "
        "end C3209T5;");
    BEZEL_EXPECT_TRUE(code != -2);
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(out.find("dot_str=hello\n") != std::string::npos);
    BEZEL_EXPECT_TRUE(out.find("qdot_str=hello\n") != std::string::npos);
    BEZEL_EXPECT_TRUE(out.find("qdot_str_null=fallback\n") != std::string::npos);
}

// ===========================================================================
// T6 — SINGLE-MODULE LOCAL TWIN CONTROL. The identical program with the
// class in the SAME module must still work — it pinned the #3015 path
// before this fix and must pin it now too. The module boundary is the
// entire difference between T1 and T6; if T6 breaks, the fix is
// over-broad (it touched a path the issue did not authorize). // per
// §7.5 / §23.2
// ===========================================================================
BEZEL_TEST(xmod_qmark_imported_computed_property_3209,
           t6_local_expr_bodied_property_unchanged_control_7_5_23_2) {
    auto [code, out] = compileLocalRun3209(
        "module Local3209T6; "
        "public class Box3209T6 "
        "  public property C: int => 5; "
        "  public constructor() end constructor; "
        "end Box3209T6; "
        "public class C3209T6 "
        "  public static method Main(): int "
        "    using var b := new Box3209T6(); "
        "    Console.WriteLine(\"local_dot=\" + (b.C).ToString()); "
        "    Console.WriteLine(\"local_qdot=\" + (b?.C ?? 99).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C3209T6;");
    BEZEL_EXPECT_TRUE(code != -2);
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(out.find("local_dot=5\n") != std::string::npos);
    BEZEL_EXPECT_TRUE(out.find("local_qdot=5\n") != std::string::npos);
}

}  // namespace bezel::module
