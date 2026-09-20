// fix #2590 (§2.8.2 / §9.1 / §9.2 / §22.8 / §23.1 / §28.9, v0.306): directly
// invoking a function-typed FIELD or auto-PROPERTY of an IMPORTED RECORD —
// `b.F(5)` where `F` is declared `F: (int) -> int := lambda(x: int) => x + 7`
// on a record reached through `--import` — silently miscompiled, returning 0
// (the #2477 "codegen cannot dispatch member" zero-init fallback) instead of
// the call result.
//
// WHAT THE ISSUE ORIGINALLY REPORTED, AND WHAT IS LEFT OF IT. #2590 was filed
// as a SIGSEGV on the *read*: `var g := b.F; g(5)` crashed, because an
// imported record's function-typed field was never INITIALIZED across the
// module boundary (records are constructed inline by value at the consumer's
// `new` site — §9.1/§9.2 — with no `bz$<T>$init` to call, so the exporter's
// lambda initializer reached the consumer as nothing and the field kept its
// §7.9 `ConstantAggregateZero` default: a null closure pointer, dereferenced
// by the invoke). That half has since been fixed by the field-initializer
// REPLAY chain — #2874 (the parsed-AST carriage `parseAndAttachFieldInit`
// publishes into `importedInstanceFieldInitExpr`), #3671 (`buildRecordValue`
// consuming it), #3723 (the writer-side `(isGenericType || Record)` widening
// in `ModuleMetadataBuilder.cpp` that populates `fieldInitText` for a
// NON-generic record) — none of which named #2590. Verified on this lane's
// baseline (origin/main 7f29b8ce): the issue's literal repro returns 12, no
// crash. T4 below is the pin that keeps it that way.
//
// THE HALF THAT WAS STILL BROKEN — and what this commit fixes — is the
// *invoke*: `b.F(5)` and `h.P(10)` still degraded to 0. `Emitter::lowerCall`
// (LlvmBackend_Calls2.cpp) recovers an IMPORTED member's declared type from
// the `ctx_.importedMemberType` side table (an imported member carries no
// `ast::FieldDecl`/`ast::PropertyDecl` — the importer reconstructs the Symbol
// from dependency metadata), and both closure-dispatch arms guarded that
// fallback with their OWN copy of a receiver-kind allowlist that excluded
// `Record`:
//   - the FieldSymbol arm (#2300)     — `Class` only
//   - the PropertySymbol arm (#2589)  — `Class || Interface`
// Both carve-outs cited THIS issue by number and gave the same reason: with
// the read broken, dispatching a record receiver would have converted #2477's
// silent zero-init into a segfault. The replay chain above retired that
// premise; the two allowlists outlived it, so `rt` stayed null for a record
// receiver, the `rt->kind == Function` dispatch guard was skipped, and the
// call fell through to the zero-init fallback.
//
// THE FIX (this commit): one shared predicate,
// `importedMemberTypeAdmitsClosureDispatch(parent)`, replaces the two drifted
// copies and admits `Record` alongside `Class` and `Interface`. §2.8.2 clause
// 1 requires a consumer to type-check and run "every well-formed use of the
// module's public surface"; §9.2 makes a function-typed record field (and
// §23.1 an auto-property) such a use; §28.9 makes its value runtime-managed
// with unbounded lifetime, so no ownership or disposal rule distinguishes the
// record receiver from the class one the same arms already served.
//
// TEST GRID — every row asserts the VALUE, because the pre-fix behaviour is a
// silent wrong-value miscompile, not a diagnostic. A located-diagnostic helper
// would be worthless here for the opposite reason too: the pre-fix compile
// SUCCEEDS (it only emits the #2477/#2434 internal notes), so "did it reject?"
// answers the same on both sides. The chosen values (12 / 11 / 105 / 23 / 42)
// are all unreachable from the zero-init default of 0.
//   T1 imported record BODY field, direct invoke     — the fix   (0  -> 12)
//   T2 imported record AUTO-PROPERTY, direct invoke  — the fix   (0  -> 11)
//   T3 imported record POSITIONAL (§9.1) field,      — the fix   (0  -> 12)
//      direct invoke
//   T4 imported record, the ISSUE'S LITERAL REPRO    — guard     (12 both)
//      (`var g := b.F; g(5)`) — pins that the READ
//      the replay chain fixed stays sound
//   T5 both spellings in one program                 — the fix   (0  -> 105)
//      (`g(90) + b.F(1)`)
//   T6 imported CLASS field + auto-property          — guard     (23 both)
//      (#2300 / #2589 controls — unchanged)
//   T7 SINGLE-MODULE record field + auto-property    — guard     (23 both)
//      (the sound `fd->type` / `pd->type` branches)
//   T8 imported record NON-function field read       — guard     (42 both)
//      (`b.N` where `N: int`) — the widened gate
//      only ever supplies `rt`; the
//      `rt->kind == Function` guard still decides
//      dispatch, so an ordinary read is untouched
//
// FAIL-WITHOUT / PASS-WITH, measured: restoring
// `src/codegen/LlvmBackend_Calls2.cpp` to its parent commit (copied aside per
// the lane rules, never `git stash`) and relinking makes T1, T2, T3 and T5
// read back 0 — each accompanied by the two internal notes "codegen cannot
// dispatch member 'F'" (#2477) and "lowerLocalDecl's initializer expression
// failed to lower" (#2434). T4, T6, T7 and T8 pass on both trees; they are
// the controls.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile2590 {
    std::string path;
    explicit TempFile2590(std::string p) : path(std::move(p)) {}
    ~TempFile2590() { std::remove(path.c_str()); }
};

