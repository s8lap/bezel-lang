// fix #3748 (§22.8 / §2.8.2 / §18.5, v0.286) — a non-constant string `+`
// field initializer on an imported GENERIC class reached the importer in two
// adjacent failure modes measured on the pre-fix HEAD (85147dc):
//
//   Row 1 (MALFORMED IR): `b := H.Str() + H.Str()` (both non-constant) parsed
//     and sema-checked cleanly, the load-time reconstruction successfully
//     re-attached the BinaryExpr as the field initializer, and codegen
//     produced `add ptr %8, %11` — an LLVM-verifier abort
//     ("Integer arithmetic operators only work with integral types!").
//
//   Row 2 (false REJECT): `b := "v" + H.Str()` (one constant, one
//     non-constant) tripped the #2860 construction-site §2.8.2 reject: the
//     load-time `parseAndAttachFieldInit` silently failed to give the
//     `H.Str()` CallExpr an exprType (the imported method's
//     `importedReturnType` was not published into the throwaway SemaContext
//     before pass 3 ran), so the `+` typecheck left the BinaryExpr
//     untyped, the reconstruction fell back to the constant-fold-only
//     branch, the field init was deemed non-foldable, and the construction
//     site got "constructing imported generic `G<...>` cannot run the
//     initializer of its field `b` (`:= "v" + H.Str()`)…". The same
//     initializer written in ONE module is fine; the same initializer
//     written in a sibling module importing that generic was REJECTED — a
//     §2.8.2 clause-2 "external instantiation behaves identically to a
//     same-module instantiation" violation.
//
// ROOT CAUSE (#3748's actual measured defect, NOT the issue's hypothesis).
// `parseAndAttachFieldInit` builds a throwaway `SemaContext`, re-parses
// the synthetic class source, and typechecks the re-parsed initializer
// against that throwaway context. The call to `H.Str()` resolves to the
// IMPORTED `H.Str` method (the sibling TypeSymbol was registered into the
// throwaway via `registerImportedType`), so the typecheck arm that gives
// the CallExpr its exprType — `!md && ctx_.isImported(ms)` in
// ExprTypecheck_Calls.cpp — keys on the throwaway's `isImported`
// predicate AND on `importedReturnType(ms)`. Both were empty in the
// throwaway: `isImported` did not have `ms` in its set (it is the SAME
// object the mainCtx has, but the throwaway's set is private to it), and
// `importedReturnType(ms)` was not populated because the loader's
// `returnTypes_ → setImportedReturnType` publish block ran AFTER pass 3
// (the `parseAndAttachFieldInit` loop). Both gaps closed in this fix:
// (a) `setImportedReturnType` / `setImportedMemberType` lifted to run
//     before pass 3 (mirroring the `resolvePendingNamed` placement);
// (b) `parseAndAttachFieldInit` carries the type channels for every
//     sibling member across into the throwaway AND inserts each member
//     into the throwaway's `imported_` set via a new `markImported` API.
//
// The two rows collapse to the same outcome post-fix: both replay to the
// exporter's declared value (`"[s42s42]"` / `"[vs42]"`). The control rows
// (T3 const-folded `+`, T4 single non-constant call, T5 interpolation in
// `+`, T6 500-iter ASan/LSan amplification on T1+T2) pin the
// non-regression surface and the no-leak invariant.
//
// The family lands whole (LANE_RULES §6b): both rows below are the
// CLASS-side `emitOwnFieldInit` consumer, the same path the #2874 / #3724
// / #3736 fixes already pinned. No record twin is required because the
// record-side `buildRecordValue` consumer is the same load-time
// reconstruction (`parseAndAttachFieldInit`) feeding it.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/Diagnostic.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

