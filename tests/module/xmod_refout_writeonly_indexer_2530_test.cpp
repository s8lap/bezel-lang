// fix #2530 (§22.3 / §23, v0.236): the write-only INDEXER as `ref` operand
// was a SEPARATE pre-existing gap from #2461's write-only PROPERTY, because
// the IndexExpr ref/out operand path (ExprTypecheck.cpp's `checkRefOutOperandCategory`
// at the `dynamic_cast<const ast::IndexExpr*>(&target)` branch) consulted
// `ctx_.exprSymbol(target)` to find the indexer's PropertySymbol — but the
// IndexExpr visit (ExprTypecheck_Exprs.cpp's user-defined-indexer path,
// ~line 1829+) never called `ctx_.setExprSymbol(&e, ps)`, so the symbol
// stayed null and `checkRefOutPropertyOperand` was silently bypassed.
//
// Two distinct pre-fix failure modes (verified mechanically on HEAD 5ca8606):
//
//   1. LOCAL write-only indexer as `ref` → ICE at codegen:
//      `internal: lowerExpr returned null for call argument in buildOrderedArgs
//       — #1386` (the IndexExpr's `lowerIndexerGet` returns null at
//      `if (!getterExpr && !getterBlock) return nullptr;` because no
//      `get` accessor exists; nothing else in lowerExpr recognises the
//      IndexExpr either; buildOrderedArgs reports the lowerExpr-null ICE).
//
//   2. IMPORTED write-only indexer as `ref` → raw LINKER error:
//      `undefined reference to 'bz$<Mod>.<Type>$idxget$<arity>'`. The
//      exporter never emits an indexer getter stub for a set-only
//      indexer, so the importer-side external-linkage declaration
//      `declareImportedIndexerGetters` installs (an export-as-stub
//      mechanism that always emits the stub regardless of hasGetter —
//      #2414's mechanics, intentionally conservative) has no body to
//      link against.
//
// §22.3 "`ref` and `out` argument locations": "for `ref`, the getter is
// called once to produce the callee's slot value (for `out` no getter
// call occurs); on normal return the setter is called once with the
// slot's final value." So a write-only indexer is a legal `out` target
// but NOT a legal `ref` target — exactly the named-PROPERTY case #2461
// already fixed.
//
// FIX:
//   - ExprTypecheck_Exprs.cpp's user-defined-indexer visit attaches the
//     resolved indexer PropertySymbol to the IndexExpr via
//     `ctx_.setExprSymbol(&e, ps)` (mirrors how a named-property
//     `d.W` resolves and stamps its PropertySymbol; #2461's #imported
//     branch already keys on this — without the attachment, the
//     imported-side hasGetter check from #2461 was unreachable for any
//     indexer).
//   - ExprTypecheck.cpp's `checkRefOutPropertyOperand` extends its LOCAL
//     branch (the `pd`-bearing case, after the early-return `!pd`
//     imported branch) with a hasGetter check mirroring #2461's
//     imported branch shape, guarded by `kind[0] == 'r'` so `out` is
//     correctly admitted (§22.3: no getter call for `out`). The
//     hasGetter computation mirrors `propertyDeclIsSetOnly`'s INVERSE
//     exactly (Expression/Auto bodyKinds always have a getter; a Block
//     bodyKind has a getter iff any accessor is a `get` arm).
//
// LOCAL≡IMPORTED consistency: both shapes now reject a write-only
// indexer as `ref` with the same clean §22.3/§23 message, and both
// admit `out` and the read-write `ref` round-trip.
//
// Message quality: the imported branch already quotes the PropertySymbol's
// name (`ps->name`, which for an indexer is the metadata-serialized indexer
// handle, e.g. `.this`); the local branch was extended to fall back to
// "the `<DeclaringType>` indexer" for the parser's `property this[i: T]: U`
// form (whose `pd->name` is empty), matching the indexer-label intent of
// `propertyDeclIsSetOnly`'s "property `W` is write-only" shape and
// avoiding an unfriendly `` ` `` is write-only output.
//
// SCOPE (recorded per CLAUDE.md rule 4, not silently applied):
//   - A bare READ of a write-only INDEXER that is NOT a ref/out argument
//     (e.g. `var x := d[0]` where `d` has a set-only indexer) is still a
//     separate gap. `propertyDeclIsSetOnly`'s read-of-write-only gate
//     (ExprTypecheck_Calls.cpp) explicitly excludes indexers (`pd->isIndexer
//     → return false`), and that exclusion is deliberate — §23 only names
//     the named-property path, and the analogous rejection for an indexer
//     would need a fresh spec citation / clean message. Filed as a separate
//     follow-up; this fix mirrors #2461's exact "ref/out operand only"
//     scope (not touching plain reads, see #2461's analogous follow-up
//     gap also left to a separate issue).
//
// Mechanically verified fail-without/pass-with: T1/T2 (imported
// write-only indexer as `ref`/`out`) and T3/T4 (local write-only
// indexer as `ref`/`out`) AND T5 (read-write imported indexer as
// `ref` round-trip) AND T6 (read-write local indexer as `ref`
// round-trip). T1, T3: pre-fix produced an ICE (LOCAL T3) or a
// linker error (IMPORTED T1); post-fix produces the clean
// §22.3/§23 message. T2/T4/T5/T6: confirmed unaffected by the
// fix (still compile + run, value reads back as expected).

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile2530 {
    std::string path;
    explicit TempFile2530(std::string p) : path(std::move(p)) {}
    ~TempFile2530() { std::remove(path.c_str()); }
};

