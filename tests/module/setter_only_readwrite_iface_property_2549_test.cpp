// fix #2549 (§11.2 / §23.3 / §2.8.2 clause 1 / §2.8.3 rule 4, v0.236):
// the SYMMETRIC INVERSE of #2413's setter-axis accessor-presence fix. The
// pre-existing setter check in `Resolver::checkAbstractImplementations`
// (TypeResolution_Checks.cpp ~3780) rejects a GETTER-ONLY impl of a
// read-write interface property obligation — the missing-setter side. The
// getter axis had no analogous check: a SETTER-ONLY impl of a read-write
// interface property obligation (the missing-getter side, the NAMED-
// property twin of #2529's indexer sibling) silently compiled. The runtime
// then dispatched through a null getter itable slot and SIGSEGV'd at
// pc=0x0 — confirmed via ASan probe: "AddressSanitizer: SEGV on unknown
// address 0x000000000000 (pc 0x000000000000 bp ... sp ... T0)" with
// "Hint: pc points to the zero page". The iface side declared a read-write
// property (`property Name: int;` is Auto-body, has both arms by default),
// the impl had a Block body with only a `set` arm (no `get` arm), and the
// setter check above did not fire because the impl DID declare a setter.
//
// §11.2 (v0.235): "A member matches an interface member when it is declared
// `public`, has the same name, the same arity, the same parameter
// modifiers, parameter types that are exactly equal ..., and a return type
// that is exactly equal". The implicit accessor-set invariant is the same
// as the existing setter check enforces — a getter-only impl does not
// satisfy a read-write obligation, and neither does a setter-only impl. §23.3
// ("A read-only property in block form omits `set`; a write-only property
// omits `get`") supplies the symmetric missing-getter rule: a Block-body
// implementation has a getter iff it declares a `get` arm.
//
// Mirrors the local-and-imported fallback shape of the indexer-side
// `ifaceHasSetter`/`candidateHasSetter` lambdas in
// `ifaceIndexerObligations` (same file, a few hundred lines below, #2529/
// #2449). The `MetaMember::hasGetter` bit (#2461) is already published to
// `ctx_.importedPropertyHasGetter`, the same accessor that #2543's
// indexer-read gate and #2530's `checkRefOutPropertyOperand` consume.
// Crucially, the setter-only pattern is RARE in the corpus (manually
// swept during this fix across tests/module/ and tests/sema/ — every
// iface-property impl in those directories is either expression-bodied,
// full block (get + set), or positional-record; NONE pair a setter-only
// block with a read-write iface), so the IMPORTED-side fallback ships
// alongside the LOCAL fix without a separate fixture-cleanup pass (unlike
// #2542's reverted setter-side fallback, where #2401/#2387's legacy
// tests paired getter-only impls with read-write iface obligations — see
// #2552 for the deferred fixture work).
//
// Each reject test asserts the SPECIFIC diagnostic TEXT (§11.2 + §23.3 +
// "setter-only implementation" + "must provide a getter"), not merely a
// failed compile: `runAot`'s `WIFSIGNALED` branch synthesizes an `Error`
// for a runtime SIGSEGV too, so the coarse `rejects()` helper is INSUFFICIENT
// (it would pass for the wrong/crash reason). The fail-without/pass-with
// discriminator is the diagnostic substring; pre-fix the compile produces
// no error at all (silent accept, crash on iface read), post-fix it
// produces the §11.2/§23.3 message. Each test first proves the dependency
// artifact was produced, so a malformed two-module setup cannot turn the
// expected importer rejection into a vacuous pass.
//
// File shape mirrors xmod_abstract_impl_prop_2380_test.cpp's helpers
// (compileToObj + compileImportCaptureErr + compileImportAndRun); see the
// header of that file for the full rationale on the cross-module testing
// pattern and the why-this-file-exists documentation for #2380 itself.

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
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile2549 {
    std::string path;
    explicit TempFile2549(std::string p) : path(std::move(p)) {}
    ~TempFile2549() { std::remove(path.c_str()); }
};

