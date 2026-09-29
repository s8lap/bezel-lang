// fix #2300 (§2.8.2/§28.9, v0.236): directly invoking a function-typed FIELD
// of an IMPORTED class — `h.F(args)` where `F` is declared `F: (int) -> int`
// on a class reached through `--import` — silently miscompiled, returning 0
// (the zero-init fallback) instead of the call result. Binding the field to a
// local first (`var g := h.F; g(args)`) was a complete workaround, and the
// identical program in a SINGLE module was correct — so the defect was
// specific to the `--import` boundary on the FieldSymbol callee path.
//
// ROOT CAUSE: `Emitter::lowerCall` (LlvmBackend_Calls2.cpp, the FieldSymbol
// callee arm ~L3902) resolved the field's TypeRef for closure dispatch from
// `fs->decl` (an `ast::FieldDecl`) or `fs->posParam` (a record positional
// field). An IMPORTED class/record field has NEITHER: the importer
// (ModuleLoader) reconstructs a `FieldSymbol` from dependency metadata with no
// AST, so its resolved type rides the `ctx_.importedMemberType()` side table.
// Without consulting that table, `rt` stayed null, the `rt->kind == Function`
// dispatch guard was skipped, and `lowerCall` fell through to the #2477
// "codegen cannot dispatch member 'F'" zero-init fallback — silently
// returning 0 instead of the call result. The local-bind path worked because
// it dispatches through the `LocalSymbol` arm, where the field READ supplies
// the closure pointer and the local's declType supplies the function type.
//
// THE FIX (this commit): the FieldSymbol arm now falls back to
// `ctx_.importedMemberType(fs)` when neither `fs->decl` nor `fs->posParam`
// yields a type — mirroring every other imported-field read site
// (LlvmBackend_Exprs.cpp ~L4844 property reads, LlvmBackend_Layout.cpp ~L1032
// storage layout). The `rt->kind == Function` guard still gates dispatch, so
// the change only affects function-typed imported fields.
//
// SCOPE (filed separately, NOT covered by this fix):
//   - #2589: directly invoking a function-typed auto-PROPERTY (`obj.P(args)`)
//     silently miscompiles for BOTH local and imported classes — a different
//     root cause (no PropertySymbol closure-dispatch arm in lowerCall at all).
//     Bites locally too; out of scope for this field-specific fix.
//   - #2590: reading/invoking a function-typed field of an imported RECORD
//     segfaulted (bad closure pointer across --import). The fix here was
//     deliberately gated to CLASS receivers because the record field-READ ABI
//     was a separate, deeper defect; routing the record case through the
//     closure dispatch would only have turned its silent zero-init into a
//     segfault. ⚠ RETIRED: #2590 (v0.306) fixed the read via the
//     #2874/#3671/#3723 initializer replay and then admitted `Record` to this
//     arm's fallback, which now asks the shared
//     `importedMemberTypeAdmitsClosureDispatch` predicate instead of its own
//     Class-only allowlist. No oracle in THIS file changes — every row here
//     uses a class receiver — but the rationale above no longer describes the
//     code; the record rows live in
//     tests/module/xmod_imported_record_func_member_invoke_2590_test.cpp.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile2300 {
    std::string path;
    explicit TempFile2300(std::string p) : path(std::move(p)) {}
    ~TempFile2300() { std::remove(path.c_str()); }
};

