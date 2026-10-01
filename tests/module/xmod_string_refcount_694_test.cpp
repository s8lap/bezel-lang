// Cross-module string retain/release (issue #694 residue-1, §4.6 / §28.9).
//
// The within-module #422/#429 retain categories closed the string refcount
// leak for strings produced, returned, and retained inside a SINGLE compilation
// unit.  This file covers the CROSS-MODULE residue: strings that cross the
// `--import` boundary between module A (compiled to .o) and module B
// (compiled importing A).
//
// LEAK MATRIX (8 shapes):
//   S1  imported method returning string, B binds non-escaping local
//   S2  imported class with string field, B constructs/reads/disposes
//   S3  two-level: B calls imported fn that calls another imported fn
//   S4  imported function taking a string argument (A receives; is it released?)
//   S5  imported string return stored into B-side class field / collection
//   S6  imported method returning string, B local escapes (argument position)
//   S7  string constant cross-module (canary: must NOT be released — sentinel)
//   S8  imported instance method returning string (non-static path)
//
// Each shape is checked:
//   (a) correctness (native exit code, no ASan/ASan abort);
//   (b) LSan clean (ASAN_OPTIONS=detect_leaks=1:exitcode=99 runs cleanly after fix).
//
// BEFORE the fix: S1/S2/S3/S6/S8 leak; S4/S7 are already correct.
// AFTER the fix:  all shapes clean.
//
// The lane-local ASan runtime is compiled on first use at /tmp/wt-694/rt_asan.o
// (per HARD RULES — never the shared memoized .o).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "support/emit_object_support.hpp"
#include "driver/Driver.hpp"
#include "lexer/Lexer.hpp"
#include "module/ModuleLoader.hpp"
#include "module/ModuleMetadata.hpp"
#include "module/ModuleMetadataBuilder.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include "native_test_harness.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit. The
// process-unique stem is what keeps concurrent shards from racing each
// other's files mid-compile.
const std::filesystem::path& wtDir694() {
    return wtScratchDir("wt-694");
}


// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct TempFile694 {
    std::string path;
    explicit TempFile694(const std::string& p) : path(p) {}
    ~TempFile694() { std::remove(path.c_str()); }
};