void writeSource3748(const fs::path& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile the LIBRARY module to an object with embedded `.bezel.meta` (§2.4).
bool buildLib3748(const fs::path& dir, const std::string& stem,
                  const std::string& libSrc, std::string& objOut) {
    fs::path src = dir / (stem + "_a.bzl");
    objOut = (dir / (stem + "_a.o")).string();
    writeSource3748(src, libSrc);
    bezel::Driver driver;
    bool ok = driver.compileObject(src.string(), objOut).success;
    return ok && std::ifstream(objOut).good();
}

// Same RunOutcome shape as the sibling #3724 test: an AOT exit status
// truncates to 8 bits, so the observable — the replayed STRING — travels
// on stdout, never in the exit code.
struct RunOutcome3748 {
    bool compiled = false;
    bool ran = false;       // exited normally (WIFEXITED)
    int exitCode = -1;
    std::string out;        // captured stdout
};

RunOutcome3748 compileImportAndRun3748(
        const fs::path& dir, const std::string& stem, const std::string& bSrcText,
        const std::string& aObj,
        const codegen::CodegenOptions& opts = codegen::CodegenOptions{},
        const std::string& envPrefix = "") {
    RunOutcome3748 r;
    fs::path bSrc = dir / (stem + "_b.bzl");
    fs::path bExe = dir / (stem + "_b");
    fs::path bOut = dir / (stem + "_b.out");
    writeSource3748(bSrc, bSrcText);

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj;
    d.objectPath = aObj;
    deps.push_back(d);
    r.compiled = driver.compileFile(bSrc.string(), bExe.string(), deps, opts)
                     .success;
    if (!r.compiled) return r;

    std::string cmd = envPrefix + bExe.string() + " > " + bOut.string() +
                      " 2>/dev/null";
    int status = std::system(cmd.c_str());
    r.ran = (status != -1) && WIFEXITED(status);
    if (r.ran) r.exitCode = WEXITSTATUS(status);
    std::ifstream in(bOut);
    std::ostringstream ss;
    ss << in.rdbuf();
    r.out = ss.str();
    return r;
}

// App-source helper — same shape as the #3724 sibling, pointed at the
// import of the #3748 library.
std::string appSrc3748(const std::string& moduleName, const std::string& body) {
    return "module " + moduleName + ";\n"
           "import Lib3748;\n"
           "public class C\n"
           "  public static method Main(): int\n" + body +
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

// ===========================================================================
// T1 — ROW 1, the MALFORMED IR shape: `H.Str() + H.Str()` (both
// non-constant). Pre-fix this built and emitted `add ptr %8, %11` and
// failed the LLVM verifier. Post-fix it compiles and `g.b` reads the
// exporter's declared "s42s42".
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_string_plus_field_init_3748,
           t1_string_plus_string_field_replays_s42s42_22_8_2_8_2) {
    const auto& dir = wtScratchDir("bz3748");
    std::string stem = unique_stem("bz3748_t1_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3748(dir, stem,
        "module Lib3748; "
        "public class H3748 "
        "  public static method Get(): int => 42; "
        "  public static method Str(): string => \"s42\"; "
        "end H3748; "
        "public class G3748<T> "
        "  public v: T; "
        "  public b: string := H3748.Str() + H3748.Str(); "
        "  public constructor(v0: T) v := v0; end constructor; "
        "end G3748;",
        aObj));

    auto r = compileImportAndRun3748(dir, stem,
        appSrc3748("App3748T1",
                   "    var g := new G3748<int>(9);\n"
                   "    Console.WriteLine(\"[\" + g.b + \"]\");\n"
                   "    return 0;\n"),
        aObj);
    BEZEL_EXPECT_TRUE(r.compiled);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("[s42s42]\n"));
}

// ===========================================================================
// T2 — ROW 2, the §2.8.2 false REJECT shape: `"v" + H.Str()` (one
// constant, one non-constant). Pre-fix this was a LOUD construction-site
// reject ("constructing imported generic G<...> cannot run the
// initializer of its field b (`:= "v" + H.Str()`)…"). Post-fix it
// compiles and reads the exporter's declared "vs42".
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_string_plus_field_init_3748,
           t2_string_literal_plus_call_field_replays_vs42_22_8_2_8_2) {
    const auto& dir = wtScratchDir("bz3748");
    std::string stem = unique_stem("bz3748_t2_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3748(dir, stem,
        "module Lib3748; "
        "public class H3748 "
        "  public static method Get(): int => 42; "
        "  public static method Str(): string => \"s42\"; "
        "end H3748; "
        "public class G3748<T> "
        "  public v: T; "
        "  public b: string := \"v\" + H3748.Str(); "
        "  public constructor(v0: T) v := v0; end constructor; "
        "end G3748;",
        aObj));

    auto r = compileImportAndRun3748(dir, stem,
        appSrc3748("App3748T2",
                   "    var g := new G3748<int>(9);\n"
                   "    Console.WriteLine(\"[\" + g.b + \"]\");\n"
                   "    return 0;\n"),
        aObj);
    BEZEL_EXPECT_TRUE(r.compiled);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("[vs42]\n"));
}

// ===========================================================================
// T3 — CONTROL: the constant-folded shape `b := "v" + "w"`, which has
// never been broken (the constant-fold replay path is the one the
// pre-#2874 carriage supported and the #2874 load-time path does not
// need to fall back to). Must be UNCHANGED by this fix.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_string_plus_field_init_3748,
           t3_constant_folded_string_concat_field_unchanged_control) {
    const auto& dir = wtScratchDir("bz3748");
    std::string stem = unique_stem("bz3748_t3_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3748(dir, stem,
        "module Lib3748; "
        "public class G3748<T> "
        "  public v: T; "
        "  public b: string := \"v\" + \"w\"; "
        "  public constructor(v0: T) v := v0; end constructor; "
        "end G3748;",
        aObj));

    auto r = compileImportAndRun3748(dir, stem,
        appSrc3748("App3748T3",
                   "    var g := new G3748<int>(9);\n"
                   "    Console.WriteLine(\"[\" + g.b + \"]\");\n"
                   "    return 0;\n"),
        aObj);
    BEZEL_EXPECT_TRUE(r.compiled);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("[vw]\n"));
}