void writeSource2590(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile lib -> object, compile app (importing lib) -> exe, run it in a
// SEPARATE process, return its exit code (-1 on compile/link/run failure, -2
// if the dependency itself could not be produced). Mirrors the #2300/#2589
// sibling files' own compileImportAndRun helpers verbatim, including the -2
// "toolchain unavailable" skip convention. NB: the exit code is
// WEXITSTATUS-masked to 8 bits, so every case's Main returns a value in
// 0..255.
int compileImportAndRun2590(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2590_");
    TempFile2590 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2590 aObj((dir / (stem + "_a.o")).string());
    writeSource2590(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2590 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2590 bExe((dir / stem).string());
    writeSource2590(bSrc.path, appSrc);
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

// Single-module compile + run, for T7's control (no dependency at all).
int compileAndRun2590(const std::string& src) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2590s_");
    TempFile2590 s((dir / (stem + ".bzl")).string());
    TempFile2590 exe((dir / stem).string());
    writeSource2590(s.path, src);
    bezel::Driver driver;
    if (!driver.compileFile(s.path, exe.path, {}).success) return -1;
    int status = std::system(exe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// T1 — the invoke half of #2590: a function-typed BODY field (§9.2) of an
// IMPORTED record, invoked directly. Pre-fix: 0 (the #2477 zero-init fallback
// fired, with the internal "cannot dispatch member 'F'" note). Post-fix: 12.
// 12 is unreachable from the zero-init default, so a green row is proof the
// call dispatched rather than a coincidence.
// ===========================================================================
BEZEL_TEST(xmod_imported_record_func_member_invoke_2590,
           imported_record_function_typed_field_direct_invoke_2_8_2_9_2) {
    int r = compileImportAndRun2590(
        "module LibRec2590T1; "
        "public record Box2590T1 "
        "  public F: (int) -> int := lambda(x: int) => x + 7; "
        "end Box2590T1;",
        "module App2590T1; import LibRec2590T1; "
        "public class Program2590T1 "
        "  public static method Main(): int "
        "    var b := new Box2590T1(); "
        "    var d := b.F(5); "
        "    return d; "
        "  end Main; "
        "end Program2590T1;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 12);
}

// ===========================================================================
// T2 — the auto-PROPERTY spelling (§23.1) of the same defect, which the issue
// thread folded into #2590 explicitly ("if the fix for #2590 covers the record
// field READ, the property case comes along for free and the #2589 gate can be
// widened to Record"). Pre-fix: 0. Post-fix: 11.
// ===========================================================================
BEZEL_TEST(xmod_imported_record_func_member_invoke_2590,
           imported_record_function_typed_auto_property_direct_invoke_23_1) {
    int r = compileImportAndRun2590(
        "module LibRec2590T2; "
        "public record Holder2590T2 "
        "  public property F: (int) -> int := lambda(x: int) => x + 1; "
        "end Holder2590T2;",
        "module App2590T2; import LibRec2590T2; "
        "public class Program2590T2 "
        "  public static method Main(): int "
        "    var h := new Holder2590T2(); "
        "    var d := h.F(10); "
        "    return d; "
        "  end Main; "
        "end Program2590T2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 11);
}

// ===========================================================================
// T3 — a POSITIONAL record field (§9.1). This is a distinct route into the
// same FieldSymbol arm: for a LOCAL record the arm reads the type off
// `fs->posParam`, but an IMPORTED positional field has no `posParam` either
// (no AST crosses the boundary), so it lands on the very same
// `importedMemberType` fallback the allowlist was gating. Pre-fix: 0.
// Post-fix: 12.
// ===========================================================================
BEZEL_TEST(xmod_imported_record_func_member_invoke_2590,
           imported_record_positional_function_typed_field_invoke_9_1) {
    int r = compileImportAndRun2590(
        "module LibRec2590T3; "
        "public record Pair2590T3(F: (int) -> int, K: int) "
        "end Pair2590T3;",
        "module App2590T3; import LibRec2590T3; "
        "public class Program2590T3 "
        "  public static method Main(): int "
        "    var p := new Pair2590T3(lambda(x: int) => x + 7, 3); "
        "    var d := p.F(5); "
        "    return d; "
        "  end Main; "
        "end Program2590T3;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 12);
}

// ===========================================================================
// T4 — REGRESSION GUARD for the half of #2590 that the #2874/#3671/#3723
// replay chain already fixed: the issue's LITERAL repro, the local-bind
// spelling `var g := b.F; var e := g(5);` that used to SIGSEGV on a null
// closure pointer. It returns 12 on this lane's baseline and must keep doing
// so — this row is what would catch a regression of the replay chain, which
// no test currently names #2590 for. Passes both pre- and post-fix; that is
// the point of a guard.
// ===========================================================================
BEZEL_TEST(xmod_imported_record_func_member_invoke_2590,
           imported_record_function_typed_field_local_bind_read_22_8) {
    int r = compileImportAndRun2590(
        "module LibRec2590T4; "
        "public record Box2590T4 "
        "  public F: (int) -> int := lambda(x: int) => x + 7; "
        "end Box2590T4;",
        "module App2590T4; import LibRec2590T4; "
        "public class Program2590T4 "
        "  public static method Main(): int "
        "    var b := new Box2590T4(); "
        "    var g := b.F; "
        "    var e := g(5); "
        "    return e; "
        "  end Main; "
        "end Program2590T4;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 12);
}

// ===========================================================================
// T5 — both spellings in ONE program, so the fix is shown to compose with the
// already-working local-bind path rather than replacing it: `g(90)` is 97 via
// the LocalSymbol arm and `b.F(1)` is 8 via the newly-admitted FieldSymbol
// arm, summing to 105. Pre-fix the whole sum degraded to 0 (the failed direct
// invoke zero-inits its local, and the pre-fix run returns 0, not 97).
// ===========================================================================
BEZEL_TEST(xmod_imported_record_func_member_invoke_2590,
           imported_record_local_bind_and_direct_invoke_compose_28_9) {
    int r = compileImportAndRun2590(
        "module LibRec2590T5; "
        "public record Box2590T5 "
        "  public F: (int) -> int := lambda(x: int) => x + 7; "
        "end Box2590T5;",
        "module App2590T5; import LibRec2590T5; "
        "public class Program2590T5 "
        "  public static method Main(): int "
        "    var b := new Box2590T5(); "
        "    var g := b.F; "
        "    return g(90) + b.F(1); "
        "  end Main; "
        "end Program2590T5;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 105);
}

// ===========================================================================
// T6 — REGRESSION GUARD: the imported CLASS receiver, both member kinds. This
// is exactly what #2300 (field) and #2589 (auto-property) fixed, and what the
// two receiver-kind allowlists admitted before this commit merged them into
// one predicate. 11 + 12 = 23 on both trees; a break here would mean the
// shared predicate lost an alternative the old copies had.
// ===========================================================================
BEZEL_TEST(xmod_imported_record_func_member_invoke_2590,
           imported_class_field_and_property_invoke_unchanged_2300_2589) {
    int r = compileImportAndRun2590(
        "module LibCls2590T6; "
        "public class Holder2590T6 "
        "  public F: (int) -> int := lambda(x: int) => x + 1; "
        "  public property P: (int) -> int := lambda(x: int) => x + 2; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Holder2590T6;",
        "module App2590T6; import LibCls2590T6; "
        "public class Program2590T6 "
        "  public static method Main(): int "
        "    using var h := new Holder2590T6(); "
        "    var a := h.F(10); "
        "    var b := h.P(10); "
        "    return a + b; "
        "  end Main; "
        "end Program2590T6;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 23);
}

// ===========================================================================
// T7 — REGRESSION GUARD: the identical record program in a SINGLE module was
// correct before this commit (the issue says so) and must stay correct. A
// local record's members carry a real `ast::FieldDecl`/`ast::PropertyDecl`, so
// both arms take their `fd->type` / `pd->type` branch and never reach the
// widened fallback at all. 12 + 11 = 23 on both trees.
// ===========================================================================
BEZEL_TEST(xmod_imported_record_func_member_invoke_2590,
           single_module_record_field_and_property_invoke_unchanged_9_2) {
    int r = compileAndRun2590(
        "module Single2590T7; "
        "public record Box2590T7 "
        "  public F: (int) -> int := lambda(x: int) => x + 7; "
        "  public property P: (int) -> int := lambda(x: int) => x + 1; "
        "end Box2590T7; "
        "public class Program2590T7 "
        "  public static method Main(): int "
        "    var b := new Box2590T7(); "
        "    return b.F(5) + b.P(10); "
        "  end Main; "
        "end Program2590T7;");
    BEZEL_EXPECT_EQ(r, 23);
}

// ===========================================================================
// T8 — REGRESSION GUARD on the widening's blast radius: an ordinary,
// NON-function-typed field of the same imported record. The widened predicate
// only ever supplies `rt`; the `rt->kind == Function` test still decides
// whether anything dispatches, so an `int` field read must be wholly
// untouched. 42 on both trees. (It is also the #2467 foldable-initializer
// control the issue's own analysis used.)
// ===========================================================================
BEZEL_TEST(xmod_imported_record_func_member_invoke_2590,
           imported_record_non_function_field_read_unaffected_22_8) {
    int r = compileImportAndRun2590(
        "module LibRec2590T8; "
        "public record Box2590T8 "
        "  public N: int := 42; "
        "  public F: (int) -> int := lambda(x: int) => x + 7; "
        "end Box2590T8;",
        "module App2590T8; import LibRec2590T8; "
        "public class Program2590T8 "
        "  public static method Main(): int "
        "    var b := new Box2590T8(); "
        "    return b.N; "
        "  end Main; "
        "end Program2590T8;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 42);
}

}  // namespace bezel::module