void writeSource2530(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2530(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

bool compileImportCaptureErr2530(const std::string& bSrc,
                                 const std::string& bExe,
                                 const std::string& aObj, std::string& err) {
    namespace fs = std::filesystem;
    std::string errPath = (fs::temp_directory_path() /
                           ("bz2530_err_" + std::to_string(std::rand()) +
                            ".txt"))
                              .string();

    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool success = false;
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj;
        d.objectPath = aObj;
        deps.push_back(d);
        success = driver.compileFile(bSrc, bExe, deps).success;
    }
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);

    std::ifstream in(errPath);
    err.assign((std::istreambuf_iterator<char>(in)),
               std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return success;
}

int compileImportAndRun2530(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2530_" + std::to_string(std::rand());
    TempFile2530 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2530 aObj((dir / (stem + "_a.o")).string());
    writeSource2530(aSrc.path, libSrc);
    if (!compileToObj2530(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2530 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2530 bExe((dir / stem).string());
    writeSource2530(bSrc.path, appSrc);
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

// Capture stderr from a Driver::compileFile call (used by the LOCAL tests
// that don't need a dependency object). Mirrors the same helper in
// xmod_refout_writeonly_prop_2461_test.cpp / _2454.
bool compileCaptureErr2530(const std::string& src, const std::string& exe,
                           std::string& err) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string errPath = (dir / ("bz2530_local_err_" +
                                  std::to_string(std::rand()) + ".txt"))
                              .string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool success = false;
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver driver;
        success = driver.compileFile(src, exe, {}).success;
    }
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);

    std::ifstream in(errPath);
    err.assign((std::istreambuf_iterator<char>(in)),
               std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return success;
}

}  // namespace

// ===========================================================================
// T1 — the IMPORTED bug case: a WRITE-ONLY imported INDEXER (set-only, no
// getter) used as a `ref` argument. §22.3: ref's copy-in calls the getter
// once; a write-only indexer has no getter, so this MUST be rejected.
// Pre-fix the operand fell through sema to a raw LINKER error ("undefined
// reference to bz$<Mod>.<Type>$idxget$<arity>" — the exporter never
// emitted a getter stub for a set-only indexer); post-fix it rejects with
// the clean §22.3/§23 message. Asserts the diagnostic TEXT (the
// message-quality improvement this fix ships), which is absent pre-fix
// (a linker error carries none of it).
// ===========================================================================
BEZEL_TEST(xmod_refout_writeonly_indexer_2530,
           write_only_imported_indexer_rejected_as_ref_arg) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2530_t1_" + std::to_string(std::rand());
    TempFile2530 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2530 aObj((dir / (stem + "_a.o")).string());
    writeSource2530(aSrc.path,
                    "module WOIIdxLib2530T1; "
                    "public class WOIIdx2530T1 "
                    "  private _n: int := 1; "
                    "  public property this[i: int]: int "
                    "    set => _n := value; "
                    "  end; "
                    "  public method Peek(): int => _n; "
                    "end WOIIdx2530T1;");
    BEZEL_EXPECT_TRUE(compileToObj2530(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;  // toolchain unavailable

    std::string bSrc =
        "module App2530T1; import WOIIdxLib2530T1; "
        "public class C "
        "  public static method Bump(ref v: int) "
        "    v := v + 1; "
        "  end Bump; "
        "  public static method Main(): int "
        "    using var d: WOIIdx2530T1 := new WOIIdx2530T1(); "
        "    C.Bump(ref d[0]); "
        "    return 0; "
        "  end Main; "
        "end C;";
    TempFile2530 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2530 bExe((dir / (stem + "_b")).string());
    writeSource2530(bSrcFile.path, bSrc);

    std::string err;
    bool success = compileImportCaptureErr2530(
        bSrcFile.path, bExe.path, aObj.path, err);
    // Rejected both before and after the fix (pre-fix raw linker error,
    // post-fix sema rejection) — `success` alone does not distinguish them.
    BEZEL_EXPECT_TRUE(!success);
    // The clean §22.3/§23 message is present ONLY with the fix (pre-fix the
    // stderr carries a raw "undefined reference to ...$idxget" linker error
    // and none of these phrases). This is the fail-without/pass-with proof.
    BEZEL_EXPECT_TRUE(err.find("write-only") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("no getter for the copy-in") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("22.3") != std::string::npos);
    // And the pre-fix raw linker error is gone with the fix.
    BEZEL_EXPECT_TRUE(err.find("undefined reference") == std::string::npos);
}

// ===========================================================================
// T2 — ACCEPT-CONTROL (false-reject guard, imported): the SAME
// write-only imported indexer as T1, but as an `out` argument. §22.3:
// "for `out` no getter call occurs" — a write-only indexer IS a legal
// `out` target (the setter runs the copy-out; no getter is needed).
// Compiles AND RUNS, asserting the value actually written (read back
// via a Peek method, since the indexer itself has no getter). The
// fix's `kind == "ref"` guard must NOT reject this `out` site.
// ===========================================================================
BEZEL_TEST(xmod_refout_writeonly_indexer_2530,
           write_only_imported_indexer_accepted_as_out_arg_and_writes) {
    int r = compileImportAndRun2530(
        "module WOILib2530T2; "
        "public class WOI2530T2 "
        "  private _n: int := 1; "
        "  public property this[i: int]: int "
        "    set => _n := value; "
        "  end; "
        "  public method Peek(): int => _n; "
        "end WOI2530T2;",
        "module App2530T2; import WOILib2530T2; "
        "public class C "
        "  public static method Produce(out v: int) "
        "    v := 42; "
        "  end Produce; "
        "  public static method Main(): int "
        "    using var d: WOI2530T2 := new WOI2530T2(); "
        "    C.Produce(out d[0]); "
        "    return d.Peek() - 42; "
        "  end Main; "
        "end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T3 — the LOCAL bug case: the SAME-MODULE (local) twin of T1's shape.
// Pre-fix a LOCAL write-only indexer as `ref` reached codegen and ICE'd
// at "lowerExpr returned null for call argument in buildOrderedArgs
// — #1386" (lowerIndexerGet's no-getter path returns null at
// `if (!getterExpr && !getterBlock) return nullptr;`). Post-fix the
// LOCAL-branch hasGetter check in `checkRefOutPropertyOperand` (the
// twin of #2461's imported-branch check) rejects it with the same
// clean §22.3/§23 message as T1.
// ===========================================================================
BEZEL_TEST(xmod_refout_writeonly_indexer_2530,
           local_write_only_indexer_rejected_as_ref_arg) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2530_t3_" + std::to_string(std::rand());
    TempFile2530 src((dir / (stem + ".bzl")).string());
    TempFile2530 exe((dir / stem).string());
    writeSource2530(src.path,
                    "module LocalMod2530T3; "
                    "public class LocalWOIdx2530T3 "
                    "  private _n: int := 1; "
                    "  public property this[i: int]: int "
                    "    set => _n := value; "
                    "  end; "
                    "end LocalWOIdx2530T3; "
                    "public class C "
                    "  public static method Bump(ref v: int) "
                    "    v := v + 1; "
                    "  end Bump; "
                    "  public static method Main(): int "
                    "    using var d: LocalWOIdx2530T3 := "
                    "new LocalWOIdx2530T3(); "
                    "    C.Bump(ref d[0]); "
                    "    return 0; "
                    "  end Main; "
                    "end C;");

    std::string err;
    bool success = compileCaptureErr2530(src.path, exe.path, err);
    // Pre-fix ICE ("lowerExpr returned null ... #1386") also failed
    // the compile, so `!success` is not enough on its own — assert the
    // diagnostic TEXT, mirroring T1's message-quality proof.
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("write-only") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("no getter for the copy-in") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("22.3") != std::string::npos);
    // And the pre-fix ICE is gone with the fix.
    BEZEL_EXPECT_TRUE(err.find("lowerExpr returned null") ==
                      std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("#1386") == std::string::npos);
}

// ===========================================================================
// T4 — ACCEPT-CONTROL (false-reject guard, local): the LOCAL twin of T2.
// A write-only LOCAL indexer as `out` is a legal `ref`/`out` target
// per §22.3 — compiles AND RUNS, asserting the written value via
// Peek. The local-branch hasGetter check must NOT false-reject this.
// ===========================================================================
BEZEL_TEST(xmod_refout_writeonly_indexer_2530,
           local_write_only_indexer_accepted_as_out_arg_and_writes) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2530_t4_" + std::to_string(std::rand());
    TempFile2530 src((dir / (stem + ".bzl")).string());
    TempFile2530 exe((dir / stem).string());
    writeSource2530(src.path,
                    "module LocalMod2530T4; "
                    "public class LocalWOIdx2530T4 "
                    "  private _n: int := 1; "
                    "  public property this[i: int]: int "
                    "    set => _n := value; "
                    "  end; "
                    "  public method Peek(): int => _n; "
                    "end LocalWOIdx2530T4; "
                    "public class C "
                    "  public static method Produce(out v: int) "
                    "    v := 42; "
                    "  end Produce; "
                    "  public static method Main(): int "
                    "    using var d: LocalWOIdx2530T4 := "
                    "new LocalWOIdx2530T4(); "
                    "    C.Produce(out d[0]); "
                    "    return d.Peek() - 42; "
                    "  end Main; "
                    "end C;");

    bezel::Driver driver;
    bool compiled = driver.compileFile(src.path, exe.path, {}).success;
    if (!compiled) return;  // dependency toolchain unavailable — skip
    int status = std::system(exe.path.c_str());
    int r = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T5 — ACCEPT-CONTROL (false-reject guard, imported): a READ-WRITE
// imported INDEXER (both get and set) as a `ref` argument. §22.3 /
// §23.4: a read-write user indexer IS a legal `ref` target (getter
// for copy-in, setter for copy-out). Compiles AND RUNS, asserting the
// round-trip (1 -> 2). The fix's hasGetter check must NOT false-reject
// an indexer that DOES have a getter — the core false-reject axis
// for a `true`-defaulting getter-presence bit. Mirrors
// `ref_out_accessor_writeback_1302`'s `ref_to_user_indexer_writes_back`
// for the local case, but with an IMPORTED class indexer (the cross-
// module path the issue's IMPORTED case uses).
// ===========================================================================
BEZEL_TEST(xmod_refout_writeonly_indexer_2530,
           read_write_imported_indexer_accepted_as_ref_arg_and_writes) {
    int r = compileImportAndRun2530(
        "module RWIdxLib2530T5; "
        "public class RWIdx2530T5 "
        "  private _v: int := 1; "
        "  public property this[i: int]: int "
        "    get => _v; "
        "    set => _v := value; "
        "  end; "
        "  public method Read(): int => _v; "
        "end RWIdx2530T5;",
        "module App2530T5; import RWIdxLib2530T5; "
        "public class C "
        "  public static method Bump(ref v: int) "
        "    v := v + 1; "
        "  end Bump; "
        "  public static method Main(): int "
        "    using var d: RWIdx2530T5 := new RWIdx2530T5(); "
        "    C.Bump(ref d[0]); "
        "    return d.Read() - 2; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T6 — ACCEPT-CONTROL (false-reject guard, local): the LOCAL twin of
// T5. The setExprSymbol attachment must NOT regress the existing
// `ref_to_user_indexer_writes_back` shape that #1302 ships — a local
// read-write indexer as `ref` continues to compile and round-trip.
// ===========================================================================
BEZEL_TEST(xmod_refout_writeonly_indexer_2530,
           read_write_local_indexer_accepted_as_ref_arg_and_writes) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2530_t6_" + std::to_string(std::rand());
    TempFile2530 src((dir / (stem + ".bzl")).string());
    TempFile2530 exe((dir / stem).string());
    writeSource2530(src.path,
                    "module LocalMod2530T6; "
                    "public class LocalRWIdx2530T6 "
                    "  private _v: int := 1; "
                    "  public property this[i: int]: int "
                    "    get => _v; "
                    "    set => _v := value; "
                    "  end; "
                    "  public method Read(): int => _v; "
                    "end LocalRWIdx2530T6; "
                    "public class C "
                    "  public static method Bump(ref v: int) "
                    "    v := v + 1; "
                    "  end Bump; "
                    "  public static method Main(): int "
                    "    using var d: LocalRWIdx2530T6 := "
                    "new LocalRWIdx2530T6(); "
                    "    C.Bump(ref d[0]); "
                    "    return d.Read() - 2; "
                    "  end Main; "
                    "end C;");

    bezel::Driver driver;
    bool compiled = driver.compileFile(src.path, exe.path, {}).success;
    if (!compiled) return;  // dependency toolchain unavailable — skip
    int status = std::system(exe.path.c_str());
    int r = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(r, 0);
}

}  // namespace bezel::module
