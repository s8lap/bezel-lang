// fix #2955 (§23.4 "Indexers" / §13 "Interfaces" / §14.1 "Generic
// declarations" / §2.8.3 rule 4, v0.240): a type-parameter-typed INDEX
// PARAMETER (`property [i: T]: int`) on an interface reached through a
// generic interface aborts with a raw LLVM module-verifier error and no
// source location, on a legal two-module program. The compiler reports
// "malformed IR emitted" instead of either compiling it or declining it with
// a §-cited diagnostic. Measured while probing the axis grid for #2946, on
// `f069645` AND with #2946's fix applied — byte-identical output both ways,
// so this is pre-existing and not caused or fixed by #2946.
//
// THE DEFECT — a CALL-SITE bug, not a table-fill bug. The itable slot is
// filled by #2946 / `accessorAbiFits` (LlvmBackend_Setup.cpp) with the
// cross-module stub `bz$<Class>$idxget$<arity>`, whose parameter types are
// the ERASED template (T → i32 placeholder). The call site builds the same
// `i32 (ptr, i32)` signature, but the index ARGUMENT is the substituted
// concrete type (`ptr` for `string`). The LLVM verifier rejects the call:
// the function signature expects `i32` for the second parameter but the
// argument is `ptr @.bz.str.50`. The same shape reached DIRECTLY through
// the implementing class (`g["k"]` on `var g := new GArrU5<string>()`) hits
// the same exported stub and byte-identically aborts — verified pre-fix.
//
// THE FIX SHIPPED HERE IS A DECLINE, NOT THE FULL CAPABILITY (CLAUDE.md
// rule 4), mirroring #2884 (the property twin, "not yet monomorphized
// (§14.1 / #2884)") in form and reasoning. It rejects the access at the
// source with a §-cited diagnostic — strictly better than the raw verifier
// abort, which was illegible — and rejects NOTHING that works today:
// every shape the issue names is broken NOW, and the working neighbours
// (a non-generic receiver, an indexer parameter typed by a
// declaration-resolved class/record, a generic class accessed DIRECTLY in
// the same module) are outside the gate and pinned as T5/T6/T7.
//
// SCOPE OF THE DECLINE: only the shapes the ABI mismatch actually breaks:
//   * the receiver is a generic Interface (any — local or imported — the
//     itable dispatch is always broken when the substitution changes the
//     ABI);
//   * OR the receiver is an IMPORTED generic Class/Record (the cross-module
//     stub is the same erasure and there is no per-instantiation
//     re-emission).
// The LOCAL generic Class/Record case is OUT OF GATE — codegen inlines the
// body into the caller (the `getterExpr`/`getterBlock` paths in
// `lowerIndexerGet`/`-Set`), and the body runs in the caller's substitution
// context, so a body that doesn't touch the index parameter works at
// runtime even when the indexer's declared parameter type is the generic
// param. PROBED: `GCtrl4<string>` accessed directly as `g["k"]` (local
// generic class) returns the correct 42, no verifier abort — the body is
// `_w + 2`, never reads `i`.
//
// WHY NOT THE REAL FIX HERE: making the access WORK requires either
// per-instantiation re-emission of the indexer accessor bodies (the
// `methodBodyText` / `ctorBodyText` / `fieldInitText` analogue, #428
// family) — the same metadata-carriage change #2884's property twin
// deferred — OR building the indirect call's type from the substituted
// parameter type at the call site, which would still call a function
// whose ABI doesn't match (silent type confusion; the body might dereference
// the index as `i32` when it was passed as `ptr`). The decline is honest.
//
// FAIL-WITHOUT/PASS-WITH: with src/ reverted, T1-T4 fail at codegen with
// the raw verifier abort "Call parameter type does not match function
// signature!" (and no source location); T5-T7 pass (the accept controls).
// With the fix, T1-T4 fail at SEMANTIC time with a §-cited diagnostic
// naming the indexer / interface / #2955; T5-T7 still pass.

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
// COVERAGE SHORT). popen() at L163 is kept — per-stem uniqueness already
// mitigates the file-overwrite race; full fork isolation is a separate concern.
const std::filesystem::path& wtDir2955() {
    return wtScratchDir("wt-2955");
}

struct TempFile2955 {
    std::string path;
    explicit TempFile2955(std::string p) : path(std::move(p)) {}
    ~TempFile2955() { std::remove(path.c_str()); }
};

