// fix #2639 (§22.3 "Parameter modifiers" / §22.4 "Inline `out` declarations" /
// §2.5 "Dependency resolution"; v0.236) — a `new ImportedT(...)` whose argument
// list carried ANY by-ref modifier emitted NO CONSTRUCTOR CALL AT ALL.
//
// (Revision note: the checkout these tests were written against carries
// v0.237 in its header, but the v0.236->v0.237 diff to
// docs/Bezel_Language_Spec.md is the version line ONLY — the §22.3 and §22.4
// bodies quoted below are byte-identical at both revisions, so the v0.236
// citation the sibling #2629 commit uses stays exact.)
//
// WHAT THE DEFECT WAS. `lowerNew`'s imported-type branch
// (src/codegen/LlvmBackend_Exprs2.cpp, `if (ctx_.isImported(t))`) gates its
// whole body on an `allPositional` predicate that originally read:
//
//     if (!a.name.empty() || !a.modifier.empty() || a.isDiscard)
//         { allPositional = false; break; }
//
// i.e. ANY non-empty argument modifier disqualified the call. Every `ref`,
// `out`, `out var`, `out owned var` and `out using var` argument has one. The
// branch's only other exit is a bare `return obj`, which emits nothing — so for
// those shapes the instance was calloc'd, `bz$T$init` (vtable + field
// initializers) ran, and `bz$T$ctor` was **never called**. Read off
// `BEZEL_DUMP_IR=1` for the issue's own repro, pre-fix `main` contained
//
//     call void @"bz$XLib2629.Widget$init"(ptr %2)
//
// and no `$ctor` call of any kind, while the same class reached through a
// modifier-free `constructor(a: int)` correctly emitted
// `call void @"bz$YLib.Gadget$ctor"(ptr %2, i32 2)`.
//
// The consequence is a SILENT WRONG ANSWER across a module boundary, and it is
// broader than the recipient: because the constructor body never ran, every
// field the constructor assigns also kept its zero/default (T5 pins exactly
// that). §22.3 requires "on normal return the slot's value is copied to the
// caller's recipient"; no call means no normal return and no copy-out, yet the
// program is accepted. The only compile-time signal was an `internal:` #2491
// note, emitted incidentally when the never-written recipient was later read in
// a position that then failed to lower — absent entirely when the recipient is
// a primitive (T3) or is read through a nullable test (T6).
//
// Everything the by-ref case needs was ALREADY implemented inside that branch
// and merely unreachable for a `new`: the per-argument loop reads `formalMod`
// off the imported ConstructorSymbol and takes §22.3's alias path
// (`lvalueAddr`) or its bracket path (`emitAccessorRefArgCopyIn` /
// `emitAccessorRefArgWriteBacks`, #2319), and `declareImportedCtor`
// (LlvmBackend_Setup.cpp) already types a by-ref parameter as a pointer. The
// guard arrived verbatim with the f291057 TU split, before #2283/#2319 added
// that handling, so the two never met.
//
// THE FIX has two halves and BOTH are required — shipping the first alone turns
// this silent wrong value into a SIGSEGV (mechanically confirmed, see below):
//
//   (1) LlvmBackend_Exprs2.cpp — admit `isByRefMod` modifiers to
//       `allPositional`, so the constructor call is emitted at all.
//   (2) LlvmBackend_Calls2.cpp — `ensureCtorOutVarSlots` typed the caller-side
//       inline-`out var` slot ONLY from an `ast::ConstructorDecl`, which an
//       imported constructor does not have (ModuleLoader reconstructs a
//       `sema::ConstructorSymbol` whose `ParameterSymbol`s carry
//       `resolvedDeclType` instead). `slotTy` therefore fell back to its
//       `i32()` default: a 4-byte integer alloca standing in for an 8-byte
//       `string` pointer. With (1) but not (2), the emitted IR is
//
//           %1 = alloca i32, align 4
//           store i32 0, ptr %1, align 4
//           call void @"bz$XLib2629.Widget$ctor"(ptr %2, ptr @.bz.str.49, ptr %1)
//
//       — the imported callee writes 8 bytes through a 4-byte stack slot, and
//       T1's program SIGSEGVs (exit 139) on the PLAIN build. The fix reads the
//       imported ConstructorSymbol's parameter type, which is the same field
//       `declareImportedCtor` uses to build the callee's own signature, so the
//       caller's slot and the callee's formal cannot disagree on width. Post-
//       fix the same IR reads `%1 = alloca ptr, align 8` / `store ptr null`.
//
// ⚠ THE PLAIN BUILD IS THE ORACLE HERE, NOT ASAN. This whole out-var-slot
// family is one where AddressSanitizer misleads (recorded at length on #2629,
// whose defect ASan masked entirely). It does not help on this one either: the
// dependency artifact is built WITHOUT instrumentation (`--sanitize=address` is
// rejected with `-c`), so the callee's over-wide write into the caller's
// under-sized slot is performed by uninstrumented code and ASan does not see
// it — confirmed by probe, the Fix-A-only build reports nothing under ASan and
// SIGSEGVs on the plain build. Every case below was verified red-without /
// green-with on the plain path.
//
// MECHANICAL FAIL->PASS (plain path; `git checkout --` of both source files,
// full rebuild, re-run, then `git apply` of the byte-identical patch):
//
//     case                                              pre-fix   post-fix
//     T1 imported ctor, inline `out var` string          0          5
//     T2 imported ctor, pre-declared `out` string        2          5
//     T3 imported ctor, inline `out var` int             0          7
//     T4 imported ctor, `ref` int                      100        110
//     T5 ctor-assigned FIELD read back                   0          3
//     T6 imported ctor, `out n: string?` := null        41         40
//     T7 control: no modifiers at all                   49         49
//
// T2's pre-fix `2` is why this issue survived: it is the length of the
// recipient's own UNTOUCHED initializer `"zz"`, and #2639's original report
// used a `"hi"`-assigning constructor, so the pre-declared control looked
// CORRECT by coincidence (both strings have length 2) and pointed the
// diagnosis at the inline form specifically. T2 here deliberately picks an
// initializer whose length differs from the constructor's value so the
// coincidence cannot recur.
//
// KNOWN-UNFIXED SIBLING, deliberately not covered here: an imported *METHOD*
// (not constructor) with an inline `out var` recipient is broken too — it
// returns 0 with the same #2491 note, and SIGSEGVs when the recipient is read
// into a local. It lives on a different site (`lowerCall`'s imported-method
// path, not `lowerNew`'s), reproduces identically before and after this fix,
// and is filed separately. Named here so a later reader does not mistake its
// absence for coverage.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile2639 {
    std::string path;
    explicit TempFile2639(std::string p) : path(std::move(p)) {}
    ~TempFile2639() { std::remove(path.c_str()); }
};

