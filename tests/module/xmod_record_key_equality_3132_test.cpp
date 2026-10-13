// Spec-derived regression test for issue #3132 — family F7 of the
// imported-symbol classification seam (umbrella #3152).
//
// §9.1 (positional records synthesize value semantics) / §9.2 / §8.3 (a record
// hashes and compares structurally) / §2.8.3 rule 4 (imported artifacts),
// v0.252. Audit: docs/proposals/papers/imported-symbol-audit-2026-08-13.md, family F7.
//
// ── THE DEFECT ─────────────────────────────────────────────────────────────
//
// `Emitter::recordTypeOfRef` (src/codegen/LlvmBackend_Calls.cpp) is what routes
// a record-typed Dictionary/Set KEY or Array/LinkedList ELEMENT to the
// synthesized §9.1/§9.2 structural hash/equality trampolines. It decided "is
// this positional?" from the AST declaration only:
//
//     bool positional = false;
//     if (auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl))
//         positional = td->hasPositionalFields;
//
// `ModuleLoader` never assigns `Symbol::decl` for an imported type — there is no
// `ast::TypeDecl` to assign, because the importing compilation has no AST for a
// dependency's types at all — so that cast is null for EVERY imported record and
// the predicate answered "no auto equality" across a module boundary, silently.
//
// It is NOT a slow path. `emitRemoveElementEquality` states above its own call
// that the record arm is "mandatory (not an extension)" because an `ICmp` on a
// struct is invalid; the fallback the misclassified record fell into does
// exactly that compare.
//
// ── MEASURED, on a0b655e, with a real two-module build ─────────────────────
//   (`bezelc -c dep.bzl -o dep.o` then `bezelc --import dep.o -o app app.bzl`)
//
//   T1/T2  `record PtR(X: int, Y: int)` IMPORTED, used as a Dictionary key and
//          as an Array element:
//            debug build          bezelc ABORTED — assertion
//                                 `ICmpInst::AssertOK` … "Invalid operand types
//                                 for ICmp instruction" (exit 134, core dumped)
//            RelWithDebInfo       the assert is compiled out, so the malformed
//                                 compare reaches the verifier: "bezelc: codegen
//                                 error: IR verification failed (malformed IR
//                                 emitted)". This is the shape the GATE sees.
//          Either way: no executable, no diagnostic naming a language rule.
//   T3     the SAME program with `PtR` declared LOCALLY — compiled, ran, exit 0.
//
// Mechanically proven on this branch (RelWithDebInfo, `agent_check.sh --area
// module xmod_record_key_equality_3132`): with the one-line fix reverted,
// 1 passed / 2 failed (T1 and T2 red, T3 green); with it restored, 3 passed.
//
// T3 is the point of the file: the two programs are otherwise identical, so what
// broke is not the shape but the MODULE BOUNDARY. It is also the differential
// control — it passed before the fix and must keep passing, which pins that
// reading `hasPositionalFields` did not change the local answer.
//
// The fix routes the site through `Emitter::recordIsAutoBearing`, the
// both-sources predicate #2934's follow-up introduced for the two §8.5 rendering
// sites (`hasPositionalFields` first, AST fallback, then §9.2's body-record
// conditions) — one predicate, every caller.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile3132 {
    std::string path;
    explicit TempFile3132(std::string p) : path(std::move(p)) {}
    ~TempFile3132() { std::remove(path.c_str()); }
};