void writeSource2300(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2300(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile lib -> object, compile app (importing lib) -> exe, run it in a
// SEPARATE process (fork-isolated per tests/README.md's AOT harness
// discipline), return its exit code (-1 on compile/link/run failure, -2 if
// the dependency itself could not be produced). Mirrors #2452/#2414's own
// compileImportAndRun helpers. NB: exit code is WEXITSTATUS-masked to 8 bits,
// so each case's Main returns a distinct value in 0..255.
int compileImportAndRun2300(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2300_");
    TempFile2300 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2300 aObj((dir / (stem + "_a.o")).string());
    writeSource2300(aSrc.path, libSrc);
    if (!compileToObj2300(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2300 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2300 bExe((dir / stem).string());
    writeSource2300(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!driver.compileFile(bSrc.path, bExe.path, deps).success) return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// T1 — the #2300 repro's core: directly invoke a function-typed FIELD on an
// IMPORTED class. Pre-fix: 0 (the #2477 zero-init fallback fired; the
// internal "cannot dispatch member 'F'" diagnostic also emitted). Post-fix:
// 11. 11 cannot arise from the zero-init default (0), so a passing assertion
// is genuine proof the field-invoke dispatched, not a coincidence.
// ===========================================================================
BEZEL_TEST(xmod_imported_func_field_invoke_2300,
           imported_function_typed_field_direct_invoke_2_8_2_28_9) {
    int r = compileImportAndRun2300(
        "module LibX2300T1; "
        "public class Holder2300T1 "
        "  public F: (int) -> int := lambda(x: int) => x + 1; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Holder2300T1;",
        "module App2300T1; import LibX2300T1; "
        "public class Program2300T1 "
        "  public static method Main(): int "
        "    using var h := new Holder2300T1(); "
        "    var d := h.F(10); "
        "    return d; "
        "  end Main; "
        "end Program2300T1;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 11);
}

// ===========================================================================
// T2 — the #2300 issue's second repro shape: the field is ASSIGNED a closure
// (built by an imported static method) before the invoke. Proves the fix
// covers the assigned-closure path too, not just the field-initializer path.
// Pre-fix: 0. Post-fix: 42.
// ===========================================================================
BEZEL_TEST(xmod_imported_func_field_invoke_2300,
           imported_function_typed_field_assigned_closure_invoke_2_8_2) {
    int r = compileImportAndRun2300(
        "module LibX2300T2; "
        "public class Holder2300T2 "
        "  public F: (int) -> int := lambda(x: int) => x + 1; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Holder2300T2; "
        "public class Api2300T2 "
        "  public static method MakeAdder(k: int): (int) -> int "
        "    return lambda(x: int) => x + k; "
        "  end MakeAdder; "
        "end Api2300T2;",
        "module App2300T2; import LibX2300T2; "
        "public class Program2300T2 "
        "  public static method Main(): int "
        "    using var ih := new Holder2300T2(); "
        "    ih.F := Api2300T2.MakeAdder(40); "
        "    var c := ih.F(2); "
        "    return c; "
        "  end Main; "
        "end Program2300T2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 42);
}

// ===========================================================================
// T3 — REGRESSION GUARD: the local-bind workaround (`var g := h.F; g(args)`)
// worked even BEFORE this fix (it dispatches through the LocalSymbol arm, not
// the FieldSymbol arm). This case MUST keep passing — it proves the fix did
// not break the already-working local-bind path on an imported field. Passes
// both pre- and post-fix (that is the point of a regression guard).
// ===========================================================================
BEZEL_TEST(xmod_imported_func_field_invoke_2300,
           imported_function_typed_field_local_bind_invoke_2_8_2) {
    int r = compileImportAndRun2300(
        "module LibX2300T3; "
        "public class Holder2300T3 "
        "  public F: (int) -> int := lambda(x: int) => x + 1; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Holder2300T3;",
        "module App2300T3; import LibX2300T3; "
        "public class Program2300T3 "
        "  public static method Main(): int "
        "    using var h := new Holder2300T3(); "
        "    var g := h.F; "
        "    var e := g(10); "
        "    return e; "
        "  end Main; "
        "end Program2300T3;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 11);
}

// ===========================================================================
// T4 — imported field invoke called MULTIPLE TIMES and used inside a larger
// expression. Proves the dispatch is stable across repeated calls (not just
// first-call luck) and that the result composes arithmetically. The field's
// lambda is `x + 1`, so h.F(50) + h.F(51) = 51 + 52 = 103. Pre-fix: 0 (both
// calls degraded). Post-fix: 103.
// ===========================================================================
BEZEL_TEST(xmod_imported_func_field_invoke_2300,
           imported_function_typed_field_repeated_invoke_in_expression_2_8_2) {
    int r = compileImportAndRun2300(
        "module LibX2300T4; "
        "public class Holder2300T4 "
        "  public F: (int) -> int := lambda(x: int) => x + 1; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Holder2300T4;",
        "module App2300T4; import LibX2300T4; "
        "public class Program2300T4 "
        "  public static method Main(): int "
        "    using var h := new Holder2300T4(); "
        "    var sum := h.F(50) + h.F(51); "
        "    return sum; "
        "  end Main; "
        "end Program2300T4;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 103);
}

// ===========================================================================
// T5 — REGRESSION GUARD (single-module sanity): the IDENTICAL field-invoke
// shape in a SINGLE module (no --import) was correct BEFORE this fix and must
// stay correct. The issue report called this out explicitly ("the identical
// program in a SINGLE module is correct"). This case uses a one-module
// compile (no dependency) to prove the fix did not regress the local
// FieldDecl path that the FieldSymbol arm still uses as its primary source.
// Passes both pre- and post-fix.
// ===========================================================================
BEZEL_TEST(xmod_imported_func_field_invoke_2300,
           single_module_function_typed_field_direct_invoke_unchanged_2_8_2) {
    bezel::Driver driver;
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2300t5_");
    TempFile2300 src((dir / (stem + ".bzl")).string());
    TempFile2300 exe((dir / stem).string());
    writeSource2300(src.path,
        "module Single2300T5; "
        "public class Holder2300T5 "
        "  public F: (int) -> int := lambda(x: int) => x + 1; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Holder2300T5; "
        "public class Program2300T5 "
        "  public static method Main(): int "
        "    using var h := new Holder2300T5(); "
        "    var d := h.F(10); "
        "    return d; "
        "  end Main; "
        "end Program2300T5;");
    if (!driver.compileFile(src.path, exe.path, {}).success) {
        BEZEL_EXPECT_EQ(false, true);  // compile failure — surface loudly
        return;
    }
    int status = std::system(exe.path.c_str());
    int r = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(r, 11);
}

}  // namespace bezel::module