void writeSource2639(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// The dependency library: every shape under test lives here so each test pays
// for exactly one library compile. Compiled with `compileObject` (the `-c`
// library-artifact path), then imported by the app below.
const char* kLib2639 =
    "module OutCtorLib2639; "
    "public class W1 "
    "  public ok: bool; "
    "  public constructor(s: string, out n: string) "
    "    n := \"abcde\"; ok := true; "
    "  end; "
    "end W1; "
    "public class W2 "
    "  public v: int; "
    "  public constructor(s: string, out n: int) "
    "    n := 7; v := 3; "
    "  end; "
    "end W2; "
    "public class W3 "
    "  public v: int; "
    "  public constructor(ref n: int) "
    "    n := n + 10; v := 1; "
    "  end; "
    "end W3; "
    "public class W5 "
    "  public v: int; "
    "  public constructor(s: string, out n: string?) "
    "    n := null; v := 4; "
    "  end; "
    "end W5; "
    "public class W6 "
    "  public v: int; "
    "  public constructor(a: int) "
    "    v := a + 40; "
    "  end; "
    "end W6;";

// Compile the library to an object, compile the app importing it, run the app
// in a SEPARATE process (fork-isolated per tests/README.md). Returns the
// child's exit code, -1 on a compile/link/run failure, -2 when the dependency
// itself could not be produced (no toolchain — the caller skips).
int runImported2639(const std::string& mainBody, const std::string& moduleTag) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2639_" + moduleTag + "_" + std::to_string(std::rand());

    TempFile2639 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2639 aObj((dir / (stem + "_a.o")).string());
    writeSource2639(aSrc.path, kLib2639);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    std::string appSrc =
        "module OutCtorApp2639" + moduleTag + "; import OutCtorLib2639; "
        "class C "
        "  public static method Main(): int " + mainBody +
        "  end Main; "
        "end C;";
    TempFile2639 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2639 bExe((dir / (stem + "_b")).string());
    writeSource2639(bSrc.path, appSrc);

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps).success) return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// T1 — #2639's own repro: an IMPORTED constructor with an inline `out var`
// recipient of `string`. §22.3: "on normal return the slot's value is copied
// to the caller's recipient". Pre-fix the constructor was never called, so
// nothing was copied and the read of `n` degraded to 0. This case is also the
// one that SIGSEGVs (exit 139) if the Exprs2 half of the fix lands without the
// Calls2 half — asserting the VALUE, not merely "did not crash", catches both.
// ===========================================================================
BEZEL_TEST(xmod_ctor_out_var_2639,
           imported_ctor_inline_out_var_string_receives_value) {
    int rc = runImported2639(
        "    var a := new W1(\"x\", out var n); "
        "    _ := a; "
        "    return n.Length; ", "T1");
    if (rc == -2) return;  // toolchain unavailable
    BEZEL_EXPECT_EQ(rc, 5);
}

