// fix #2380 (§11.2 / §12.5 / §2.8.2, v0.234): `Resolver::checkAbstractImplementations`'s
// `propType` lambda (the §11.2/§23 PROPERTY type-match check) read `ps->decl` as an
// `ast::PropertyDecl*` with no fallback, which is unconditionally null for any
// IMPORTED property — an interface property declared on an imported interface, or a
// property inherited from an imported base class. `propType` returned nullptr for
// such a property, which skipped the whole exact-type-match comparison via
// `if (!itype || !ctype) continue;` — a false ACCEPT of any property-type mismatch
// through an imported interface, admitting a wrongly-typed implementation that is
// then dispatched to and read at the wrong type through the interface reference.
//
// Filed as the SIXTH confirmed instance of #2374's audit pattern (an `astNode`/
// `decl`-only gate silently mishandling an imported symbol), and the direct
// PROPERTY sibling of #2373 (the METHOD half of the same function, same defect
// shape, landed earlier the same day as efa56ba).
//
// CONFIRMED SYMPTOM (verified by probe before writing this test, then mechanically
// by reverting the src/ fix and re-running this exact filter): pre-fix, a LOCAL
// class implementing an IMPORTED interface's `Name: int` property with a `string`-
// typed property compiled clean (exit 0) for both a read-only and a settable
// property. Read through the interface-typed reference (`b.Name` coerced to the
// interface's static `int` type), the call returned a garbage `int` reinterpreted
// from the `string`-returning body's actual result — observed `v=-943274556` (the
// SAME garbage value observed for #2373's method-side false accept, an artifact of
// this environment's register/stack layout, not a claim of determinism).
//
// A SECOND, independently-reachable shape of the exact same `propType` gap was
// found during verification (not named by the filed issue, which only pointed at
// the imported-INTERFACE case): `propType` is applied identically to BOTH sides of
// the comparison — `ips` (always from an interface reached by this function's
// ancestor BFS) and `cit->second` (from ANY non-interface node the SAME BFS
// reaches, which can just as well be a base CLASS declared in another module). A
// property inherited from an IMPORTED BASE CLASS, satisfying a LOCAL interface's
// property obligation with the wrong type, was ALSO silently accepted pre-fix —
// confirmed by probe. The fix (falling back to `ctx_.importedMemberType(ps)` in
// `propType` itself, mirroring the already-established fallback for imported
// fields/consts, ModuleLoader.cpp ~445) covers both shapes uniformly since both
// call sites share the one lambda; T3 below pins the second shape.
//
// A separate, PRE-EXISTING, CODEGEN-side null-itable-slot crash was found and is
// NOT fixed here (out of #2380's sema-only scope; filed as its own issue): a
// CORRECT (type-matching) property inherited from an IMPORTED base class and
// dispatched through an interface reference calls a null function pointer
// (SIGSEGV, faulting address 0x0) because `ensurePropertyAccessorFn`
// (LlvmBackend_Assign.cpp) also gates on `dynamic_cast<const ast::PropertyDecl*>
// (ps->decl)` with no fallback to the already-declared `importedPropGetterFns_`
// external stub (LlvmBackend_Setup.cpp's `declareImportedPropertyGetters`) — the
// SAME `astNode`/`decl`-only gate defect class, one layer down in codegen. T3's
// accept-control below therefore asserts COMPILATION only, not the runtime value,
// and documents the crash rather than exercising it (a crashing regression test
// would defeat the point of a green suite). The SAME-MODULE equivalent of T3's
// shape (a locally-declared base class's inherited property satisfying a local
// interface) was confirmed unaffected — it compiles AND runs correctly — so this
// is specifically a cross-module codegen gap, not a fix regression.

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

struct TempFile2380 {
    std::string path;
    explicit TempFile2380(std::string p) : path(std::move(p)) {}
    ~TempFile2380() { std::remove(path.c_str()); }
};