void writeSource3132(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// -2 = the dependency toolchain is unavailable (test skips); -1 = the importer
// did not build or did not exit normally; otherwise the program's exit code.
int compileImportAndRun3132(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3132_");
    TempFile3132 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3132 aObj((dir / (stem + "_a.o")).string());
    writeSource3132(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile3132 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3132 bExe((dir / stem).string());
    writeSource3132(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
             .success)
        return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// The single-module build of one source — for the differential control.
int compileAndRun3132(const std::string& src) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3132l_");
    TempFile3132 aSrc((dir / (stem + ".bzl")).string());
    TempFile3132 aExe((dir / stem).string());
    writeSource3132(aSrc.path, src);
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, aSrc.path, aExe.path).success)
        return -1;
    int status = std::system(aExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// §9.1: a POSITIONAL record — the package that ALWAYS synthesizes §8.3
// structural hashing and equality, whatever module it is declared in.
const char* kDep3132 =
    "module DepF7_3132; "
    "public record PtR3132(X: int, Y: int); ";

}  // namespace

// ===========================================================================
// T1 — an imported positional record as a DICTIONARY KEY. §8.3: two
// structurally-equal records hash and compare equal, so a lookup built from a
// fresh `new PtR3132(1, 2)` must find the entry stored under an equal key.
// Pre-fix: the importer's compile ABORTED (invalid ICmp on the record struct).
// // per §8.3 / §9.1 / §2.8.3 rule 4
// ===========================================================================
BEZEL_TEST(xmod_record_key_equality_3132,
           imported_positional_record_dictionary_key_is_structural) {
    int r = compileImportAndRun3132(
        kDep3132,
        "module AppF7_3132a; import DepF7_3132; "
        "public class Program "
        "  public static method Main(): int "
        "    using var d := new Dictionary<PtR3132, int>(); "
        "    d.Add(new PtR3132(1, 2), 42); "
        "    if not d.ContainsKey(new PtR3132(1, 2)) then return 1; end if; "
        "    if d[new PtR3132(1, 2)] <> 42 then return 2; end if; "
        "    if d.ContainsKey(new PtR3132(1, 3)) then return 3; end if; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T2 — the second consumer, a DIFFERENT emitter path: `Array<T>.Remove` reaches
// `emitRemoveElementEquality`, whose record arm its own comment calls
// "mandatory (not an extension)". A fix that repaired only the dictionary
// trampolines would leave this red.
// Pre-fix: same abort. // per §8.3 / §9.1
// ===========================================================================
BEZEL_TEST(xmod_record_key_equality_3132,
           imported_positional_record_array_remove_is_structural) {
    int r = compileImportAndRun3132(
        kDep3132,
        "module AppF7_3132b; import DepF7_3132; "
        "public class Program "
        "  public static method Main(): int "
        "    using var a := new Array<PtR3132>(); "
        "    a.Add(new PtR3132(3, 4)); "
        "    a.Add(new PtR3132(5, 6)); "
        "    if not a.Remove(new PtR3132(3, 4)) then return 1; end if; "
        "    if a.Count <> 1 then return 2; end if; "
        "    if a.Remove(new PtR3132(9, 9)) then return 3; end if; "
        "    if not a.Contains(new PtR3132(5, 6)) then return 4; end if; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// ★ THE DIFFERENTIAL CONTROL. T1's and T2's programs with `PtR3132` declared in
// the SAME module. Already clean before the fix — the AST cast succeeds for a
// local symbol — so it pins that the module boundary was the whole difference
// and that the both-sources predicate did not change the local answer.
// // per §8.3 / §9.1
// ===========================================================================
BEZEL_TEST(xmod_record_key_equality_3132,
           local_positional_record_key_still_structural_control) {
    int r = compileAndRun3132(
        "module LocalF7_3132; "
        "public record PtR3132L(X: int, Y: int); "
        "public class Program "
        "  public static method Main(): int "
        "    using var d := new Dictionary<PtR3132L, int>(); "
        "    d.Add(new PtR3132L(1, 2), 42); "
        "    if not d.ContainsKey(new PtR3132L(1, 2)) then return 1; end if; "
        "    if d[new PtR3132L(1, 2)] <> 42 then return 2; end if; "
        "    using var a := new Array<PtR3132L>(); "
        "    a.Add(new PtR3132L(3, 4)); "
        "    if not a.Remove(new PtR3132L(3, 4)) then return 3; end if; "
        "    if a.Count <> 0 then return 4; end if; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_EQ(r, 0);
}

}  // namespace bezel::module
