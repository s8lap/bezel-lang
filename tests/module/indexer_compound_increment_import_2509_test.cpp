// fix #2509 (§9.2/§23.4, v0.236): the IMPORTED-indexer compound-assign and
// increment/decrement paths — the cross-module twin of the just-landed #2507
// (LOCAL record indexer `r[i] += v` / `r[i]++`, tests/codegen/
// indexer_compound_increment_local_2507_test.cpp). Filed by the #2507 lane
// while probing that fix's own "unconfirmed but plausible by code-shape"
// flag on the imported case: confirmed by direct probe that the imported
// case is NOT affected by #2507's copy-vs-address crash at all — both
// `Emitter::lowerIndexerCompound` (LlvmBackend_Assign.cpp, `r[i] += v`)
// and the IncrementStmt indexer path (LlvmBackend_Stmts.cpp, `r[i]++`)
// required an `ast::PropertyDecl*` to even attempt the read-modify-write
// (`if (!pd) return false;` / `if (ps && pd) {...}`), and an IMPORTED
// indexer never has one (reconstructed from side tables only, per #2414's
// own comment on `lowerIndexerGet`). So both functions bailed out early
// for an imported receiver, the caller fell through
// `lowerCollectionIndexerCompound` (false: not Array<T>/Dictionary<K,V>)
// and the generic `lvalueAddr` fallback (null: no LLVM ArrayType for a
// user indexer), and the write was SILENTLY DROPPED — no crash, no
// diagnostic. Confirmed by direct probe against origin/main before any
// src/ change: `r[5] += 3; return r[2];` against an imported RECORD
// indexer returned 2 (the pristine getter value `_v+2 = 0+2`), not the
// expected 5; identical exit code against an imported CLASS indexer —
// #2489's own Class||Record scope (the same `recvDecl->kind != Class`
// gate, just at a different call site). Same silent-no-op shape for
// `r[5]++` against either kind.
//
// Fix mirrors #2489's `!pd` branches in `lowerIndexerGet`/`lowerIndexerSet`
// (this file's sibling xmod_record_property_setter_indexer_import_2489_test
// covers those): added a `!pd` branch to both `lowerIndexerCompound` and
// the IncrementStmt indexer path that dispatches directly through the
// exported `bz$<Class>$idxget$<arity>` / `bz$<Class>$idxset$<arity>` ABI
// stubs (importedIndexerGetterFns_ / importedIndexerSetterFns_), resolving
// the receiver's OWN storage address via `lvalueAddr` (mirrors #2507's
// local fix and #2489's setter fix — a record receiver is by-value per
// §9.2, so `lowerExpr` loads a copy and the setter would mutate a
// throwaway slot) and evaluating the receiver and each index expression
// ONCE, sharing them between the getter and setter calls (§23.4's explicit
// single-evaluation contract — the issue text rules out delegating through
// `lowerIndexerGet`/`lowerIndexerSet`, which would re-evaluate; that is
// filed as a #2510 refactor, not a fix).
//
// CONSTANTS kept < 256 (runAot/child-process exit status is
// WEXITSTATUS-masked to 8 bits, per the lane protocol and #2489's own
// file-header note) and ADDITION/SUBTRACTION only (never multiplication,
// per the same guidance) so a stale/crashed read (-999) or a silently
// dropped write (the pristine getter value) could never coincidentally
// match a correct write's result.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile2509 {
    std::string path;
    explicit TempFile2509(std::string p) : path(std::move(p)) {}
    ~TempFile2509() { std::remove(path.c_str()); }
};

