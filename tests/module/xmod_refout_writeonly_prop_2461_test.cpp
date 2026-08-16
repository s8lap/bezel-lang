// fix #2461 (§22.3 / §23, v0.236): `checkRefOutPropertyOperand`
// (ExprTypecheck.cpp) could not distinguish a WRITE-ONLY imported property
// (a property with a `set` accessor but NO `get` accessor) as an illegal
// `ref` target — the getter-presence twin of #2400's `hasSetter` metadata
// bit did not exist, so the operand fell through sema to a raw LINKER error
// (undefined `...$propget` symbol — the exporter never emitted a getter for
// a write-only property) instead of a clean §22.3/§23 rejection.
//
// §22.3 "`ref` and `out` argument locations" (v0.236): "for `ref`, the
// getter is called once to produce the callee's slot value (for `out` no
// getter call occurs); on normal return the setter is called once with the
// slot's final value." A write-only property (setter but no getter) is
// therefore a LEGAL `out` target (no getter call) but NOT a legal `ref`
// target (the copy-in has no getter to call). The LOCAL same-module shape
// was already correctly rejected, but by a SEPARATE earlier check
// (ExprTypecheck_Calls.cpp's `propertyDeclIsSetOnly` gate on the `d.W`
// member-access READ), which is `pd`-gated and so does not cover an imported
// property (no ast::PropertyDecl crosses the boundary).
//
// FIX: adds a `MetaMember::hasGetter` bit (additive 14th MEMBER_BLOCK scalar
// within schema v7 — the getter-presence twin of #2400's 11th-scalar
// `hasSetter`), computed by the exporter mirroring `propertyDeclIsSetOnly`'s
// inverse exactly (Expression/Auto bodyKinds always have a getter; a Block
// bodyKind has a getter iff any accessor is a `get` arm), published to
// `ctx_.importedPropertyHasGetter`, and consulted in
// `checkRefOutPropertyOperand`'s imported (`!pd`) branch — guarded by
// `kind == "ref"` so `out` is correctly admitted. The missing-scalar
// default is `true` (NOT `false` like `hasSetter`): a false default would
// FALSE-REJECT a legitimate `ref` to a read-write property from a stale
// (pre-#2461) dependency, while `true` preserves the pre-fix behavior for a
// stale dependency (write-only-as-ref stays the pre-existing linker error;
// read-write-as-ref stays a clean accept) until the dependency is
// recompiled — the same stale-dependency model every other additive scalar
// in this schema follows.
//
// SCOPE (recorded per CLAUDE.md rule 4, not silently applied):
//   - A write-only imported INDEXER as `ref` is an INDEPENDENT, pre-existing
//     gap NOT closed by this fix: the IndexExpr ref/out operand path does
//     not reach the write-only rejection (the LOCAL same-module shape ICEs
//     at codegen with "#1386 lowerExpr returned null" for the identical
//     write-only-indexer-as-ref shape, so there is no local≡imported
//     inconsistency to mirror — both are broken). Filed as a separate
//     follow-up; this fix's hasGetter bit IS published for indexers too
//     (the metadata faithfully carries it), but checkRefOutPropertyOperand's
//     IndexExpr call site does not resolve an imported indexer to the
//     PropertySymbol this check keys on.
//   - A plain READ of an imported write-only property that is NOT a ref/out
//     argument (e.g. `var x := d.W;`) remains a separate gap — the
//     `propertyDeclIsSetOnly` read-of-write-only gate is also `pd`-gated.
//     That gap is orthogonal to #2461 (which is scoped to the ref/out
//     operand) and would additionally false-reject the spec-LEGAL
//     write-only-as-`out` case if widened naively (see T2), so it is left
//     for a dedicated follow-up.
//
// Mechanically verified fail-without/pass-with: T1 (the bug case) was
// confirmed to produce a raw LINKER error ("undefined reference to
// ...$propget") without the fix and the clean §22.3/§23 message with it;
// T2/T3 (accept controls) confirmed unaffected (still compile + run).

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
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile2461 {
    std::string path;
    explicit TempFile2461(std::string p) : path(std::move(p)) {}
    ~TempFile2461() { std::remove(path.c_str()); }
};