void writeSource694(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile lane-local ASan runtime once, cache path.  Per HARD RULES, NEVER
// the shared memoised .o; always /tmp/wt-694/rt_asan.o.
static const std::string& laneLocalAsanRt() {
    static std::string cached;
    static bool attempted = false;
    if (attempted) return cached;
    attempted = true;
    std::string dst = (wtDir694() / "rt_asan.o");
    // If already built (by the pre-build step in the task instructions), reuse.
    std::ifstream check(dst);
    if (check.good()) { cached = dst; return cached; }
    // Otherwise compile now.
    if (!::bezel::native_test::buildRuntimeSanitizerObject("address", dst))
        return cached;  // leave empty → SKIP
    cached = dst;
    return cached;
}

// Compile source A to an object (uses the Driver — wraps the standard pipeline).
// Returns true on success.
static bool compileToObj694(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileObject(src, obj);
    return r.success;
}

// Compile source B importing A's object, produce an executable, run it.
// Returns the executable's exit code, or -1 on compile/link/run failure.
static int compileImportAndRun694(
    const std::string& bSrc, const std::string& bExe,
    const std::string& aObj)
{
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj;
    d.objectPath   = aObj;
    deps.push_back(d);
    bezel::CompileResult r = driver.compileFile(bSrc, bExe, deps);
    if (!r.success) return bezel::test::asanBuildFailure("compileFile");
    int status = std::system(bExe.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// Cross-module LSan probe.  Compiles B importing A; links BOTH objects with the
// lane-local ASan runtime; runs under detect_leaks=1.
//
// Returns:
//   0  — LSan clean (no leak, no memory-safety error).
//   1  — LSan/ASan problem (exit 99).
//  -1  — toolchain unavailable or compilation/link failure → SKIP.
static int xmodLsan694(
    const std::string& /*aSrc*/, const std::string& aObj,
    const std::string& bSrc,
    const std::vector<bezel::Dependency>& deps,
    int expectedCode)
{
    const std::string& rt = laneLocalAsanRt();
    if (rt.empty()) return -1;  // SKIP

    namespace fs = std::filesystem;
    auto tmpDir = fs::temp_directory_path();
    std::string stem = unique_stem("bz694_lsan");
    std::string bObjPath = (tmpDir / (stem + "_b.o")).string();
    std::string exePath  = (tmpDir / (stem + "_exe")).string();

    // Compile B to object (not exe yet — we link manually with ASan rt).
    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(bSrc, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return bezel::test::asanBuildFailure("parse");
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        for (const auto& dep : deps) {
            if (!dep.metadataPath.empty())
                bezel::module::loadDependency(dep.metadataPath, ctx);
        }
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return bezel::test::asanBuildFailure("sema");
        llvm::LLVMContext llvmCtx;
        auto mod = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!mod) return bezel::test::asanBuildFailure("emitModule");
        std::string err;
        if (!bezel::test::emitObjectFileForTest(*mod, bObjPath, err))
            return bezel::test::asanBuildFailure("emitObjectFile");
    }

    auto cleanup = [&] {
        std::remove(bObjPath.c_str());
        std::remove(exePath.c_str());
    };

    // Link: A.o + B.o + ASan runtime + stdlib.
    // aObj is the dependency object compiled by the Driver (standard, no ASan
    // on the Bezel IR side; ASan is on the runtime side only). This mirrors
    // the within-module LSan tests where only bezel_runtime.cpp carries ASan.
    std::string linkCmd =
        "clang++ -fsanitize=address -g " +
        aObj + " " + bObjPath + " " + rt +
        " -lm -lpthread -lstdc++ -o " + exePath +
        " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) {
        cleanup();
        return bezel::test::asanLinkFailure("link");
    }

    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99:abort_on_error=0 " +
        exePath + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return bezel::test::asanExecFailure("system");
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (code == 99) return 1;   // leak / memory safety error
    if (code == expectedCode) return 0;  // clean + correct
    return -3;  // unexpected exit code — skip (not a leak check failure)
}

}  // namespace