void writeSource2380(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2380(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile B importing A, capturing the driver's stderr (fd 2) into `err`. Mirrors
// xmod_abstract_impl_subst_2373_test.cpp's compileImportCaptureErr2373 (itself
// mirroring xmod_generic_method_arg_marshalling_1943_test.cpp).
bool compileImportCaptureErr2380(const std::string& bSrc,
                                 const std::string& bExe,
                                 const std::string& aObj, std::string& err) {
    namespace fs = std::filesystem;
    std::string errPath = (fs::temp_directory_path() /
                           ("bz2380_err_" + std::to_string(std::rand()) +
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

// Compile B importing A -> exe, run, return exit code (-1 on compile/link/run
// failure, -2 if the dependency itself could not be produced).
int compileImportAndRun2380(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2380_" + std::to_string(std::rand());
    TempFile2380 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2380 aObj((dir / (stem + "_a.o")).string());
    writeSource2380(aSrc.path, libSrc);
    if (!compileToObj2380(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2380 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2380 bExe((dir / stem).string());
    writeSource2380(bSrc.path, appSrc);
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

// True iff B (importing A) compiles clean.
bool compileImportSucceeds2380(const std::string& libSrc,
                               const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2380_ok_" + std::to_string(std::rand());
    TempFile2380 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2380 aObj((dir / (stem + "_a.o")).string());
    writeSource2380(aSrc.path, libSrc);
    if (!compileToObj2380(aSrc.path, aObj.path)) return false;
    if (!std::ifstream(aObj.path).good()) return false;

    TempFile2380 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2380 bExe((dir / (stem + "_b")).string());
    writeSource2380(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    return driver.compileFile(bSrc.path, bExe.path, deps).success;
}

struct ImportCapture2542 {
    bool dependencyCompiled = false;
    bool appCompiled = false;
    std::string error;
};

// Compile a source-text dependency, then compile an importer while capturing
// the importer's diagnostic text. Each reject test first asserts that the
// dependency genuinely compiled, so a malformed two-module setup cannot turn
// the expected importer rejection into a vacuous pass.
ImportCapture2542 compileImportSources2542(const std::string& libSrc,
                                           const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2542_" + std::to_string(std::rand());
    TempFile2380 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2380 aObj((dir / (stem + "_a.o")).string());
    writeSource2380(aSrc.path, libSrc);

    ImportCapture2542 out;
    out.dependencyCompiled =
        compileToObj2380(aSrc.path, aObj.path) &&
        std::ifstream(aObj.path).good();
    if (!out.dependencyCompiled) return out;

    TempFile2380 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2380 bExe((dir / (stem + "_b")).string());
    writeSource2380(bSrc.path, appSrc);
    out.appCompiled = compileImportCaptureErr2380(
        bSrc.path, bExe.path, aObj.path, out.error);
    return out;
}

}  // namespace

// ===========================================================================
// T1 — a READ-ONLY property implementing an IMPORTED interface's property with
// a DIFFERENT type is REJECTED (§11.2 / §23). Pre-fix: `propType(ips)` read
// `ips->decl` (an imported PropertySymbol's decl is always null — ModuleLoader
// never attaches an ast::PropertyDecl to a body-less imported declaration), so
// `itype` was null and the whole comparison was skipped via
// `if (!itype || !ctype) continue;`.
// ===========================================================================
BEZEL_TEST(xmod_abstract_impl_prop_2380,
           readonly_property_type_mismatch_rejected_11_2) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2380_t1_" + std::to_string(std::rand());
    TempFile2380 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2380 aObj((dir / (stem + "_a.o")).string());
    writeSource2380(aSrc.path,
                    "module PropIfaceLib2380T1; "
                    "public interface IHasName2380T1 "
                    "  property Name: int; "
                    "end IHasName2380T1;");
    BEZEL_EXPECT_TRUE(compileToObj2380(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;  // toolchain unavailable

    std::string bSrc =
        "module App2380T1; import PropIfaceLib2380T1; "
        "public class BadProp2380T1: IHasName2380T1 "
        "  private _n: string := \"x\"; "
        "  public property Name: string => _n; "
        "end BadProp2380T1; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2380 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2380 bExe((dir / (stem + "_b")).string());
    writeSource2380(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2380(bSrcFile.path, bExe.path, aObj.path, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("11.2") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("property") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("different type") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("IHasName2380T1") != std::string::npos);
}

// ===========================================================================
// T2 — a SETTABLE property implementing an IMPORTED interface's property with
// a DIFFERENT type is REJECTED (§11.2 / §23). Isolates the settable/block-form
// accessor shape from T1's expression-bodied read-only shape; per the A.4
// grammar (`PropertyDecl ::= ... "property" Identifier ":" Type PropertyBody`)
// getter and setter share one declared type, so this exercises the same
// `propType` gate rather than a distinct setter-only type.
// ===========================================================================
BEZEL_TEST(xmod_abstract_impl_prop_2380,
           settable_property_type_mismatch_rejected_11_2) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2380_t2_" + std::to_string(std::rand());
    TempFile2380 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2380 aObj((dir / (stem + "_a.o")).string());
    writeSource2380(aSrc.path,
                    "module PropIfaceLib2380T2; "
                    "public interface IHasName2380T2 "
                    "  property Name: int; "
                    "end IHasName2380T2;");
    BEZEL_EXPECT_TRUE(compileToObj2380(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2380T2; import PropIfaceLib2380T2; "
        "public class BadProp2380T2: IHasName2380T2 "
        "  private _n: string := \"x\"; "
        "  public property Name: string "
        "    get => _n; "
        "    set => _n := value; "
        "  end Name; "
        "end BadProp2380T2; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2380 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2380 bExe((dir / (stem + "_b")).string());
    writeSource2380(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2380(bSrcFile.path, bExe.path, aObj.path, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("11.2") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("property") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("different type") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("IHasName2380T2") != std::string::npos);
}

// ===========================================================================
// T3 — the SECOND shape found during verification: a property INHERITED FROM
// AN IMPORTED BASE CLASS, satisfying a LOCAL interface's property obligation
// with a DIFFERENT type, is REJECTED. `propType` is applied identically to
// `cit->second` (the "concrete" witness) as to `ips` (the interface member);
// `cit->second` can be populated from ANY non-interface node the ancestor BFS
// reaches, including a base class declared in another module — so this side
// needed the same fallback, independently of T1/T2's imported-INTERFACE shape.
// ===========================================================================
BEZEL_TEST(xmod_abstract_impl_prop_2380,
           property_inherited_from_imported_base_class_mismatch_rejected_11_2) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2380_t3_" + std::to_string(std::rand());
    TempFile2380 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2380 aObj((dir / (stem + "_a.o")).string());
    writeSource2380(aSrc.path,
                    "module BasePropLib2380T3; "
                    "public class BaseWithProp2380T3 "
                    "  public property Name: int => 1; "
                    "end BaseWithProp2380T3;");
    BEZEL_EXPECT_TRUE(compileToObj2380(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2380T3; import BasePropLib2380T3; "
        "public interface IHasName2380T3 "
        "  property Name: string; "
        "end IHasName2380T3; "
        "public class Derived2380T3: BaseWithProp2380T3, IHasName2380T3 "
        "end Derived2380T3; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2380 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2380 bExe((dir / (stem + "_b")).string());
    writeSource2380(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2380(bSrcFile.path, bExe.path, aObj.path, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("11.2") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("property") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("different type") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("IHasName2380T3") != std::string::npos);
}

// ===========================================================================
// T4 — REJECT-CONTROL: a genuinely MISSING implementation of an imported
// interface's property is still rejected with the pre-existing §11.2/§23
// diagnostic ("does not implement interface property"). Guards that widening
// WHERE the interface property's type is read from does not also widen WHAT
// counts as satisfying the obligation in the first place.
// ===========================================================================
BEZEL_TEST(xmod_abstract_impl_prop_2380,
           missing_property_implementation_still_rejected_11_2) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2380_t4_" + std::to_string(std::rand());
    TempFile2380 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2380 aObj((dir / (stem + "_a.o")).string());
    writeSource2380(aSrc.path,
                    "module PropIfaceLib2380T4; "
                    "public interface IHasName2380T4 "
                    "  property Name: int; "
                    "end IHasName2380T4;");
    BEZEL_EXPECT_TRUE(compileToObj2380(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2380T4; import PropIfaceLib2380T4; "
        "public class NoImpl2380T4: IHasName2380T4 end NoImpl2380T4; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2380 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2380 bExe((dir / (stem + "_b")).string());
    writeSource2380(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2380(bSrcFile.path, bExe.path, aObj.path, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("11.2") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("does not implement") != std::string::npos);
}

// ===========================================================================
// T5 — ACCEPT + RUN + VALUE: a correct READ-ONLY implementation of an
// imported READ-ONLY interface property still compiles AND dispatches to the
// right value across the module boundary.
//
// Updated by #2542 (§11.2 / §23.3, v0.236): this control originally omitted
// `readonly` from the interface declaration, accidentally pinning the bug — a
// getter-only member does not satisfy a read-write interface property. Keep the
// intended getter-only-vs-readonly control by spelling the interface obligation
// correctly.
// ===========================================================================
BEZEL_TEST(xmod_abstract_impl_prop_2380,
           readonly_correct_implementation_runs_and_dispatches) {
    int r = compileImportAndRun2380(
        "module PropIfaceLib2380T5; "
        "public interface IHasName2380T5 "
        "  readonly property Name: int; "
        "end IHasName2380T5;",
        "module App2380T5; import PropIfaceLib2380T5; "
        "public class GoodProp2380T5: IHasName2380T5 "
        "  private _n: int := 77; "
        "  public property Name: int => _n; "
        "end GoodProp2380T5; "
        "public class C "
        "  public static method Main(): int "
        "    using var b: IHasName2380T5 := new GoodProp2380T5(); "
        "    return b.Name - 77; "
        "  end Main; "
        "end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T6 — RESTORED to a value-asserting RUN by fix #2406 (§23/§11.2/§2.8.2/
// §2.8.3 rule 4, v0.235); the accept-control retirement below is now
// history, kept for context.
//
// NOTE added while investigating #2401 (§12.1/§11.2/§23/§2.8.2/§2.8.3 rule 4,
// v0.235 — `ifacePropertyGetIndex_`/`ifacePropertySetIndex_` not mirrored for
// IMPORTED interfaces): this test previously asserted `r == 0` for
// `b.Name := 55; return b.Name - 55;`. Before #2401's fix, `b.Name` (a GET
// through the imported `IHasName2380T6` interface reference) did not dispatch
// through the itable at all — #2401's own filed root cause — so this
// assertion's "pass" was not evidence of correct dispatch. After #2401's fix,
// the GET genuinely dispatched through the itable but the SET (`b.Name :=
// 55`) was visibly a no-op (`_n` stayed at `0`, observed exit `201`), traced
// to `Emitter::propertyHasSetter(ps)` (`LlvmBackend_impl.hpp`) unconditionally
// returning `false` for an imported PropertySymbol (`ps->decl` is
// unconditionally null for one) — so no setter itable slot was ever reserved
// for the imported interface's read-write property. The value assertion was
// retired to an accept-control pending that fix.
//
// fix #2406: `propertyHasSetter` now falls back to
// `ctx_.importedPropertyHasSetter(ps)` — the `MetaMember::hasSetter` bit
// #2400 added — instead of a naive `modifiers`-only heuristic (confirmed
// insufficient: `readonly` is a compile error on any block-form property
// regardless of setter presence, so a get-only block form and a read-write
// auto form are indistinguishable by modifiers alone). A companion fix to
// `Emitter::lowerVirtualPropertySet` (the actual SET dispatch, which had the
// identical `!pd`-gated bailout on the value type it needs) was also
// required — mirroring #503's pre-existing fallback on the GET side
// (`lowerVirtualPropertyGet`) — confirmed necessary by probe: fixing
// `propertyHasSetter` alone reserved the slot but the write still silently
// no-op'd, the exact #2387-lesson shape (verify the runtime OUTCOME, not
// that a fix compiles). `b.Name := 55; return b.Name - 55;` now correctly
// returns `0`.
// ===========================================================================
BEZEL_TEST(xmod_abstract_impl_prop_2380,
           settable_correct_implementation_runs_and_dispatches) {
    int r = compileImportAndRun2380(
        "module PropIfaceLib2380T6; "
        "public interface IHasName2380T6 "
        "  property Name: int; "
        "end IHasName2380T6;",
        "module App2380T6; import PropIfaceLib2380T6; "
        "public class GoodProp2380T6: IHasName2380T6 "
        "  private _n: int := 0; "
        "  public property Name: int "
        "    get => _n; "
        "    set => _n := value; "
        "  end Name; "
        "end GoodProp2380T6; "
        "public class C "
        "  public static method Main(): int "
        "    using var b: IHasName2380T6 := new GoodProp2380T6(); "
        "    b.Name := 55; "
        "    return b.Name - 55; "
        "  end Main; "
        "end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T7 — ACCEPT-CONTROL (compiles only — see the file header for why the value
// is not asserted): a CORRECT getter-only property inherited from an imported
// base class satisfies a local READ-ONLY interface obligation with the RIGHT
// type and still compiles clean. Guards against the fix over-widening
// `propType`'s fallback into a false REJECT of a correct T3-shaped witness.
// Updated by #2542 (§11.2 / §23.3, v0.236): the interface originally omitted
// `readonly`, accidentally pinning the imported-candidate accessor-presence
// bug this fix closes. Does NOT run the program: this shape hits the separate,
// PRE-EXISTING codegen null-itable-slot crash documented in the file header
// (filed as its own issue, out of #2380's sema-only scope) — asserting only
// compilation keeps this a regression guard on the SEMA fix without asserting
// on the separately-broken codegen path.
// ===========================================================================
BEZEL_TEST(xmod_abstract_impl_prop_2380,
           property_inherited_from_imported_base_class_correct_still_compiles) {
    bool ok = compileImportSucceeds2380(
        "module BasePropLib2380T7; "
        "public class BaseWithProp2380T7 "
        "  public property Name: int => 1; "
        "end BaseWithProp2380T7;",
        "module App2380T7; import BasePropLib2380T7; "
        "public interface IHasName2380T7 "
        "  readonly property Name: int; "
        "end IHasName2380T7; "
        "public class Derived2380T7: BaseWithProp2380T7, IHasName2380T7 "
        "end Derived2380T7; "
        "public class C public static method Main(): int => 0; end C;");
    BEZEL_EXPECT_TRUE(ok);
}

// ===========================================================================
// #2542 (§11.2 / §23.3 / §2.8.2 clause 1 / §2.8.3 rule 4,
// v0.236): imported PropertySymbols have no ast::PropertyDecl. The named-
// property accessor-presence check used to skip whenever either the interface
// obligation or its candidate was imported, even though ModuleLoader already
// publishes MetaMember::hasSetter through importedPropertyHasSetter. These
// reject cases assert the specific conformance diagnostic text, not merely a
// failed compile, and first prove that the dependency artifact was produced.
// ===========================================================================
BEZEL_TEST(xmod_iface_property_accessor_2542,
           imported_readwrite_iface_getter_only_class_rejected_11_2_23_3) {
    auto r = compileImportSources2542(
        "module PropIfaceLib2542T1; "
        "public interface IProp2542T1 "
        "  property Name: int; "
        "end IProp2542T1;",
        "module PropIfaceApp2542T1; import PropIfaceLib2542T1; "
        "public class GetterOnly2542T1: IProp2542T1 "
        "  private _n: int := 7; "
        "  public property Name: int => _n + 1; "
        "end GetterOnly2542T1; "
        "public class C public static method Main(): int => 0; end C;");
    BEZEL_EXPECT_TRUE(r.dependencyCompiled);
    if (!r.dependencyCompiled) return;
    BEZEL_EXPECT_TRUE(!r.appCompiled);
    BEZEL_EXPECT_TRUE(r.error.find("property `Name` implements interface") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(r.error.find("getter-only implementation") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(r.error.find("declared read-write") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.error.find("must provide a setter") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(r.error.find("11.2") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.error.find("23.3") != std::string::npos);
}

// The same imported obligation implemented by an explicit getter-only RECORD
// property. This exercises the identical concreteProps branch with `t` a
// record, per §9.6's rule that records may implement interfaces.
BEZEL_TEST(xmod_iface_property_accessor_2542,
           imported_readwrite_iface_getter_only_record_rejected_9_6_11_2) {
    auto r = compileImportSources2542(
        "module PropIfaceLib2542T2; "
        "public interface IProp2542T2 "
        "  property Name: int; "
        "end IProp2542T2;",
        "module PropIfaceApp2542T2; import PropIfaceLib2542T2; "
        "public record GetterOnly2542T2(X: int): IProp2542T2 "
        "  public property Name: int => X; "
        "end GetterOnly2542T2; "
        "public class C public static method Main(): int => 0; end C;");
    BEZEL_EXPECT_TRUE(r.dependencyCompiled);
    if (!r.dependencyCompiled) return;
    BEZEL_EXPECT_TRUE(!r.appCompiled);
    BEZEL_EXPECT_TRUE(r.error.find("getter-only implementation") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(r.error.find("declared read-write") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.error.find("must provide a setter") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(r.error.find("11.2") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.error.find("23.3") != std::string::npos);
}

// A positional record field is represented as a FieldSymbol rather than a
// PropertySymbol and reaches the sibling concretePosFields branch. Its
// generated §9.1/§9.4 property is always getter-only, so an IMPORTED
// read-write obligation must be rejected there too.
BEZEL_TEST(xmod_iface_property_accessor_2542,
           imported_readwrite_iface_positional_record_rejected_9_1_9_4_11_2) {
    auto r = compileImportSources2542(
        "module PropIfaceLib2542T3; "
        "public interface IProp2542T3 "
        "  property Name: int; "
        "end IProp2542T3;",
        "module PropIfaceApp2542T3; import PropIfaceLib2542T3; "
        "public record Positional2542T3(Name: int): IProp2542T3 "
        "end Positional2542T3; "
        "public class C public static method Main(): int => 0; end C;");
    BEZEL_EXPECT_TRUE(r.dependencyCompiled);
    if (!r.dependencyCompiled) return;
    BEZEL_EXPECT_TRUE(!r.appCompiled);
    BEZEL_EXPECT_TRUE(r.error.find("positional field (§9.1)") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(r.error.find("always getter-only") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.error.find("declared read-write") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.error.find("must provide a setter") !=
                      std::string::npos);
}

// The other side of the old `!ipd || !cpd` gate: a LOCAL read-write interface
// obligation satisfied by a getter-only property inherited from an IMPORTED
// base class. `cpd` is null, so candidate accessor presence must use the same
// imported metadata fallback.
BEZEL_TEST(xmod_iface_property_accessor_2542,
           getter_only_imported_base_candidate_rejected_11_2_23_3) {
    auto r = compileImportSources2542(
        "module PropBaseLib2542T4; "
        "public class GetterOnlyBase2542T4 "
        "  public property Name: int => 7; "
        "end GetterOnlyBase2542T4;",
        "module PropIfaceApp2542T4; import PropBaseLib2542T4; "
        "public interface IProp2542T4 "
        "  property Name: int; "
        "end IProp2542T4; "
        "public class Derived2542T4: GetterOnlyBase2542T4, IProp2542T4 "
        "end Derived2542T4; "
        "public class C public static method Main(): int => 0; end C;");
    BEZEL_EXPECT_TRUE(r.dependencyCompiled);
    if (!r.dependencyCompiled) return;
    BEZEL_EXPECT_TRUE(!r.appCompiled);
    BEZEL_EXPECT_TRUE(r.error.find("getter-only implementation") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(r.error.find("declared read-write") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.error.find("must provide a setter") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(r.error.find("11.2") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.error.find("23.3") != std::string::npos);
}

// False-reject guard for the positional-record branch: an imported READ-ONLY
// obligation is fully satisfied by §9.1's generated getter-only property and
// still dispatches the expected value across the module boundary.
BEZEL_TEST(xmod_iface_property_accessor_2542,
           imported_readonly_iface_positional_record_runs_correct_value) {
    int r = compileImportAndRun2380(
        "module PropIfaceLib2542T5; "
        "public interface IProp2542T5 "
        "  readonly property Name: int; "
        "end IProp2542T5;",
        "module PropIfaceApp2542T5; import PropIfaceLib2542T5; "
        "public record Positional2542T5(Name: int): IProp2542T5 "
        "end Positional2542T5; "
        "public class C "
        "  public static method Main(): int "
        "    var value := new Positional2542T5(42); "
        "    var face: IProp2542T5 := value; "
        "    return face.Name; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 42);
}

}  // namespace bezel::module