void writeSource2509(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2509(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Mirrors #2414/#2474/#2478/#2489's own compileImportAndRun helpers.
int compileImportAndRun2509(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2509_");
    TempFile2509 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2509 aObj((dir / (stem + "_a.o")).string());
    writeSource2509(aSrc.path, libSrc);
    if (!compileToObj2509(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2509 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2509 bExe((dir / stem).string());
    writeSource2509(bSrc.path, appSrc);
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
// T1 — CORE repro: imported RECORD indexer `r[i] += v`, PUBLIC backing
// field combined with a positional one (§9.2/§9.3). Pre-fix: 2 (the silent
// no-op — `_v` never left its initial 0, `r[2]` read the pristine
// `_v+2 = 0+2`). Post-fix: 5 (`r[5] += 3`: get `_v+5 = 5`, new value `5+3=8`,
// set `_v := 8-5 = 3`; `r[2]` reads `_v+2 = 3+2 = 5`).
// ===========================================================================
BEZEL_TEST(indexer_compound_increment_import_2509,
           record_compound_add_public_backing) {
    const char* lib =
        "module RecIdxLib2509T1; "
        "public record IdxRec2509T1(seed: int) "
        "  public _v: int := 0; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end IdxRec2509T1;";
    int r = compileImportAndRun2509(
        lib,
        "module App2509T1; import RecIdxLib2509T1; "
        "public class C2509T1 "
        "  public static method Main(): int "
        "    var r := new IdxRec2509T1(9); "
        "    r[5] += 3; "
        "    return r[2]; "
        "  end Main; "
        "end C2509T1;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 5);
}

// ===========================================================================
// T2 — imported RECORD `r[i]++` (the IncrementStmt path, distinct from T1's
// compound-assign path). Pre-fix: 2 (silent no-op). Post-fix: 3 (`r[5]++`:
// get `5`, new `5+1=6`, set `_v := 6-5 = 1`; `r[2]` reads `1+2 = 3`).
// ===========================================================================
BEZEL_TEST(indexer_compound_increment_import_2509, record_increment) {
    const char* lib =
        "module RecIdxLib2509T2; "
        "public record IdxRec2509T2(seed: int) "
        "  public _v: int := 0; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end IdxRec2509T2;";
    int r = compileImportAndRun2509(
        lib,
        "module App2509T2; import RecIdxLib2509T2; "
        "public class C2509T2 "
        "  public static method Main(): int "
        "    var r := new IdxRec2509T2(9); "
        "    r[5]++; "
        "    return r[2]; "
        "  end Main; "
        "end C2509T2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 3);
}

// ===========================================================================
// T3 — imported RECORD `r[i] -= v` axis, distinct constants (subtraction
// path, mirrors T1's get-modify-set over the same exported ABI). Pre-fix: 2
// (silent no-op). Post-fix: 6 (`r[4] -= 1`: get `_v+4 = 4`, new `4-1=3`,
// set `_v := 3-4 = -1`; `r[7]` reads `-1+7 = 6`).
// ===========================================================================
BEZEL_TEST(indexer_compound_increment_import_2509, record_compound_sub) {
    const char* lib =
        "module RecIdxLib2509T3; "
        "public record IdxRec2509T3(seed: int) "
        "  public _v: int := 0; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end IdxRec2509T3;";
    int r = compileImportAndRun2509(
        lib,
        "module App2509T3; import RecIdxLib2509T3; "
        "public class C2509T3 "
        "  public static method Main(): int "
        "    var r := new IdxRec2509T3(9); "
        "    r[4] -= 1; "
        "    return r[7]; "
        "  end Main; "
        "end C2509T3;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 6);
}

// ===========================================================================
// T4 — imported RECORD `r[i]--` axis. Pre-fix: 2 (silent no-op). Post-fix: 7
// (`r[3]--`: get `_v+3 = 3`, new `3-1=2`, set `_v := 2-3 = -1`; `r[8]` reads
// `-1+8 = 7`).
// ===========================================================================
BEZEL_TEST(indexer_compound_increment_import_2509, record_decrement) {
    const char* lib =
        "module RecIdxLib2509T4; "
        "public record IdxRec2509T4(seed: int) "
        "  public _v: int := 0; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end IdxRec2509T4;";
    int r = compileImportAndRun2509(
        lib,
        "module App2509T4; import RecIdxLib2509T4; "
        "public class C2509T4 "
        "  public static method Main(): int "
        "    var r := new IdxRec2509T4(9); "
        "    r[3]--; "
        "    return r[8]; "
        "  end Main; "
        "end C2509T4;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 7);
}

// ===========================================================================
// T5 — imported CLASS `r[i] += v` axis. Same defect shape and same
// `Class || Record` scope as #2489 (the gate was a kind-agnostic
// `ast::PropertyDecl*` null check, not a record-only gate). Pre-fix: 2
// (silent no-op — confirmed by probe against origin/main before any src/
// change). Post-fix: 5 (same arithmetic as T1 over the class ABI stub).
// ===========================================================================
BEZEL_TEST(indexer_compound_increment_import_2509, class_compound_add) {
    const char* lib =
        "module ClsIdxLib2509T5; "
        "public class ClsIdx2509T5 "
        "  private _v: int := 0; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end ClsIdx2509T5;";
    int r = compileImportAndRun2509(
        lib,
        "module App2509T5; import ClsIdxLib2509T5; "
        "public class C2509T5 "
        "  public static method Main(): int "
        "    using var r := new ClsIdx2509T5(); "
        "    r[5] += 3; "
        "    return r[2]; "
        "  end Main; "
        "end C2509T5;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 5);
}

// ===========================================================================
// T6 — imported CLASS `r[i]++` axis (the IncrementStmt path). Pre-fix: 2
// (silent no-op). Post-fix: 3 (same arithmetic as T2).
// ===========================================================================
BEZEL_TEST(indexer_compound_increment_import_2509, class_increment) {
    const char* lib =
        "module ClsIdxLib2509T6; "
        "public class ClsIdx2509T6 "
        "  private _v: int := 0; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end ClsIdx2509T6;";
    int r = compileImportAndRun2509(
        lib,
        "module App2509T6; import ClsIdxLib2509T6; "
        "public class C2509T6 "
        "  public static method Main(): int "
        "    using var r := new ClsIdx2509T6(); "
        "    r[5]++; "
        "    return r[2]; "
        "  end Main; "
        "end C2509T6;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 3);
}

// ===========================================================================
// T7 — MULTI-DIMENSIONAL imported RECORD indexer `r[a,b] -= v`, exercising
// the per-index-once loop in BOTH the compound and increment `!pd` branches
// (each index spilled to its own slot, shared between the getter and setter
// calls — §23.4 single-evaluation contract for N>1 indices). Pre-fix: 11
// (silent no-op — `_v` stays 0, `r[1,1] = 0+10+1 = 11`). Post-fix: 6
// (`r[2,3] -= 5`: get `0+20+3 = 23`, new `23-5=18`, set
// `_v := 18-20-3 = -5`; `r[1,1]` reads `-5+10+1 = 6`).
// ===========================================================================
BEZEL_TEST(indexer_compound_increment_import_2509,
           record_multi_dim_compound_sub) {
    const char* lib =
        "module MultiIdxLib2509T7; "
        "public record MultiIdxRec2509T7(seed: int) "
        "  public _v: int := 0; "
        "  public property [i: int, j: int]: int "
        "    get => _v + i * 10 + j; "
        "    set => _v := value - i * 10 - j; "
        "  end; "
        "end MultiIdxRec2509T7;";
    int r = compileImportAndRun2509(
        lib,
        "module App2509T7; import MultiIdxLib2509T7; "
        "public class C2509T7 "
        "  public static method Main(): int "
        "    var r := new MultiIdxRec2509T7(9); "
        "    r[2, 3] -= 5; "
        "    return r[1, 1]; "
        "  end Main; "
        "end C2509T7;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 6);
}

// ===========================================================================
// T8 — PRIVATE backing field axis on an imported RECORD's `r[i]--`. The
// backing field is `private` in the library module — only the indexer
// accessors can reach it, so this confirms the cross-module RMW dispatches
// the exported ABI stubs (which see private state through the accessor
// bodies) rather than any same-module inline path. Pre-fix: 3 (silent
// no-op — `_w` stays 0, `r[3] = 0+3 = 3`). Post-fix: 2 (`r[4]--`: get
// `0+4 = 4`, new `4-1=3`, set `_w := 3-4 = -1`; `r[3]` reads `-1+3 = 2`).
// ===========================================================================
BEZEL_TEST(indexer_compound_increment_import_2509,
           record_private_backing_decrement) {
    const char* lib =
        "module PrivIdxLib2509T8; "
        "public record PrivIdxRec2509T8(seed: int) "
        "  private _w: int := 0; "
        "  public property [k: int]: int "
        "    get => _w + k; "
        "    set => _w := value - k; "
        "  end; "
        "end PrivIdxRec2509T8;";
    int r = compileImportAndRun2509(
        lib,
        "module App2509T8; import PrivIdxLib2509T8; "
        "public class C2509T8 "
        "  public static method Main(): int "
        "    var r := new PrivIdxRec2509T8(9); "
        "    r[4]--; "
        "    return r[3]; "
        "  end Main; "
        "end C2509T8;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 2);
}

}  // namespace bezel::module