// ===========================================================================
// T4 — CONTROL: a single non-constant call `b := H.Str()`, the
// canonical #2874 replay shape. Already worked before this fix; the fix
// must not break it.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_string_plus_field_init_3748,
           t4_single_non_constant_call_field_unchanged_control) {
    const auto& dir = wtScratchDir("bz3748");
    std::string stem = unique_stem("bz3748_t4_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3748(dir, stem,
        "module Lib3748; "
        "public class H3748 "
        "  public static method Str(): string => \"s42\"; "
        "end H3748; "
        "public class G3748<T> "
        "  public v: T; "
        "  public b: string := H3748.Str(); "
        "  public constructor(v0: T) v := v0; end constructor; "
        "end G3748;",
        aObj));

    auto r = compileImportAndRun3748(dir, stem,
        appSrc3748("App3748T4",
                   "    var g := new G3748<int>(9);\n"
                   "    Console.WriteLine(\"[\" + g.b + \"]\");\n"
                   "    return 0;\n"),
        aObj);
    BEZEL_EXPECT_TRUE(r.compiled);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("[s42]\n"));
}

// ===========================================================================
// T5 — CONTROL: the INTERPOLATION-in-binary `+` shape `b := $"v{H.Get()}" +
// ""`, which the #3724 fix's T7 row measured as already correct BEFORE
// that fix. The fix #3748 must leave this row UNCHANGED — the
// interpolation is wrapped in a `+`, the enclosing BinaryExpr gets its
// span from `parsePrimaryExpression`'s `stampSpan` wrapper, and the
// load-time reconstruction reaches the interpolation. Pinning the
// behavior here is the evidence that #3748's change to the sibling
// type-channel carriage does not regress the #3724 fix.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_string_plus_field_init_3748,
           t5_interpolation_in_binary_plus_unchanged_18_5_4_6) {
    const auto& dir = wtScratchDir("bz3748");
    std::string stem = unique_stem("bz3748_t5_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3748(dir, stem,
        "module Lib3748; "
        "public class H3748 "
        "  public static method Get(): int => 42; "
        "end H3748; "
        "public class G3748<T> "
        "  public v: T; "
        "  public b: string := $\"v{H3748.Get()}\" + \"\"; "
        "  public constructor(v0: T) v := v0; end constructor; "
        "end G3748;",
        aObj));

    auto r = compileImportAndRun3748(dir, stem,
        appSrc3748("App3748T5",
                   "    var g := new G3748<int>(9);\n"
                   "    Console.WriteLine(\"[\" + g.b + \"]\");\n"
                   "    return 0;\n"),
        aObj);
    BEZEL_EXPECT_TRUE(r.compiled);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("[v42]\n"));
}

// ===========================================================================
// T6 — 500-iteration ASan/LSan amplification on BOTH the row 1 and row 2
// shapes. Per LANE_RULES §3 / [[lsan-one-shot-leak-tests-pass-vacuously]]:
// a one-shot leak row passes on both sides — leak residue below the
// detection floor. Amplify to 500 iterations so any +1 retention per
// construction accumulates past that floor. `total` is checked so a
// "clean because nothing ran" outcome cannot pass.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_string_plus_field_init_3748,
           t6_500x_amplification_no_leak_no_double_free_28_2) {
    const auto& dir = wtScratchDir("bz3748");
    std::string stem = unique_stem("bz3748_t6_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3748(dir, stem,
        "module Lib3748; "
        "public class H3748 "
        "  public static method Get(): int => 42; "
        "  public static method Str(): string => \"s42\"; "
        "end H3748; "
        "public class GA3748<T> "
        "  public v: T; "
        "  public b: string := H3748.Str() + H3748.Str(); "
        "  public constructor(v0: T) v := v0; end constructor; "
        "end GA3748; "
        "public class GB3748<T> "
        "  public v: T; "
        "  public b: string := \"v\" + H3748.Str(); "
        "  public constructor(v0: T) v := v0; end constructor; "
        "end GB3748;",
        aObj));

    codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;

    auto r = compileImportAndRun3748(dir, stem,
        appSrc3748("App3748T6",
                   "    var total := 0;\n"
                   "    for var i in 0..<500 do\n"
                   "      var a := new GA3748<int>(i);\n"
                   "      total := total + a.b.Length;\n"
                   "      var b := new GB3748<int>(i);\n"
                   "      total := total + b.b.Length;\n"
                   "    end for;\n"
                   "    Console.WriteLine(total.ToString());\n"
                   "    return 0;\n"),
        aObj, opts,
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99:abort_on_error=0 ");
    BEZEL_EXPECT_TRUE(r.compiled);
    // A sanitizer report exits 99; an untrapped crash makes `ran` false.
    // Both are failures, and so is a clean exit that printed the wrong
    // total.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // 500 iterations x 2 shapes. String.Length is a CODE-POINT count
    // (std §4.6, O(1)) — "s42s42" is 6 ASCII code points, "vs42" is 4 →
    // 500 * (6 + 4) == 5000.
    BEZEL_EXPECT_EQ(r.out, std::string("5000\n"));
}

}  // namespace bezel::module