void writeSource2461(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2461(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile B (importing A's compiled object), capturing the driver's stderr
// (fd 2) into `err`. Mirrors xmod_refout_prop_visibility_2454_test.cpp's
// compileImportCaptureErr2454.
bool compileImportCaptureErr2461(const std::string& bSrc,
                                 const std::string& bExe,
                                 const std::string& aObj, std::string& err) {
    namespace fs = std::filesystem;
    std::string errPath = (fs::temp_directory_path() /
                           ("bz2461_err_" + unique_stem("") +
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

// Compile B importing A -> exe, run in a SEPARATE process (fork-isolated per
// tests/README.md), return exit code (-1 on compile/link/run failure, -2 if
// the dependency itself could not be produced). Mirrors #2454's helper.
int compileImportAndRun2461(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2461_");
    TempFile2461 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2461 aObj((dir / (stem + "_a.o")).string());
    writeSource2461(aSrc.path, libSrc);
    if (!compileToObj2461(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2461 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2461 bExe((dir / stem).string());
    writeSource2461(bSrc.path, appSrc);
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
// T1 — the bug case: a WRITE-ONLY imported property (set-only, no getter)
// used as a `ref` argument. §22.3: ref's copy-in calls the getter once; a
// write-only property has no getter, so this MUST be rejected. Pre-fix the
// operand fell through sema to a raw LINKER error ("undefined reference to
// ...$propget"); post-fix it rejects with the clean §22.3/§23 message.
// Asserts the diagnostic TEXT (the message-quality improvement this fix
// ships), which is absent pre-fix (a linker error carries none of it).
// ===========================================================================
BEZEL_TEST(xmod_refout_writeonly_prop_2461,
           write_only_imported_property_rejected_as_ref_arg) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2461_t1_");
    TempFile2461 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2461 aObj((dir / (stem + "_a.o")).string());
    writeSource2461(aSrc.path,
                    "module WriteOnlyLib2461T1; "
                    "public class WriteOnlyClass2461T1 "
                    "  private _n: int := 1; "
                    "  public property W: int "
                    "    set => _n := value; "
                    "  end W; "
                    "  public method Peek(): int => _n; "
                    "end WriteOnlyClass2461T1;");
    BEZEL_EXPECT_TRUE(compileToObj2461(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;  // toolchain unavailable

    std::string bSrc =
        "module App2461T1; import WriteOnlyLib2461T1; "
        "public class C "
        "  public static method Bump(ref v: int) "
        "    v := v + 1; "
        "  end Bump; "
        "  public static method Main(): int "
        "    using var d: WriteOnlyClass2461T1 := new "
        "WriteOnlyClass2461T1(); "
        "    C.Bump(ref d.W); "
        "    return 0; "
        "  end Main; "
        "end C;";
    TempFile2461 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2461 bExe((dir / (stem + "_b")).string());
    writeSource2461(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2461(bSrcFile.path, bExe.path, aObj.path, err);
    // Rejected both before and after the fix (pre-fix linker error, post-fix
    // sema rejection) — `success` alone does not distinguish them.
    BEZEL_EXPECT_TRUE(!success);
    // The clean §22.3/§23 message is present ONLY with the fix (pre-fix the
    // stderr carries a raw "undefined reference to ...$propget" linker error
    // and none of these phrases). This is the fail-without/pass-with proof.
    BEZEL_EXPECT_TRUE(err.find("write-only") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("no getter for the copy-in") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("22.3") != std::string::npos);
    // And the pre-fix raw linker error is gone with the fix.
    BEZEL_EXPECT_TRUE(err.find("undefined reference") == std::string::npos);
}

// ===========================================================================
// T2 — ACCEPT-CONTROL (false-reject guard): the SAME write-only imported
// property as T1, but as an `out` argument. §22.3: "for `out` no getter call
// occurs" — a write-only property IS a legal `out` target (the setter is
// present for the copy-out; no getter is needed). Compiles AND RUNS,
// asserting the value actually written (read back via a Peek method, since
// the property itself has no getter). The fix's `kind == "ref"` guard must
// NOT reject this `out` site.
// ===========================================================================
BEZEL_TEST(xmod_refout_writeonly_prop_2461,
           write_only_imported_property_accepted_as_out_arg_and_writes) {
    int r = compileImportAndRun2461(
        "module WriteOnlyLib2461T2; "
        "public class WriteOnlyClass2461T2 "
        "  private _n: int := 1; "
        "  public property W: int "
        "    set => _n := value; "
        "  end W; "
        "  public method Peek(): int => _n; "
        "end WriteOnlyClass2461T2;",
        "module App2461T2; import WriteOnlyLib2461T2; "
        "public class C "
        "  public static method Produce(out v: int) "
        "    v := 42; "
        "  end Produce; "
        "  public static method Main(): int "
        "    using var d: WriteOnlyClass2461T2 := new "
        "WriteOnlyClass2461T2(); "
        "    C.Produce(out d.W); "
        "    return d.Peek() - 42; "
        "  end Main; "
        "end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T3 — ACCEPT-CONTROL (false-reject guard): a READ-WRITE imported property
// (both get and set) as a `ref` argument. §22.3: a read-write property is a
// legal `ref` target (getter for copy-in, setter for copy-out). Compiles AND
// RUNS, asserting the round-trip (1 -> 2). The fix's hasGetter check must
// NOT false-reject a property that DOES have a getter — the core false-reject
// axis for a `true`-defaulting getter-presence bit.
// ===========================================================================
BEZEL_TEST(xmod_refout_writeonly_prop_2461,
           read_write_imported_property_accepted_as_ref_arg_and_writes) {
    int r = compileImportAndRun2461(
        "module RWLib2461T3; "
        "public class RWClass2461T3 "
        "  private _n: int := 1; "
        "  public property N: int "
        "    get => _n; "
        "    public set => _n := value; "
        "  end N; "
        "end RWClass2461T3;",
        "module App2461T3; import RWLib2461T3; "
        "public class C "
        "  public static method Bump(ref v: int) "
        "    v := v + 1; "
        "  end Bump; "
        "  public static method Main(): int "
        "    using var d: RWClass2461T3 := new RWClass2461T3(); "
        "    C.Bump(ref d.N); "
        "    return d.N - 2; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T4 — REJECT-CONTROL (local≡imported consistency): the SAME-MODULE (local)
// equivalent of T1's shape is still, correctly, rejected — unaffected by the
// fix (the fix only adds to the imported `!pd` branch; the local path was
// already correctly rejected by ExprTypecheck_Calls.cpp's
// propertyDeclIsSetOnly READ gate, unchanged). Confirms the fix did not
// regress the local path and that local≡imported both reject a write-only
// property as `ref`. Drives the Driver directly (no dependency object).
// ===========================================================================
BEZEL_TEST(xmod_refout_writeonly_prop_2461,
           local_write_only_property_still_rejected_as_ref_control) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2461_t4_");
    TempFile2461 src((dir / (stem + ".bzl")).string());
    TempFile2461 exe((dir / stem).string());
    writeSource2461(src.path,
                    "module LocalMod2461T4; "
                    "public class WriteOnlyClass2461T4 "
                    "  private _n: int := 1; "
                    "  public property W: int "
                    "    set => _n := value; "
                    "  end W; "
                    "end WriteOnlyClass2461T4; "
                    "public class C "
                    "  public static method Bump(ref v: int) "
                    "    v := v + 1; "
                    "  end Bump; "
                    "  public static method Main(): int "
                    "    using var d: WriteOnlyClass2461T4 := "
                    "new WriteOnlyClass2461T4(); "
                    "    C.Bump(ref d.W); "
                    "    return 0; "
                    "  end Main; "
                    "end C;");

    std::string errPath = (dir / (stem + "_err.txt")).string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool success = false;
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver driver;
        success = driver.compileFile(src.path, exe.path, {}).success;
    }
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);
    std::ifstream in(errPath);
    std::string err((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());

    BEZEL_EXPECT_TRUE(!success);
    // The local path rejects via the read-of-write-only gate (§23), whose
    // message shape the imported-branch fix deliberately mirrors.
    BEZEL_EXPECT_TRUE(err.find("write-only") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("23") != std::string::npos);
}

}  // namespace bezel::module