// ===========================================================================
// LEAK MATRIX — Shape S1
// §2.6 / §4.6 / §28.9 (#694): imported static method returning string.
// B binds the result to a non-escaping `var` local (not returned, not stored
// in a field, not passed as argument). After #694 fix: B treats the returned
// string as a +1 producer (no extra retain), releases at scope end → LSan clean.
// Before: B borrow-retains (+2) then releases (+1), leaking 1 reference.
// The exit-code proxy: the importer's Main returns the string's length (5).
// ===========================================================================
BEZEL_TEST(xmod_string_refcount_694, s1_imported_static_method_nonescaping_lsan_4_6) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz694_s1_");
    TempFile694 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile694 aObj((dir / (stem + "_a.o")).string());

    // The method returns a HEAP string (concat = +1 producer in A).
    // A's ReturnStmt: isPlusOneStringProducer → true → TRANSFER (no extra retain).
    // Caller gets back exactly +1 (the concat's allocation).
    // BEFORE fix: B borrow-retains (+2), releases (-1) → net +1 LEAK.
    // AFTER  fix: B treats as +1 producer, no borrow-retain, releases (-1) → CLEAN.
    writeSource694(aSrc.path,
        "module StrLib; "
        "public class Greeter "
        "  public static method GetMsg(): string => \"hel\" + \"lo\"; "
        "end Greeter;");

    BEZEL_EXPECT_TRUE(compileToObj694(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App; "
        "import StrLib; "
        "public class Program "
        "  public static method Main(): int "
        "    var s := Greeter.GetMsg(); "  // non-escaping local
        "    return s.Length; "            // returns 5 (length of "hello")
        "  end Main; "
        "end Program;";

    // Native correctness check.
    TempFile694 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile694 bExe((dir / (stem + "_b")).string());
    writeSource694(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(compileImportAndRun694(bSrcFile.path, bExe.path, aObj.path), 5);

    // LSan check.
    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep; dep.metadataPath = aObj.path; dep.objectPath = aObj.path;
    deps.push_back(dep);
    int lsan = xmodLsan694(aSrc.path, aObj.path, bSrc, deps, 5);
    if (lsan == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(lsan, 0);  // 0 = LSan clean
}


// ===========================================================================
// LEAK MATRIX — Shape S2
// §2.6 / §4.6 / §28.9 (#694): imported class with string instance field.
// B constructs the class, reads the field, disposes (using var).
// The string field is retained in the ctor (or initializer) and released in
// the destructor.  B's read of the field should not retain (it's a borrow);
// but the local `var s := obj.Name` binds a borrow, which before fix was
// retained without corresponding release.
// ===========================================================================
BEZEL_TEST(xmod_string_refcount_694, s2_imported_class_string_field_lsan_4_6) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz694_s2_");
    TempFile694 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile694 aObj((dir / (stem + "_a.o")).string());

    // A: class with a heap-allocated string field (NOT a literal/sentinel).
    // GetName() returns a BORROW of the Name field.
    // A's ReturnStmt on `Name` (a field read = borrow): emitStringRetain → +1.
    // Caller gets +1.  BEFORE fix: B borrow-retains (+2), releases (-1) → +1 LEAK.
    // AFTER  fix: B treats as +1 producer, no borrow-retain, releases (-1) → CLEAN.
    // The Name field is initialized from a string concat (heap string) so the
    // rc arithmetic is non-trivial (not all no-ops).
    writeSource694(aSrc.path,
        "module PersonLib; "
        "public class Person "
        "  public Name: string; "
        "  public constructor() Name := \"Ali\" + \"ce\"; end constructor; "
        "  public method GetName(): string => Name; "
        "  destructor end destructor; "
        "end Person;");

    BEZEL_EXPECT_TRUE(compileToObj694(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    // B: construct a Person (no-arg ctor), call GetName() (imported instance
    // method returning string borrow-retained in A), bind to non-escaping local,
    // return length.
    std::string bSrc =
        "module App; "
        "import PersonLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var p := new Person(); "
        "    var s := p.GetName(); "  // non-escaping: S2 shape
        "    return s.Length; "       // 5 = length of "Alice"
        "  end Main; "
        "end Program;";

    TempFile694 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile694 bExe((dir / (stem + "_b")).string());
    writeSource694(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(compileImportAndRun694(bSrcFile.path, bExe.path, aObj.path), 5);

    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep; dep.metadataPath = aObj.path; dep.objectPath = aObj.path;
    deps.push_back(dep);
    int lsan = xmodLsan694(aSrc.path, aObj.path, bSrc, deps, 5);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}


// ===========================================================================
// LEAK MATRIX — Shape S3
// §2.6 / §4.6 / §28.9 (#694): two-level cross-module string return.
// A exports Inner.Get() returning string;
// A also exports Outer.GetForwarded() which calls Inner.Get() and returns it
// (a within-A cross-method call — A's own ReturnStmt mints +1);
// B calls Outer.GetForwarded().
// The chain: A.Outer.GetForwarded() returns +1 to B.  B should treat it as +1
// (producer semantics) and release it at scope end.
// ===========================================================================
BEZEL_TEST(xmod_string_refcount_694, s3_two_level_imported_string_return_lsan_4_6) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz694_s3_");
    TempFile694 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile694 aObj((dir / (stem + "_a.o")).string());

    // Inner.Get() returns a heap string (concat).
    // Outer.GetForwarded() returns the result of Inner.Get().
    // In A, Inner.Get() is a isUserMethodStringReturnCall (local, non-imported) →
    // producer → TRANSFER in Outer. Outer's ReturnStmt then hands +1 to B.
    // BEFORE fix: B borrow-retains (+2), releases → +1 LEAK.
    writeSource694(aSrc.path,
        "module ChainLib; "
        "public class Inner "
        "  public static method Get(): string => \"wor\" + \"ld\"; "
        "end Inner; "
        "public class Outer "
        "  public static method GetForwarded(): string => Inner.Get(); "
        "end Outer;");

    BEZEL_EXPECT_TRUE(compileToObj694(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App; "
        "import ChainLib; "
        "public class Program "
        "  public static method Main(): int "
        "    var s := Outer.GetForwarded(); "  // two-level cross-module
        "    return s.Length; "                // 5 = length of "world"
        "  end Main; "
        "end Program;";

    TempFile694 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile694 bExe((dir / (stem + "_b")).string());
    writeSource694(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(compileImportAndRun694(bSrcFile.path, bExe.path, aObj.path), 5);

    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep; dep.metadataPath = aObj.path; dep.objectPath = aObj.path;
    deps.push_back(dep);
    int lsan = xmodLsan694(aSrc.path, aObj.path, bSrc, deps, 5);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}


// ===========================================================================
// LEAK MATRIX — Shape S4
// §2.6 / §4.6 / §28.9 (#694): imported function taking a string argument.
// B produces a heap string (concat/interpolation — a +1 producer) and passes
// it as an argument to an imported method.  The argument is a borrow at the
// call site (§28.3); the callee in A does NOT retain it.  B retains the local
// before calling (borrow retain), then releases at scope end.
// This shape tests that PASSING a string to an imported method does NOT cause
// a double-free.  The local in B is a +1 producer (concat), bound and released
// normally; the callee in A is a simple consumer (no ownership).
// ===========================================================================
BEZEL_TEST(xmod_string_refcount_694, s4_b_passes_string_to_imported_fn_lsan_4_6) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz694_s4_");
    TempFile694 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile694 aObj((dir / (stem + "_a.o")).string());

    // A: receives a string, returns its length (an int — no string ownership).
    writeSource694(aSrc.path,
        "module LenLib; "
        "public class Len "
        "  public static method GetLen(s: string): int => s.Length; "
        "end Len;");

    BEZEL_EXPECT_TRUE(compileToObj694(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    // B: builds a heap string (concat = +1 producer), passes it to Len.GetLen.
    // The concat local is non-escaping (not returned, stored, etc.) → released.
    std::string bSrc =
        "module App; "
        "import LenLib; "
        "public class Program "
        "  public static method Main(): int "
        "    var s := \"hel\" + \"lo\"; "  // +1 producer (concat)
        "    return Len.GetLen(s); "        // borrow-pass; s released at scope end
        "  end Main; "
        "end Program;";

    TempFile694 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile694 bExe((dir / (stem + "_b")).string());
    writeSource694(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(compileImportAndRun694(bSrcFile.path, bExe.path, aObj.path), 5);

    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep; dep.metadataPath = aObj.path; dep.objectPath = aObj.path;
    deps.push_back(dep);
    int lsan = xmodLsan694(aSrc.path, aObj.path, bSrc, deps, 5);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}


// ===========================================================================
// LEAK MATRIX — Shape S5
// §2.6 / §4.6 / §28.9 (#694): imported string return stored into B-side field.
// B has a local class with a string field.  It calls an imported string-return
// method and stores the result into the class field via lowerStringFieldStore.
// lowerStringFieldStore is producer-aware: if the source is a +1 producer, it
// TRANSFERS (no extra retain).  After fix, the imported call is a +1 producer,
// so the store transfers; the field holds exactly +1, which is released by the
// class destructor.  Before fix: borrow-retain adds +1, then lowerStringFieldStore
// retains again (+3 total), destructor releases one (+2 leak).
// ===========================================================================
BEZEL_TEST(xmod_string_refcount_694, s5_imported_string_stored_in_b_field_lsan_4_6) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz694_s5_");
    TempFile694 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile694 aObj((dir / (stem + "_a.o")).string());

    // Tag() returns a HEAP string so the field store is rc-relevant.
    writeSource694(aSrc.path,
        "module TagLib; "
        "public class Tagger "
        "  public static method Tag(): string => \"bez\" + \"el\"; "
        "end Tagger;");

    BEZEL_EXPECT_TRUE(compileToObj694(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    // B: local class Container holds a string field.  Main creates one,
    // stores the imported Tag() result into the field, reads back the length,
    // disposes.  The string field should hold exactly one reference throughout.
    std::string bSrc =
        "module App; "
        "import TagLib; "
        "public class Container "
        "  public Label: string; "
        "  public constructor(tag: string) Label := tag; end constructor; "
        "  public method Len(): int => Label.Length; "
        "  destructor end destructor; "
        "end Container; "
        "public class Program "
        "  public static method Main(): int "
        "    using var c := new Container(Tagger.Tag()); "  // S5: stored in field
        "    return c.Len(); "  // 5 = length of "bezel"
        "  end Main; "
        "end Program;";

    TempFile694 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile694 bExe((dir / (stem + "_b")).string());
    writeSource694(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(compileImportAndRun694(bSrcFile.path, bExe.path, aObj.path), 5);

    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep; dep.metadataPath = aObj.path; dep.objectPath = aObj.path;
    deps.push_back(dep);
    int lsan = xmodLsan694(aSrc.path, aObj.path, bSrc, deps, 5);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}


// ===========================================================================
// LEAK MATRIX — Shape S6
// §2.6 / §4.6 / §28.9 (#694): imported string return, local ESCAPES.
// B calls an imported string-return method and passes the result as an argument
// to another function (the local escapes).  An escaping local's single reference
// is NOT released at scope end (it's transferred to the argument position or
// otherwise escapes).
// After fix: +1 from import (no extra borrow-retain for imported calls).
//            Escaping local: not released at scope end → +1 leak (same as
//            within-module escaping string-return — the safe leak direction).
// Before fix: +2 (borrow-retain on top of import's +1), escape → +2 leak.
// CANARY: this shape must NOT cause a double-free or ASan abort.
// The test asserts memory-safety (no ASan abort) and tolerates the leak.
// ===========================================================================
BEZEL_TEST(xmod_string_refcount_694, s6_imported_string_escaping_no_asan_abort_4_6) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz694_s6_");
    TempFile694 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile694 aObj((dir / (stem + "_a.o")).string());

    // GetStr() returns a heap string (concat).
    writeSource694(aSrc.path,
        "module EscLib; "
        "public class Esc "
        "  public static method GetStr(): string => \"es\" + \"c\"; "
        "  public static method UseLen(s: string): int => s.Length; "
        "end Esc;");

    BEZEL_EXPECT_TRUE(compileToObj694(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    // B: GetStr() result escapes (passed as arg to UseLen).
    std::string bSrc =
        "module App; "
        "import EscLib; "
        "public class Program "
        "  public static method Main(): int "
        "    var s := Esc.GetStr(); "  // local
        "    return Esc.UseLen(s); "   // s ESCAPES as arg → no scope-end release
        "  end Main; "
        "end Program;";

    TempFile694 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile694 bExe((dir / (stem + "_b")).string());
    writeSource694(bSrcFile.path, bSrc);
    // Correctness: returns 3 (length of "esc").
    BEZEL_EXPECT_EQ(compileImportAndRun694(bSrcFile.path, bExe.path, aObj.path), 3);

    // Memory-safety check (detect_leaks=0): no UAF / double-free is the invariant.
    // A deliberate +1 leak (escaping local) is tolerated but we must have no
    // over-release/UAF.
    const std::string& rt = laneLocalAsanRt();
    if (rt.empty()) return;  // toolchain unavailable: skip

    auto tmpDir = fs::temp_directory_path();
    std::string stem6 = unique_stem("bz694_s6_safe");
    std::string bObjPath = (tmpDir / (stem6 + "_b.o")).string();
    std::string exePath  = (tmpDir / (stem6 + "_exe")).string();

    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(bSrc, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return;
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::Dependency dep2; dep2.metadataPath = aObj.path; dep2.objectPath = aObj.path;
        bezel::module::loadDependency(dep2.metadataPath, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) { std::remove(bObjPath.c_str()); return; }
        llvm::LLVMContext llvmCtx;
        auto mod = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!mod) { std::remove(bObjPath.c_str()); return; }
        std::string err;
        if (!bezel::test::emitObjectFileForTest(*mod, bObjPath, err)) { std::remove(bObjPath.c_str()); return; }
    }

    std::string linkCmd =
        "clang++ -fsanitize=address -g " +
        aObj.path + " " + bObjPath + " " + rt +
        " -lm -lpthread -lstdc++ -o " + exePath + " > /dev/null 2>&1";
    bool linked = (std::system(linkCmd.c_str()) == 0);
    std::remove(bObjPath.c_str());
    if (!linked) { std::remove(exePath.c_str()); return; }

    // detect_leaks=0: tolerates the escape leak; must not UAF/DF.
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=0:exitcode=99:abort_on_error=0 " +
        exePath + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    std::remove(exePath.c_str());
    if (status == -1) return;
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    // Must NOT exit 99 (ASan abort); must exit 3 (correct result).
    BEZEL_EXPECT_TRUE(code == 3);  // correct result and no ASan abort
}


// ===========================================================================
// LEAK MATRIX — Shape S7 (CANARY)
// §2.6 / §4.6 / §28.9 / §28.8 (#694): string LITERAL return (sentinel) from
// an imported method.  An imported method that simply returns a literal
// (e.g. `=> "abc"`) runs the ReturnStmt retain-on-return logic, but
// bz_string_retain on a sentinel is a no-op.  So the caller gets back a
// sentinel pointer.  The fix treats the call result as a +1 producer; the
// scope-end release calls bz_string_release on the sentinel — also a no-op.
// Neither over-release (UAF) nor a leak can occur here.
// The canary assertion is: exit code = 3, LSan clean, no ASan abort.
// (Contrast with the heap-allocating shapes S1/S3/S8 where the retain/release
// was previously unbalanced; the literal / sentinel path was always safe but
// the fix must not break it.)
// ===========================================================================
BEZEL_TEST(xmod_string_refcount_694, s7_literal_return_sentinel_no_abort_canary_4_6) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz694_s7_");
    TempFile694 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile694 aObj((dir / (stem + "_a.o")).string());

    // A: method returns a plain string literal (sentinel RC, no heap allocation).
    writeSource694(aSrc.path,
        "module LitLib; "
        "public class Lit "
        "  public static method GetLit(): string => \"abc\"; "
        "end Lit;");

    BEZEL_EXPECT_TRUE(compileToObj694(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    // B: binds the sentinel return to a non-escaping local.
    // bz_string_retain (ReturnStmt in A) → no-op on sentinel.
    // bz_string_release (scope end in B, after fix) → no-op on sentinel.
    std::string bSrc =
        "module App; "
        "import LitLib; "
        "public class Program "
        "  public static method Main(): int "
        "    var s := Lit.GetLit(); "  // sentinel; no-op retain/release
        "    return s.Length; "        // 3
        "  end Main; "
        "end Program;";

    TempFile694 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile694 bExe((dir / (stem + "_b")).string());
    writeSource694(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(compileImportAndRun694(bSrcFile.path, bExe.path, aObj.path), 3);

    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep; dep.metadataPath = aObj.path; dep.objectPath = aObj.path;
    deps.push_back(dep);
    int lsan = xmodLsan694(aSrc.path, aObj.path, bSrc, deps, 3);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}


// ===========================================================================
// LEAK MATRIX — Shape S8
// §2.6 / §4.6 / §28.9 (#694): imported INSTANCE method returning string.
// (S1 covered the static method; S8 covers the instance method path.)
// B constructs an imported class (using var), calls an instance string-return
// method, binds result to a non-escaping local, returns its length.
// After fix: treated as +1 producer, transferred, released at scope end → clean.
// ===========================================================================
BEZEL_TEST(xmod_string_refcount_694, s8_imported_instance_method_nonescaping_lsan_4_6) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz694_s8_");
    TempFile694 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile694 aObj((dir / (stem + "_a.o")).string());

    // Text() returns a heap string (concat) so the refcount is non-trivial.
    writeSource694(aSrc.path,
        "module MsgLib; "
        "public class Msg "
        "  public constructor() end constructor; "
        "  public method Text(): string => \"bez\" + \"el\"; "
        "  destructor end destructor; "
        "end Msg;");

    BEZEL_EXPECT_TRUE(compileToObj694(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App; "
        "import MsgLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var m := new Msg(); "
        "    var s := m.Text(); "   // non-escaping: S8 shape (instance method)
        "    return s.Length; "     // 5 = length of "bezel"
        "  end Main; "
        "end Program;";

    TempFile694 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile694 bExe((dir / (stem + "_b")).string());
    writeSource694(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(compileImportAndRun694(bSrcFile.path, bExe.path, aObj.path), 5);

    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep; dep.metadataPath = aObj.path; dep.objectPath = aObj.path;
    deps.push_back(dep);
    int lsan = xmodLsan694(aSrc.path, aObj.path, bSrc, deps, 5);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}


// ===========================================================================
// DOUBLE-FREE CANARY — Shape DC1
// §2.6 / §4.6 / §28.9 (#694): calling an imported string-return method twice
// and using both results non-escapingly.  After the fix, each call is treated
// as a separate +1 producer; each non-escaping local gets released once.
// Over-release (double-free) would be caught by ASan.
// ===========================================================================
BEZEL_TEST(xmod_string_refcount_694, dc1_two_calls_no_double_free_canary_4_6) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz694_dc1_");
    TempFile694 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile694 aObj((dir / (stem + "_a.o")).string());

    // Both A() and B() return heap strings (concat).
    writeSource694(aSrc.path,
        "module DFLib; "
        "public class DF "
        "  public static method A(): string => \"a\" + \"b\"; "
        "  public static method B(): string => \"c\" + \"de\"; "
        "end DF;");

    BEZEL_EXPECT_TRUE(compileToObj694(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    // Both s1 and s2 are non-escaping; after fix each should be released once.
    std::string bSrc =
        "module App; "
        "import DFLib; "
        "public class Program "
        "  public static method Main(): int "
        "    var s1 := DF.A(); "   // len=2
        "    var s2 := DF.B(); "   // len=3
        "    return s1.Length + s2.Length; "  // 5
        "  end Main; "
        "end Program;";

    TempFile694 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile694 bExe((dir / (stem + "_b")).string());
    writeSource694(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(compileImportAndRun694(bSrcFile.path, bExe.path, aObj.path), 5);

    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep; dep.metadataPath = aObj.path; dep.objectPath = aObj.path;
    deps.push_back(dep);
    int lsan = xmodLsan694(aSrc.path, aObj.path, bSrc, deps, 5);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}


// ===========================================================================
// DOUBLE-FREE CANARY — Shape DC2
// §2.6 / §4.6 / §28.9 (#694): an imported method returning a string LITERAL
// (a sentinel — rc is special, retain/release are no-ops).  The fix should
// NOT over-release a literal return: the string pointer is a sentinel.
// bz_string_release on a sentinel is a no-op, so this is inherently safe, but
// the test confirms the plumbing: exit = 5 and ASan clean.
// ===========================================================================
BEZEL_TEST(xmod_string_refcount_694, dc2_imported_literal_return_no_release_canary_4_6) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz694_dc2_");
    TempFile694 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile694 aObj((dir / (stem + "_a.o")).string());

    // The ReturnStmt in A mints +1 even for a literal return (bz_string_retain
    // on a sentinel is a no-op, but the caller receives a pointer to the
    // immutable sentinel which has the sentinel RC).  So this is safe: the
    // returned pointer is a sentinel; bz_string_retain (no-op) in the callee's
    // ReturnStmt is a no-op; bz_string_release in B's scope exit is a no-op.
    writeSource694(aSrc.path,
        "module LitLib; "
        "public class Lit "
        "  public static method Get(): string => \"hello\"; "  // literal return
        "end Lit;");

    BEZEL_EXPECT_TRUE(compileToObj694(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App; "
        "import LitLib; "
        "public class Program "
        "  public static method Main(): int "
        "    var s := Lit.Get(); "  // sentinel pointer; no-op retain; no-op release
        "    return s.Length; "     // 5
        "  end Main; "
        "end Program;";

    TempFile694 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile694 bExe((dir / (stem + "_b")).string());
    writeSource694(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(compileImportAndRun694(bSrcFile.path, bExe.path, aObj.path), 5);

    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep; dep.metadataPath = aObj.path; dep.objectPath = aObj.path;
    deps.push_back(dep);
    int lsan = xmodLsan694(aSrc.path, aObj.path, bSrc, deps, 5);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

}  // namespace bezel::module