void writeSource2549(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2549(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Capture the importer's stderr. Mirrors xmod_abstract_impl_prop_2380_test.cpp's
// compileImportCaptureErr2380.
bool compileImportCaptureErr2549(const std::string& bSrc,
                                 const std::string& bExe,
                                 const std::string& aObj,
                                 std::string& err) {
    namespace fs = std::filesystem;
    std::string errPath =
        (fs::temp_directory_path() /
         ("bz2549_err_" + unique_stem("") + ".txt"))
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
        success = bezel::test::compileFileForTest(driver, bSrc, bExe, deps).success;
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

// Two-module driver: compile lib -> obj, compile app importing lib -> exe,
// capture app's stderr into `err` and return success. Both sides must
// succeed in the right order — `dependencyCompiled` confirms the lib was
// actually produced (so a malformed lib source cannot turn the expected
// importer rejection into a vacuous pass).
struct ImportCapture2549 {
    bool dependencyCompiled = false;
    bool appCompiled = false;
    std::string error;
};

ImportCapture2549 compileImportSources2549(const std::string& libSrc,
                                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2549_");
    TempFile2549 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2549 aObj((dir / (stem + "_a.o")).string());
    writeSource2549(aSrc.path, libSrc);
    ImportCapture2549 out;
    out.dependencyCompiled =
        compileToObj2549(aSrc.path, aObj.path) &&
        std::ifstream(aObj.path).good();
    if (!out.dependencyCompiled) return out;
    TempFile2549 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2549 bExe((dir / stem).string());
    writeSource2549(bSrc.path, appSrc);
    out.appCompiled = compileImportCaptureErr2549(bSrc.path, bExe.path,
                                                   aObj.path, out.error);
    return out;
}

// Compile-and-run for ACCEPT controls: lib -> obj, app -> exe, run exe.
int compileImportAndRun2549(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2549_");
    TempFile2549 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2549 aObj((dir / (stem + "_a.o")).string());
    writeSource2549(aSrc.path, libSrc);
    if (!compileToObj2549(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;
    TempFile2549 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2549 bExe((dir / stem).string());
    writeSource2549(bSrc.path, appSrc);
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

// Same-module driver — for LOCAL cases (no import). Mirrors the same helper
// in xmod_abstract_impl_prop_2380_test.cpp.
bool compileLocalReject2549(const std::string& src, std::string& err) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2549_local_");
    std::string errPath =
        (fs::temp_directory_path() /
         ("bz2549_local_err_" + unique_stem("") + ".txt"))
            .string();
    TempFile2549 srcFile((dir / (stem + ".bzl")).string());
    TempFile2549 exeFile((dir / stem).string());
    writeSource2549(srcFile.path, src);
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool success = false;
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver driver;
        success = bezel::test::compileFileForTest(driver, srcFile.path, exeFile.path, {}).success;
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

int compileLocalAndRun2549(const std::string& src) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2549_local_run_");
    TempFile2549 srcFile((dir / (stem + ".bzl")).string());
    TempFile2549 exeFile((dir / stem).string());
    writeSource2549(srcFile.path, src);
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, srcFile.path, exeFile.path, {}).success) return -1;
    int status = std::system(exeFile.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// T1 — LOCAL class with a BLOCK-body setter-only impl of a LOCAL read-write
// iface property. Pre-fix: silently compiled; runtime SIGSEGV at pc=0x0
// on `face.Name` (the iface-side getter itable slot is null because
// ModuleLoader's MetaMember::hasGetter is false for this impl, but no
// sema check ever said a read-write obligation requires the impl to
// declare a getter). Post-fix: rejected at compile time with the
// §11.2/§23.3 setter-only diagnostic.
//
// Asserts the SPECIFIC diagnostic text (not just a failed compile) — the
// coarse `rejects()` helper is insufficient because `runAot`'s
// WIFSIGNALED branch synthesizes an Error for a runtime SIGSEGV too. The
// substring match pins THIS defect specifically and distinguishes it from
// the pre-existing #2413 getter-only diagnostic, #2380's "different type",
// and #489's "does not implement interface property" shapes.
// ===========================================================================
BEZEL_TEST(setter_only_readwrite_iface_property_2549,
           local_class_setter_only_impl_rejected_11_2_23_3) {
    std::string err;
    bool ok = compileLocalReject2549(
        "module Local2549T1; "
        "public interface IHasName2549T1 "
        "  property Name: int; "
        "end IHasName2549T1; "
        "public class SetterOnly2549T1: IHasName2549T1 "
        "  private _n: int := 7; "
        "  public property Name: int "
        "    set => _n := value; "
        "  end Name; "
        "end SetterOnly2549T1; "
        "public class C public static method Main(): int => 0; end C;",
        err);
    BEZEL_EXPECT_TRUE(!ok);
    BEZEL_EXPECT_TRUE(err.find("property `Name` implements interface") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("setter-only implementation") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("declared read-write") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("must provide a getter") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("11.2") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("23.3") != std::string::npos);
}

// ===========================================================================
// T2 — the IMPORTED twin of T1: an IMPORTED iface property (no ast::
// PropertyDecl after ModuleLoader) with a LOCAL class setter-only impl.
// Pre-fix: silently compiled (the pre-existing setter check's
// `if (!ipd || !cpd) continue` LOCAL-only gate short-circuited, AND
// there was no getter check at all); post-fix: rejected via the
// `ifaceHasGetter`/`candidateHasGetter` lambdas that fall back to
// `ctx_.importedPropertyHasGetter` (#2461's MetaMember::hasGetter bit,
// already published to SemaContext) for the imported iface side.
//
// First proves the dependency artifact was produced, so a malformed
// lib source cannot turn the expected importer rejection into a vacuous
// pass. The iface-side `ifaceHasGetter` lambda reads
// `ctx_.importedPropertyHasGetter(ips)` because `ips->decl` is null for
// the imported iface property (ModuleLoader reconstructs the symbol but
// cannot reconstruct source AST); the candidate side reads
// `propertyDeclHasGetter(cpd)` directly because the local impl's
// PropertyDecl IS reconstructed.
// ===========================================================================
BEZEL_TEST(setter_only_readwrite_iface_property_2549,
           imported_iface_local_class_setter_only_rejected_11_2_23_3) {
    auto r = compileImportSources2549(
        "module IfaceLib2549T2; "
        "public interface IHasName2549T2 "
        "  property Name: int; "
        "end IHasName2549T2;",
        "module App2549T2; import IfaceLib2549T2; "
        "public class SetterOnly2549T2: IHasName2549T2 "
        "  private _n: int := 7; "
        "  public property Name: int "
        "    set => _n := value; "
        "  end Name; "
        "end SetterOnly2549T2; "
        "public class C public static method Main(): int => 0; end C;");
    BEZEL_EXPECT_TRUE(r.dependencyCompiled);
    if (!r.dependencyCompiled) return;
    BEZEL_EXPECT_TRUE(!r.appCompiled);
    BEZEL_EXPECT_TRUE(r.error.find("property `Name` implements interface") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(r.error.find("setter-only implementation") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(r.error.find("declared read-write") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.error.find("must provide a getter") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(r.error.find("11.2") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.error.find("23.3") != std::string::npos);
}

// ===========================================================================
// T3 — the IMPORTED-BASE-CLASS twin: a LOCAL read-write iface property
// obligation satisfied by a setter-only property INHERITED from an
// IMPORTED base class. The candidate side is the imported shape —
// ModuleLoader did not reconstruct an `ast::PropertyDecl` for the base
// class's `Name` member (same #2414 shape as the directly-imported iface),
// so `cpd` is null and `candidateHasGetter` falls back to
// `ctx_.importedPropertyHasGetter(cit->second)` — the SAME imported-side
// fallback the indexer-side `candidateHasSetter` lambda in
// `ifaceIndexerObligations` (#2449/#2529) already consumes for the
// setter-axis sibling. Pre-fix: silently compiled (the LOCAL-only
// `if (!ipd || !cpd) continue` gate short-circuited the setter check,
// AND there was no getter check at all); post-fix: rejected.
// ===========================================================================
BEZEL_TEST(setter_only_readwrite_iface_property_2549,
           inherited_from_imported_base_setter_only_rejected_11_2_23_3) {
    auto r = compileImportSources2549(
        "module SetterOnlyBaseLib2549T3; "
        "public class SetterOnlyBase2549T3 "
        "  public property Name: int "
        "    set => 0; "
        "  end Name; "
        "end SetterOnlyBase2549T3;",
        "module App2549T3; import SetterOnlyBaseLib2549T3; "
        "public interface IHasName2549T3 "
        "  property Name: int; "
        "end IHasName2549T3; "
        "public class Derived2549T3: SetterOnlyBase2549T3, IHasName2549T3 "
        "end Derived2549T3; "
        "public class C public static method Main(): int => 0; end C;");
    BEZEL_EXPECT_TRUE(r.dependencyCompiled);
    if (!r.dependencyCompiled) return;
    BEZEL_EXPECT_TRUE(!r.appCompiled);
    BEZEL_EXPECT_TRUE(r.error.find("property `Name` implements interface") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(r.error.find("setter-only implementation") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(r.error.find("declared read-write") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.error.find("must provide a getter") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(r.error.find("11.2") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.error.find("23.3") != std::string::npos);
}

// ===========================================================================
// T4 — REJECT-CONTROL (LOCAL): a setter-only impl of a READ-ONLY iface
// property obligation is REJECTED. The iface is `readonly` (so it has a
// getter but no setter — `propertyDeclHasGetter(ipd)` is true); the impl
// has only a `set` arm (no `get` arm); the iface obligation requires the
// impl to provide a getter (since the iface itself exposes a getter that
// callers may invoke through the iface reference); the impl fails on
// the getter axis. Same diagnostic shape as T1 — the read-write vs
// readonly distinction on the iface side does NOT change which accessors
// the impl must declare: the impl must always match the iface's accessor
// set, and a getter-only iface still needs a getter from the impl.
// Pre-fix this also silently compiled and SIGSEGV'd on `face.Name` (the
// missing-getter shape is the same on both iface shapes). Closes the
// getter-only-iface variant of the same defect.
// ===========================================================================
BEZEL_TEST(setter_only_readwrite_iface_property_2549,
           setter_only_impl_of_readonly_iface_rejected_11_2_23_3) {
    std::string err;
    bool ok = compileLocalReject2549(
        "module Local2549T4; "
        "public interface IHasName2549T4 "
        "  readonly property Name: int; "
        "end IHasName2549T4; "
        "public class SetterOnly2549T4: IHasName2549T4 "
        "  private _n: int := 7; "
        "  public property Name: int "
        "    set => _n := value; "
        "  end Name; "
        "end SetterOnly2549T4; "
        "public class C public static method Main(): int => 0; end C;",
        err);
    BEZEL_EXPECT_TRUE(!ok);
    BEZEL_EXPECT_TRUE(err.find("setter-only implementation") !=
                      std::string::npos);
}

// ===========================================================================
// T5 — FALSE-REJECT GUARD (LOCAL): a WRITE-ONLY iface property obligation
// satisfied by a setter-only impl is ACCEPTED and RUNS. The iface side
// declares only a `set` accessor (a Block body on the iface with no `get`
// arm — §23.3's "write-only property omits `get`"), so
// `propertyDeclHasGetter(ipd)` is FALSE and `ifaceHasGetter()` is FALSE,
// the `ifaceHasGetter() && !candidateHasGetter` condition never fires, and
// the impl is accepted.
//
// A `WRITE-ONLY iface property` is rare but spec-permitted: the iface can
// declare `property W: int;` in block form with only a `set` arm — the
// same §23.3 rule the implementation is read against. A setter-only
// class impl satisfies that obligation cleanly. Reading the iface through
// a reference is the impl's responsibility to forbid (already covered by
// #1192/#2461's read-of-write-only gate); the CONFORMANCE check itself
// admits the impl. Verifies by running — the impl's setter is exercised
// by `face.Name := 42`, the value is stored on `_n`, and the program
// returns 0 (the property is write-only so `face.Name` would be rejected
// if read; the test deliberately uses `:=` only).
// ===========================================================================
BEZEL_TEST(setter_only_readwrite_iface_property_2549,
           write_only_iface_setter_only_impl_accepted_runs) {
    int r = compileLocalAndRun2549(
        "module Local2549T5; "
        "public interface IWriteOnly2549T5 "
        "  property W: int "
        "    set => value; "
        "  end W; "
        "end IWriteOnly2549T5; "
        "public class SetterOnlyImpl2549T5: IWriteOnly2549T5 "
        "  public property W: int "
        "    set => 0; "
        "  end W; "
        "end SetterOnlyImpl2549T5; "
        "public class C "
        "  public static method Main(): int "
        "    using var obj := new SetterOnlyImpl2549T5(); "
        "    var face: IWriteOnly2549T5 := obj; "
        "    face.W := 42; "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (r == -1) return;  // toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T6 — FALSE-REJECT GUARD (LOCAL): a read-write iface property satisfied
// by a CORRECT read-write impl (full block with both `get` and `set`
// arms) is still ACCEPTED and RUNS. Guards against a fix that widens the
// getter check into a false REJECT of a candidate that DOES provide a
// getter — the canonical "shape is fine, both arms present" control.
// ===========================================================================
BEZEL_TEST(setter_only_readwrite_iface_property_2549,
           readwrite_iface_readwrite_impl_accepted_runs) {
    int r = compileLocalAndRun2549(
        "module Local2549T6; "
        "public interface IHasName2549T6 "
        "  property Name: int; "
        "end IHasName2549T6; "
        "public class ReadWrite2549T6: IHasName2549T6 "
        "  private _n: int := 7; "
        "  public property Name: int "
        "    get => _n; "
        "    set => _n := value; "
        "  end Name; "
        "end ReadWrite2549T6; "
        "public class C "
        "  public static method Main(): int "
        "    using var obj := new ReadWrite2549T6(); "
        "    var face: IHasName2549T6 := obj; "
        "    var v := face.Name; "
        "    face.Name := v + 10; "
        "    return face.Name - 17; "
        "  end Main; "
        "end C;");
    if (r == -1) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T7 — ACCEPT-CONTROL (IMPORTED): a CORRECT read-write impl of an IMPORTED
// read-write iface property still compiles, runs, and dispatches the
// expected value through the iface reference. Guards the IMPORTED-side
// `ifaceHasGetter`/`candidateHasGetter` lambdas against a fix that widens
// the imported-side fallback into a false REJECT of a correct read-write
// impl. (The setter-side T6 above covers the LOCAL case; this covers the
// IMPORTED-side shape that the lambda fallback specifically enables.)
// ===========================================================================
BEZEL_TEST(setter_only_readwrite_iface_property_2549,
           imported_iface_readwrite_impl_accepted_runs) {
    int r = compileImportAndRun2549(
        "module IfaceLib2549T7; "
        "public interface IHasName2549T7 "
        "  property Name: int; "
        "end IHasName2549T7;",
        "module App2549T7; import IfaceLib2549T7; "
        "public class ReadWrite2549T7: IHasName2549T7 "
        "  private _n: int := 33; "
        "  public property Name: int "
        "    get => _n; "
        "    set => _n := value; "
        "  end Name; "
        "end ReadWrite2549T7; "
        "public class C "
        "  public static method Main(): int "
        "    using var obj := new ReadWrite2549T7(); "
        "    var face: IHasName2549T7 := obj; "
        "    var v := face.Name; "
        "    face.Name := v + 7; "
        "    return face.Name - 40; "
        "  end Main; "
        "end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T8 — REJECT-CONTROL (LOCAL RECORD): a setter-only BLOCK impl of a
// read-write iface property on a RECORD is rejected by the same check.
// Records go through the same `checkAbstractImplementations` walker as
// classes (#2416 widened the function's outer loop to admit Record), so
// the candidate is a `PropertySymbol` on the record (not a positional
// field — those are read-only by §9.4 and would not satisfy the iface's
// "must have a getter" obligation either, but the path is the
// `concretePosFields` branch below, not the property-symbol branch this
// fix touches). The `record` kind in the diagnostic (`t->kind ==
// ast::TypeDeclKind::Record ? "record" : "class"` in
// `checkAbstractImplementations`'s `kindWord`) is asserted by
// T1-T7's class-shape tests indirectly; this T8 confirms the record-shape
// candidate is also rejected.
// ===========================================================================
BEZEL_TEST(setter_only_readwrite_iface_property_2549,
           local_record_setter_only_impl_rejected_11_2_23_3) {
    std::string err;
    bool ok = compileLocalReject2549(
        "module Local2549T8; "
        "public interface IHasName2549T8 "
        "  property Name: int; "
        "end IHasName2549T8; "
        "public record SetterOnly2549T8: IHasName2549T8 "
        "  public property Name: int "
        "    set => 0; "
        "  end Name; "
        "end SetterOnly2549T8; "
        "public class C public static method Main(): int => 0; end C;",
        err);
    BEZEL_EXPECT_TRUE(!ok);
    BEZEL_EXPECT_TRUE(err.find("property `Name` implements interface") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("setter-only implementation") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("must provide a getter") !=
                      std::string::npos);
}

}  // namespace bezel::module