// ===========================================================================
// T2 — the PRE-DECLARED `out n` recipient of the same imported constructor.
// #2639 believed this shape was already correct; it was not — it merely read
// back its own untouched initializer. The initializer's length (2) is chosen
// to DIFFER from the constructor's value (5) precisely so a skipped ctor call
// cannot masquerade as a pass, which is exactly what happened in the original
// report.
// ===========================================================================
BEZEL_TEST(xmod_ctor_out_var_2639,
           imported_ctor_predeclared_out_string_receives_value) {
    int rc = runImported2639(
        "    var pre: string := \"zz\"; "
        "    var b := new W1(\"x\", out pre); "
        "    _ := b; "
        "    return pre.Length; ", "T2");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 5);
}

// ===========================================================================
// T3 — a PRIMITIVE inline `out var` recipient (`out n: int`). Pre-fix this
// returned 0 with NO diagnostic at all (not even the incidental #2491 note),
// because an i32 slot happens to be exactly what the mis-defaulted `slotTy`
// produces — the shape #2639 itself flagged as "may work by luck". It did not:
// no ctor call means no value.
// ===========================================================================
BEZEL_TEST(xmod_ctor_out_var_2639,
           imported_ctor_inline_out_var_int_receives_value) {
    int rc = runImported2639(
        "    var c := new W2(\"x\", out var n); "
        "    _ := c; "
        "    return n; ", "T3");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 7);
}

// ===========================================================================
// T4 — `ref` on an imported constructor. §22.3: "`ref` — pass by reference ...
// The variable must be initialized before the call." Pre-fix the caller's 100
// came back unchanged; the callee's `n := n + 10` never ran. Widens the fix's
// pinned surface past `out`, since one predicate governs every by-ref modifier.
// ===========================================================================
BEZEL_TEST(xmod_ctor_out_var_2639,
           imported_ctor_ref_argument_is_written_back) {
    int rc = runImported2639(
        "    var r: int := 100; "
        "    var d := new W3(ref r); "
        "    _ := d; "
        "    return r; ", "T4");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 110);
}

// ===========================================================================
// T5 — the defect is NOT confined to the recipient. Reads back a field the
// imported constructor assigns (`v := 3`) on an instance constructed through
// an `out var` argument list. Pre-fix: 0 — the whole constructor body was
// skipped, so this pins the actual scope of the wrong answer rather than just
// its most visible symptom.
// ===========================================================================
BEZEL_TEST(xmod_ctor_out_var_2639,
           imported_ctor_with_out_arg_still_runs_its_body) {
    int rc = runImported2639(
        "    var f := new W2(\"x\", out var n); "
        "    _ := n; "
        "    var q := f.v; "
        "    _ := f; "
        "    return q; ", "T5");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 3);
}

// ===========================================================================
// T6 — a NULLABLE reference recipient (`out n: string?`) that the constructor
// assigns `null`. Pre-fix this returned 41, i.e. the recipient tested
// NON-null: #2402's call-site default store was the only thing that ever
// touched the slot and the un-run constructor left whatever that store put
// there, so even the *nullability* answer was wrong. Post-fix the recipient is
// genuinely null (40). Also the control for the fix's OTHER half: `pt` is now
// resolved for an imported ctor, so this shape must still take the call-site
// (not entry-block) store branch that #2402/#2395 established.
// ===========================================================================
BEZEL_TEST(xmod_ctor_out_var_2639,
           imported_ctor_nullable_out_var_observes_assigned_null) {
    int rc = runImported2639(
        "    var g := new W5(\"x\", out var n); "
        "    _ := g; "
        "    if n = null then return 40; end; "
        "    return 41; ", "T6");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 40);
}

// ===========================================================================
// T7 — CONTROL. A modifier-free imported constructor already worked and must
// be untouched: the fix only ADDS by-ref modifiers to `allPositional`, and a
// regression that instead loosened the predicate too far (admitting named args
// or discards, which this positional loop cannot handle) would show up here
// first. Passes identically pre- and post-fix (49, verified).
// ===========================================================================
BEZEL_TEST(xmod_ctor_out_var_2639,
           imported_ctor_without_modifiers_control_unaffected) {
    int rc = runImported2639(
        "    var w := new W6(9); "
        "    var q := w.v; "
        "    _ := w; "
        "    return q; ", "T7");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 49);
}

}  // namespace bezel::module
