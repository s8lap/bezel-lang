// Regression tests for #4651 — §22.16's SETTER row of the instrumentation set
// did not survive a module boundary.
//
// AUTHORITY.
//
// spec §22.16 (v0.307), "Class invariants" — the instrumentation set:
//     "- **Entry and normal exit** of every `public` **setter** — block-form,
//        auto-property, indexer, `set(owned)` alike."
//
// and, two paragraphs later, the shape the instrumentation must take:
//     "Every instrumented public member compiles to a checked face (entry
//      checks → unchecked face → exit checks) and an unchecked face (the
//      body); call sites with syntactic receiver `this` bind to the unchecked
//      face — statically decidable."
//
// spec §2.8.2 clause 1 (v0.307) makes an imported member's use behave as a
// same-module one does; §13.6 governs accessor visibility.
//
// THE DEFECT. #4614(b) shipped the setter row by emitting the checks at the
// CALL SITE, around the setter body `lowerPropertySetValue` inlines there.
// That is a checked face the call site synthesises for itself, and it exists
// only where the body is inlined — inside the declaring module. For an
// IMPORTED property `computedSetterOf` returns null (an imported
// `PropertySymbol` carries no `ast::PropertyDecl`), so the importer calls the
// exporter's external setter stub `bz$<Class>$<Prop>$propset` instead — and
// the exporter emitted that stub as a bare forwarder, with no check in it.
// The instrumentation was structurally invisible across the boundary.
//
// MEASURED on f644098f with `--checked-contracts` on BOTH halves of a real
// two-module build, on #4614(b)'s own repro: the class defined in the app
// returned 99 (ContractViolationException, correct) and the *identical* class
// imported returned 42 — no check at all. The boundary was the whole
// difference, which is why rows 1-2 are paired with the same-module control.
//
// THE FIX (LlvmBackend_Setup.cpp `emitExportedPropertySetters`). The exported
// stub BECOMES the setter's §22.16 checked face: entry check → the inner
// InternalLinkage accessor (the unchecked face, unchanged) → exit check. The
// two faces the spec asks for already existed as two symbols; nothing is
// minted and no symbol moves. The accessor function itself is deliberately
// NOT instrumented — it cannot see the receiver's syntax, so it would also
// check the `this`-receiver stores §22.16 exempts.
//
// MODE POLICY, PINNED HERE ON PURPOSE. The check lives in the exporter's
// object, so the EXPORTER's `--checked-contracts` governs it. That is not a
// new policy invented by this fix: it is exactly what §22.16's public-METHOD
// row has done since #2941, because a method's entry/exit checks live in the
// method body the exporter compiled. Measured on this fix's own tree, the two
// rows' 2x2 mode grids are now identical cell for cell:
//
//     lib / app             setter row   method row
//     checked / checked        99           99
//     checked / release        99           99
//     release / checked        42           42
//     release / release        42           42
//
// Rows 5-7 pin that grid for the setter, so a later change that moves the
// check to the importer (which would make a release-mode library's setters
// check while its methods do not) fails here instead of silently landing.
//
// RECORDED RESIDUAL, deliberately NOT pinned as a row. A class in the
// IMPORTING module that derives from an imported class and writes
// `this.P := v` through the inherited public property binds this checked face
// and is therefore checked where §22.16's `this`-reentrancy exemption applies.
// That is #4597's own recorded residual ("a `this`-receiver call to an
// imported instrumented method keeps its ... checked-face binding") one member
// kind wider, and it was MEASURED to behave identically on the method row on
// this same tree — closing it needs an unchecked face exported under its own
// always-emitted symbol, for both rows at once. Pinning the wrong answer as an
// expectation would make that fix look like a regression.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile4651 {
    std::string path;
    explicit TempFile4651(std::string p) : path(std::move(p)) {}
    ~TempFile4651() { std::remove(path.c_str()); }
};