void writeSource2955(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct BuildResult2955 { bool depOk; bool success; std::string err; };

// Compile `libSrc` to an object, then compile `appSrc` against it capturing
// stderr — pre-fix these programs also fail to build (at the LLVM verifier,
// with no diagnostic text), so "the compile failed" is not evidence of
// anything.
BuildResult2955 compileImportCaptureErr2955(const std::string& libSrc,
                                            const std::string& appSrc,
                                            const std::string& tag) {
    auto dir = wtDir2955();
    std::string stem = "bz2955_" + tag + "_" + unique_stem("");
    TempFile2955 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2955 aObj((dir / (stem + "_a.o")).string());
    writeSource2955(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return {false, false, ""};
    }
    if (!std::ifstream(aObj.path).good()) return {false, false, ""};

    TempFile2955 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2955 bExe((dir / (stem + "_b")).string());
    writeSource2955(bSrc.path, appSrc);

    std::string errPath = (dir / (stem + "_err.txt")).string();
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
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        success = driver.compileFile(bSrc.path, bExe.path, deps).success;
    }
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) { ::close(fd); ::dup2(saved, STDERR_FILENO); }
    ::close(saved);
    std::ifstream in(errPath);
    std::string err((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    std::remove(bExe.path.c_str());
    return {true, success, err};
}

// Same-module compile + run, capturing stdout — for the ACCEPT controls
// that don't need a cross-module artifact to pin the gate.
std::pair<int, std::string> compileAndRun2955(const std::string& src,
                                              const std::string& tag) {
    auto dir = wtDir2955();
    std::string stem = "bz2955s_" + tag + "_" + unique_stem("");
    TempFile2955 aSrc((dir / (stem + ".bzl")).string());
    TempFile2955 aExe((dir / stem).string());
    writeSource2955(aSrc.path, src);
    {
        bezel::Driver driver;
        if (!driver.compileFile(aSrc.path, aExe.path, {}).success)
            return {-1, ""};
    }
    std::string cmd = aExe.path + " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return {-1, ""};
    std::string out;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    std::remove(aExe.path.c_str());
    return {code, out};
}

bool has2955(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Load-bearing fragments of the decline diagnostic. Asserted rather than
// the whole sentence so re-wording the advice does not break the pin.
void expectDecline2955(const std::string& err, const std::string& recvName) {
    BEZEL_EXPECT_TRUE(has2955(err, "indexer `.this`"));
    BEZEL_EXPECT_TRUE(has2955(err, "generic type `" + recvName + "`"));
    BEZEL_EXPECT_TRUE(has2955(err, "not yet monomorphized"));
    BEZEL_EXPECT_TRUE(has2955(err, "#2955"));
    // NOT the raw verifier abort — that is the pre-fix outcome.
    BEZEL_EXPECT_FALSE(has2955(err,
        "Call parameter type does not match function signature!"));
    BEZEL_EXPECT_FALSE(has2955(err, "IR verification failed"));
}

const char* kGenLib2955 =
    "module GIdxLib2955; "
    "public interface IAt2955<U> "
    "  property [i: U]: int get; end; "
    "end IAt2955; "
    "public class GArr2955<T>: IAt2955<T> "
    "  public _w: int := 40; "
    "  public property [i: T]: int "
    "    get => _w + 2; "
    "  end; "
    "end GArr2955;";

}  // namespace

// ===========================================================================
// T1 — THE ISSUE'S OWN REPRO. An INDEXER whose INDEX PARAMETER is typed by
// the interface's own type parameter (`property [i: U]: int`), reached
// through `r: IAtU5<string>`. Pre-fix: exit non-zero with the raw LLVM
// verifier abort "Call parameter type does not match function signature!"
// — no source location, illegible. Now: a §-cited diagnostic at the access.
// §23.4 / §13 / §14.1 / §2.8.3 rule 4.
// ===========================================================================
BEZEL_TEST(xmod_generic_indexer_param_2955,
           iface_indexer_via_generic_interface_declined) {
    auto r = compileImportCaptureErr2955(
        kGenLib2955,
        "module GIdxApp2955T1; import GIdxLib2955; "
        "public class M2955T1 "
        "  public static method Main(): int "
        "    using var g := new GArr2955<string>(); "
        "    var r: IAt2955<string> := g; "
        "    return r[\"k\"]; "
        "  end Main; "
        "end M2955T1;",
        "t1");
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    expectDecline2955(r.err, "IAt2955");
}

// ===========================================================================
// T2 — THE SETTER TWIN. The issue's own diagnosis names it ("The
// named-property twin of this shape does not reach here (a property has no
// index parameters)"); but the indexer setter has the same ABI question,
// and `lowerIndexerSet`'s interface-itable dispatch reads
// `lowerType(pd->indexerParams[k].type)` with the same erasure. A sourced
// diagnostic, not a verifier abort. §23.4 / §23.3.
// ===========================================================================
BEZEL_TEST(xmod_generic_indexer_param_2955,
           iface_indexer_via_generic_interface_setter_declined) {
    auto r = compileImportCaptureErr2955(
        "module GIdxLib2955T2; "
        "public interface ISet2955<U> "
        "  property [i: U]: int get; set; end; "
        "end ISet2955; "
        "public class GSet2955<T>: ISet2955<T> "
        "  public _w: int := 0; "
        "  public property [i: T]: int "
        "    get => _w; "
        "    set => do _w := value; end; "
        "  end; "
        "end GSet2955;",
        "module GIdxApp2955T2; import GIdxLib2955T2; "
        "public class M2955T2 "
        "  public static method Main(): int "
        "    using var g := new GSet2955<string>(); "
        "    var r: ISet2955<string> := g; "
        "    r[\"k\"] := 99; "
        "    return 0; "
        "  end Main; "
        "end M2955T2;",
        "t2");
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    expectDecline2955(r.err, "ISet2955");
}

// ===========================================================================
// T3 — DIRECTLY through the implementing class (the imported class type, not
// the interface). Same exported stub `bz$<Class>$idxget$<arity>` with the
// same erased `T -> i32` parameter type, same ABI mismatch, byte-identical
// pre-fix verifier abort. Now: the same decline. §23.4 / §2.5 / §2.8.2
// clause 1.
// ===========================================================================
BEZEL_TEST(xmod_generic_indexer_param_2955,
           direct_imported_generic_class_declined) {
    auto r = compileImportCaptureErr2955(
        kGenLib2955,
        "module GIdxApp2955T3; import GIdxLib2955; "
        "public class M2955T3 "
        "  public static method Main(): int "
        "    using var g := new GArr2955<string>(); "
        "    return g[\"k\"]; "
        "  end Main; "
        "end M2955T3;",
        "t3");
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    expectDecline2955(r.err, "GArr2955");
}

// ===========================================================================
// T4 — LOCAL same-module case reached THROUGH the interface. The itable
// slot is filled with the implementing class's getter, which has the same
// erased parameter type; the call type is the same erasure; the argument
// is the substituted `ptr`. Same ABI mismatch, same verifier abort
// pre-fix. Now: the same decline — this is the "Interface" branch of the
// gate, irrespective of import. §23.4 / §13 / §14.1.
// ===========================================================================
BEZEL_TEST(xmod_generic_indexer_param_2955,
           local_same_module_iface_indexer_declined) {
    auto r = compileImportCaptureErr2955(
        "module GIdxLib2955T4; "
        "public interface ILcl2955<U> "
        "  property [i: U]: int get; end; "
        "end ILcl2955; "
        "public class GLcl2955<T>: ILcl2955<T> "
        "  public _w: int := 40; "
        "  public property [i: T]: int "
        "    get => _w + 2; "
        "  end; "
        "end GLcl2955;",
        "module GIdxApp2955T4; import GIdxLib2955T4; "
        "public class M2955T4 "
        "  public static method Main(): int "
        "    using var g := new GLcl2955<string>(); "
        "    var r: ILcl2955<string> := g; "
        "    return r[\"k\"]; "
        "  end Main; "
        "end M2955T4;",
        "t4");
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    expectDecline2955(r.err, "ILcl2955");
}

// ===========================================================================
// T5 — ACCEPT CONTROL: a NON-generic interface with the same indexer shape
// is not in the gate (no substitution, the stub ABI matches the call ABI)
// and must stay correct. §2.5 / §13 / §23.4.
// ===========================================================================
BEZEL_TEST(xmod_generic_indexer_param_2955,
           nongeneric_iface_indexer_unchanged_spec_2_5) {
    auto r = compileImportCaptureErr2955(
        "module GIdxLib2955T5; "
        "public interface IPlain2955 "
        "  property [i: string]: int get; end; "
        "end IPlain2955; "
        "public class GPlain2955: IPlain2955 "
        "  public _w: int := 40; "
        "  public property [i: string]: int "
        "    get => _w + 2; "
        "  end; "
        "end GPlain2955;",
        "module GIdxApp2955T5; import GIdxLib2955T5; "
        "public class M2955T5 "
        "  public static method Main(): int "
        "    using var g := new GPlain2955(); "
        "    var r: IPlain2955 := g; "
        "    return r[\"k\"]; "
        "  end Main; "
        "end M2955T5;",
        "t5");
    if (!r.depOk) return;
    BEZEL_EXPECT_TRUE(r.success);
}

// ===========================================================================
// T6 — ACCEPT CONTROL: a generic class whose INDEXER PARAMETER is NOT typed
// by the class's type parameter (e.g. `[i: int]`) is not in the gate —
// `lowerType(int)` is `i32` regardless of substitution, so the stub ABI
// matches the call ABI. The same imported class at a `T = string`
// instantiation is reachable today; widening the gate to this shape would
// be a regression. §14.1 / §23.4.
// ===========================================================================
BEZEL_TEST(xmod_generic_indexer_param_2955,
           imported_generic_class_int_index_param_unchanged) {
    auto r = compileImportCaptureErr2955(
        "module GIdxLib2955T6; "
        "public class GIntIdx2955<T> "
        "  public _w: int := 40; "
        "  public property [i: int]: int "
        "    get => _w + i; "
        "  end; "
        "end GIntIdx2955;",
        "module GIdxApp2955T6; import GIdxLib2955T6; "
        "public class M2955T6 "
        "  public static method Main(): int "
        "    using var gs := new GIntIdx2955<string>(); "
        "    return gs[5]; "
        "  end Main; "
        "end M2955T6;",
        "t6");
    if (!r.depOk) return;
    BEZEL_EXPECT_TRUE(r.success);
}

// ===========================================================================
// T7 — ACCEPT CONTROL: the LOCAL same-module generic class with a T-typed
// index parameter (the case the gate deliberately does NOT cover — the
// body is inlined and runs in the caller's substitution context). The
// issue's own "local twin" note: `r["k"]` through an interface aborts;
// `g["k"]` directly does not. Pinned here so the gate's scope is
// observable. §23.4 / §14.1 / §2.8.2 clause 2 ("an external instantiation
// behaves identically to a same-module instantiation") demands this same
// answer for the eventual real fix on the imported case.
// ===========================================================================
BEZEL_TEST(xmod_generic_indexer_param_2955,
           local_same_module_generic_class_indexer_correct) {
    auto [code, _out] = compileAndRun2955(
        "module Lcl2955T7; "
        "public class GLcl2955T7<T> "
        "  public _w: int := 40; "
        "  public property [i: T]: int "
        "    get => _w + 2; "
        "  end; "
        "end GLcl2955T7; "
        "public class M2955T7 "
        "  public static method Main(): int "
        "    using var g := new GLcl2955T7<string>(); "
        "    return g[\"k\"]; "
        "  end Main; "
        "end M2955T7;",
        "t7");
    if (code == -1) return;
    BEZEL_EXPECT_EQ(code, 42);
}

// ===========================================================================
// T8 — ACCEPT CONTROL: a generic interface with the indexer parameter typed
// by the interface's type parameter, used through the interface, where
// the substitution DOES match the erasure (T → int → i32). The stub ABI
// (`i32 (ptr, i32)`) IS the call ABI; the verifier accepts. The decline
// must NOT fire here — the gate is on the DECLARED INDEX PARAMETER TYPE
// matching one of the receiver's generic params, not on whether the
// substitution actually moves the ABI. This is a deliberate scoping
// choice: a stricter "substitution moves the ABI" check would have to
// evaluate the substitution at sema time, which the typechecker does not
// do in this position (it would have to walk the recvTy→genericArgs and
// resolve them against the indexer's enclosing param set — a larger
// refactor that the property-side twins #2884 / #2944 / #2928 don't ask
// for either). Pin here as the scope boundary. §14.1 / §23.4.
// ===========================================================================
BEZEL_TEST(xmod_generic_indexer_param_2955,
           generic_iface_int_substitution_still_works) {
    auto r = compileImportCaptureErr2955(
        "module GIdxLib2955T8; "
        "public interface IIntIdx2955<U> "
        "  property [i: U]: int "
        "    get; "
        "  end; "
        "end IIntIdx2955; "
        "public class GIntIdxIface2955<T>: IIntIdx2955<T> "
        "  public _w: int := 40; "
        "  public property [i: T]: int "
        "    get => _w + i; "
        "  end; "
        "end GIntIdxIface2955;",
        "module GIdxApp2955T8; import GIdxLib2955T8; "
        "public class M2955T8 "
        "  public static method Main(): int "
        "    using var g := new GIntIdxIface2955<int>(); "
        "    var r: IIntIdx2955<int> := g; "
        "    return r[2]; "
        "  end Main; "
        "end M2955T8;",
        "t8");
    if (!r.depOk) return;
    BEZEL_EXPECT_TRUE(r.success);
}

}  // namespace bezel::module