void writeSource4651(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Build `libSrc` as a `-c` library object under `libOpts`, then `mainSrc` as
// an executable against it under `appOpts`, link, run, and return the exit
// code. The two option sets are SEPARATE because §22.16's checked-contracts
// bit is a per-compilation flag and this file's whole subject is which side of
// the boundary it is read on.
//
// Distinct negative sentinels (mirroring #4592's and #4599's harnesses), so a
// broken scaffold can never read as a pass:
//   -2  the library did not build (or produced no object)
//   -3  the app did not build/link
//   -4  the program could not be run to a normal exit
int buildAndRun4651(const std::string& libSrc, const std::string& mainSrc,
                    const codegen::CodegenOptions& libOpts,
                    const codegen::CodegenOptions& appOpts, const char* tag) {
    const fs::path& dir = wtScratchDir(tag);
    const std::string id = unique_stem(tag);
    std::vector<bezel::Dependency> deps;
    bezel::Driver driver;

    TempFile4651 libSrcF((dir / (id + "_lib.bzl")).string());
    TempFile4651 libObjF((dir / (id + "_lib.o")).string());
    writeSource4651(libSrcF.path, libSrc);
    if (!driver.compileObject(libSrcF.path, libObjF.path, deps, libOpts)
             .success)
        return -2;
    if (!std::ifstream(libObjF.path).good()) return -2;
    bezel::Dependency d;
    d.metadataPath = libObjF.path;
    d.objectPath = libObjF.path;
    deps.push_back(d);

    TempFile4651 appSrcF((dir / (id + "_app.bzl")).string());
    TempFile4651 appExeF((dir / id).string());
    writeSource4651(appSrcF.path, mainSrc);
    if (!bezel::test::compileFileForTest(driver, appSrcF.path, appExeF.path,
                                         deps, appOpts)
             .success)
        return -3;

    int status = bezel::test::runProgramForTest(appExeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -4;
}

// The same-module control: ONE compilation, no artifact boundary at all.
int buildSingleAndRun4651(const std::string& src,
                          const codegen::CodegenOptions& opts,
                          const char* tag) {
    const fs::path& dir = wtScratchDir(tag);
    const std::string id = unique_stem(tag);
    std::vector<bezel::Dependency> deps;
    bezel::Driver driver;
    TempFile4651 srcF((dir / (id + "_one.bzl")).string());
    TempFile4651 exeF((dir / id).string());
    writeSource4651(srcF.path, src);
    if (!bezel::test::compileFileForTest(driver, srcF.path, exeF.path, deps,
                                         opts)
             .success)
        return -3;
    int status = bezel::test::runProgramForTest(exeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -4;
}

codegen::CodegenOptions checked4651() {
    codegen::CodegenOptions o;
    o.checkedContracts = true;  // exactly what `bezelc --checked-contracts` sets
    return o;
}

codegen::CodegenOptions release4651() {
    return codegen::CodegenOptions{};  // §22.16's release default
}

codegen::CodegenOptions checkedBoth4651() {
    codegen::CodegenOptions o;
    o.checkedContracts = true;
    o.checkedDisposal = true;
    return o;
}

// ── Fixtures ────────────────────────────────────────────────────────────────
//
// #4614(b)'s own repro, split across a module boundary. The block-form setter
// stores whatever it is given, so the store drives the instance into the state
// `invariant(k > 0);` rejects and the violation is observable at the setter's
// NORMAL EXIT.
const char* kLib4651 =
    "module SetLib4651; "
    "public class CS4651 "
    "  public k: int := 1; "
    "  invariant(k > 0); "
    "  public constructor(v: int) k := v; end constructor; "
    "  public property Q: int "
    "    get => k; "
    "    set => do k := value; end; "
    "  end Q; "
    "end CS4651; ";

// EXIT boundary: entered conforming (k = 5), left violating (k = -3).
const char* kAppExitViolate4651 =
    "module SetAppExit4651; "
    "import SetLib4651; "
    "class MainSE4651 "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new CS4651(5); "
    "    try "
    "      c.Q := 0 - 3; "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainSE4651; ";

// ENTRY boundary, ISOLATED. The instance is driven into the rejected state
// through a public FIELD (never an instrumentation point — §22.16 names
// methods, setters and construction exit only), and the setter RESTORES the
// invariant before it returns. Only an ENTRY check can observe this, so a fix
// that emitted just one of the two boundaries leaves this row red.
const char* kLibEntry4651 =
    "module SetLibEntry4651; "
    "public class CE4651 "
    "  public k: int := 1; "
    "  invariant(k > 0); "
    "  public constructor(v: int) k := v; end constructor; "
    "  public property Q: int "
    "    get => k; "
    "    set => do k := 5; end; "
    "  end Q; "
    "end CE4651; ";

const char* kAppEntryViolate4651 =
    "module SetAppEntry4651; "
    "import SetLibEntry4651; "
    "class MainSN4651 "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new CE4651(5); "
    "    try "
    "      c.k := 0 - 1; "
    "      c.Q := 7; "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainSN4651; ";

// The SAME-MODULE control for the exit-boundary row: byte-for-byte the same
// class and the same statement, one compilation. #4614(b) made this 99; if it
// ever stops being 99 the cross-module rows are measuring the wrong thing.
const char* kSingleExitViolate4651 =
    "module SetOne4651; "
    "class CS4651 "
    "  public k: int := 1; "
    "  invariant(k > 0); "
    "  public constructor(v: int) k := v; end constructor; "
    "  public property Q: int "
    "    get => k; "
    "    set => do k := value; end; "
    "  end Q; "
    "end CS4651; "
    "class MainSO4651 "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new CS4651(5); "
    "    try "
    "      c.Q := 0 - 3; "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainSO4651; ";

// CONFORMING cross-module store: §22.16's clauses are pure, so a satisfied
// invariant is unobservable. Returns the value the setter stored (7), read
// back through the exporter's getter — proving the checked face still forwards
// to the real accessor body and does not fault a legal write.
const char* kAppConform4651 =
    "module SetAppOk4651; "
    "import SetLib4651; "
    "class MainSC4651 "
    "  public static method Main(): int "
    "    using var c := new CS4651(5); "
    "    c.Q := 7; "
    "    return c.Q; "
    "  end Main; "
    "end MainSC4651; ";

// A class with NO invariant member: §22.16 instruments nothing, so the
// exported stub must stay the bare forwarder it has always been and a store
// that would be "violating" for CS4651 must simply store.
const char* kLibNoInv4651 =
    "module SetLibNoInv4651; "
    "public class CN4651 "
    "  public k: int := 1; "
    "  public constructor(v: int) k := v; end constructor; "
    "  public property Q: int "
    "    get => k; "
    "    set => do k := value; end; "
    "  end Q; "
    "end CN4651; ";

const char* kAppNoInv4651 =
    "module SetAppNoInv4651; "
    "import SetLibNoInv4651; "
    "class MainNI4651 "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new CN4651(5); "
    "    try "
    "      c.Q := 0 - 3; "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainNI4651; ";

// §28.2 ordering fixture. `CD4651`'s `destructor` zeroes the very field its
// own `invariant(k > 0);` reads — §22.16 says disposal "legitimately tears the
// state down" — so a public setter invoked on the DISPOSED instance is a point
// where §28.2's guard and §22.16's entry check disagree about which exception
// to raise. The public-METHOD row answers DisposedAccessException (the
// disposed-flag test precedes the invariant in `emitFunctionBody`), so the
// setter's checked face must answer the same.
const char* kLibDisposed4651 =
    "module SetLibDisp4651; "
    "public class CD4651 "
    "  public k: int := 1; "
    "  invariant(k > 0); "
    "  public constructor(v: int) k := v; end constructor; "
    "  destructor k := 0; end destructor; "
    "  public property Q: int "
    "    get => k; "
    "    set => do k := value; end; "
    "  end Q; "
    "end CD4651; ";

const char* kAppDisposed4651 =
    "module SetAppDisp4651; "
    "import SetLibDisp4651; "
    "class MainSD4651 "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    var c := new CD4651(5); "
    "    c.Dispose(); "
    "    try "
    "      c.Q := 7; "
    "      r := 42; "
    "    catch DisposedAccessException => r := 55; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainSD4651; ";

}  // namespace

// ===========================================================================
// ★ Row 1 — THE BUG. The setter's NORMAL-EXIT boundary, across a real
// artifact boundary, `--checked-contracts` on both halves.
// Pre-fix 42 (no check reached the imported setter at all); §22.16 requires 99.
// ===========================================================================
BEZEL_TEST(xmod_setter_invariant_checked_face_4651,
           imported_public_setter_exit_boundary_fires_spec_22_16) {
    const int rc = buildAndRun4651(kLib4651, kAppExitViolate4651, checked4651(),
                                   checked4651(), "wt-4651-exit");
    BEZEL_EXPECT_EQ(99, rc);
}

// ===========================================================================
// ★ Row 2 — the ENTRY boundary, isolated (the setter restores the invariant
// before it returns, so only an entry check can see the violation).
// Pre-fix 42; §22.16 requires 99. Pinned SEPARATELY from row 1 so a fix that
// emits only one of the two boundaries cannot pass this file.
// ===========================================================================
BEZEL_TEST(xmod_setter_invariant_checked_face_4651,
           imported_public_setter_entry_boundary_fires_spec_22_16) {
    const int rc = buildAndRun4651(kLibEntry4651, kAppEntryViolate4651,
                                   checked4651(), checked4651(),
                                   "wt-4651-entry");
    BEZEL_EXPECT_EQ(99, rc);
}

// ===========================================================================
// Row 3 — the SAME-MODULE control for row 1: identical class, identical
// statement, one compilation. 99 on both sides of the fix (#4614(b) shipped
// it). Its job is to establish that the module boundary — and nothing else in
// the program — was the difference row 1 measures.
// ===========================================================================
BEZEL_TEST(xmod_setter_invariant_checked_face_4651,
           same_module_setter_exit_boundary_control_spec_22_16) {
    const int rc = buildSingleAndRun4651(kSingleExitViolate4651, checked4651(),
                                         "wt-4651-one");
    BEZEL_EXPECT_EQ(99, rc);
}

// ===========================================================================
// Row 4 — CONFORMING control. §22.16's clauses are pure, so a satisfied
// invariant is unobservable: the store must land and read back. 7 on both
// sides. Without this row the checked face could fault every cross-module
// store and rows 1-2 would still be green.
// ===========================================================================
BEZEL_TEST(xmod_setter_invariant_checked_face_4651,
           imported_public_setter_conforming_store_is_unobservable_spec_22_16) {
    const int rc = buildAndRun4651(kLib4651, kAppConform4651, checked4651(),
                                   checked4651(), "wt-4651-ok");
    BEZEL_EXPECT_EQ(7, rc);
}

// ===========================================================================
// Row 5 — RELEASE on both halves. §22.16's instrumentation is
// checked-contracts-only (#4386), so the violating store must complete: 42.
// 42 on both sides of the fix.
// ===========================================================================
BEZEL_TEST(xmod_setter_invariant_checked_face_4651,
           imported_public_setter_release_mode_emits_no_check_spec_22_16) {
    const int rc = buildAndRun4651(kLib4651, kAppExitViolate4651, release4651(),
                                   release4651(), "wt-4651-rel");
    BEZEL_EXPECT_EQ(42, rc);
}

// ===========================================================================
// Row 6 — MIXED: a RELEASE library consumed by a CHECKED app. The check lives
// in the exporter's object and the exporter was compiled without it, so no
// check runs: 42. This is the same answer §22.16's public-METHOD row gives for
// the same mix (measured on this tree), and pinning it is what stops a later
// "fix" from moving the setter check to the importer and making a release
// library's setters check while its methods do not.
// ===========================================================================
BEZEL_TEST(xmod_setter_invariant_checked_face_4651,
           release_library_checked_app_follows_exporter_mode_spec_22_16) {
    const int rc = buildAndRun4651(kLib4651, kAppExitViolate4651, release4651(),
                                   checked4651(), "wt-4651-mix-rc");
    BEZEL_EXPECT_EQ(42, rc);
}

// ===========================================================================
// Row 7 — MIXED, the other way: a CHECKED library consumed by a RELEASE app.
// The exporter compiled the checked face, so the violation fires even though
// the consumer asked for nothing: 99. Again the public-method row's answer for
// the same mix.
// ===========================================================================
BEZEL_TEST(xmod_setter_invariant_checked_face_4651,
           checked_library_release_app_follows_exporter_mode_spec_22_16) {
    const int rc = buildAndRun4651(kLib4651, kAppExitViolate4651, checked4651(),
                                   release4651(), "wt-4651-mix-cr");
    BEZEL_EXPECT_EQ(99, rc);
}

// ===========================================================================
// Row 8 — a class with NO `invariant` member mints no face and changes no
// code: the exported stub stays the bare forwarder, and the store that would
// violate CS4651's clause simply stores. 42 on both sides. This pins the
// `hasClassInvariant` gate, without which every exported setter in every
// program would grow a (vacuous) check.
// ===========================================================================
BEZEL_TEST(xmod_setter_invariant_checked_face_4651,
           imported_setter_without_invariant_is_unchanged_spec_22_16) {
    const int rc = buildAndRun4651(kLibNoInv4651, kAppNoInv4651, checked4651(),
                                   checked4651(), "wt-4651-noinv");
    BEZEL_EXPECT_EQ(42, rc);
}

// ===========================================================================
// Row 9 — §28.2 ORDERING, with BOTH checked modes on. The destructor zeroes
// the field the invariant reads, so a public setter invoked on the disposed
// instance can raise either exception depending on which guard is emitted
// first. §28.2 ("a property or indexer accessor invocation" on a disposed
// instance raises DisposedAccessException) wins, because that is the order
// §22.16's public-METHOD row already uses — `emitFunctionBody` emits the
// disposed-flag test, then `requires`, then the invariant. 55.
//
// Pre-fix this row is ALSO 55 (the stub had no check at all), so it does not
// pin the bug — it pins the CHOICE the fix had to make, which was measured
// both ways: without the checked face's own §28.2 guard the answer is 99, and
// the method row's answer for the same shape is 55.
//
// The program leaks by design (§28.2 quarantines a disposed object rather
// than freeing it), which is why every row here runs through
// `runProgramForTest` rather than `std::system` — see #4488's test header.
// ===========================================================================
BEZEL_TEST(xmod_setter_invariant_checked_face_4651,
           disposed_instance_raises_disposed_access_not_contract_spec_28_2) {
    const int rc =
        buildAndRun4651(kLibDisposed4651, kAppDisposed4651, checkedBoth4651(),
                        checkedBoth4651(), "wt-4651-disp");
    BEZEL_EXPECT_EQ(55, rc);
}

}  // namespace bezel::module
