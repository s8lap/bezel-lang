// Regression coverage for the 2026-08-13 duplication-census row 12 finding
// (`classIndexerMatching` never applied the `indexerCandidateWireable` filter
// that `findIndexer` — the itable-route twin — already applies, LlvmBackend_impl.hpp
// / LlvmBackend_Assign.cpp) and the SEMA gap the reachability probe surfaced,
// tracked as issue #3641 and FIXED by this commit.
//
// §12.5 "Abstract classes and members", v0.286, states verbatim: "`abstract`
// on a member declares it without a body; every concrete subclass must
// override it." §23.4 "Indexers" states an indexer "participate[s] in
// `virtual`/`override`/`sealed`/`abstract` like methods" — no textual
// carve-out excluding indexers from §12.5's override obligation.
//
// PRIOR BEHAVIOUR (measured before this fix, pinned as a KNOWN GAP by an
// earlier revision of this file): a concrete class inheriting, without
// overriding, an IMPORTED abstract-class indexer compiled with ZERO
// diagnostics on both sides of the module boundary, then crashed at runtime
// — `d[5]` virtually dispatched through the concrete class's OWN vtable slot
// for the indexer, which was never filled (the only candidate
// `classIndexerMatching` finds on the base-class walk is the unimplemented
// abstract ancestor's indexer, which `ensureIndexerGetterFn`/
// `importedVirtualAccessorSlotFn` (LlvmBackend_Assign.cpp) decline via their
// own independent abstract-modifier guard, leaving the slot at its
// `ConstantPointerNull` default) — a direct SIGSEGV under the plain build.
//
// ROOT CAUSE (now fixed): `TypeResolution_Checks.cpp`'s abstract-member-
// implementation walk (`Resolver::checkAbstractImplementations`) tracked
// abstract METHOD/PROPERTY obligations (`abstractNames`) but unconditionally
// `continue`d past every indexer regardless of whether the declaring
// ancestor was a class or an interface — only the INTERFACE half was tracked
// (`ifaceIndexerObligations`, gated `if (curIsIface)`, backed by
// #2417/#2437/#2442/#2449's ~150-line matching machinery that already
// mirrors codegen's `findIndexer`/`indexerCandidateWireable` exactly). A
// CLASS-declared abstract indexer had no analogous obligation.
//
// THE FIX (fix #3641, this commit): the same `!curIsIface` push arm now
// records a CLASS-ancestor's own abstract indexer into
// `ifaceIndexerObligations` too (reusing its existing candidate-matching
// loop rather than inventing a parallel one — see that loop's comments for
// the full rationale), and the matching loop's `candWireable` lambda is
// tightened to reject an `abstract`-modified candidate on its LOCAL branch
// as well as its imported one (previously only the imported branch checked
// `abstract` — a same-module `class Derived: AB` where `AB` is declared in
// the SAME module would otherwise have let the base-class walk land back on
// `AB`'s own local abstract-indexer declaration and accept it as
// "implementing itself"). Both obligation shapes now report through the
// SAME diagnostic family the method/property case above already uses ("does
// not implement inherited abstract member `indexer`", citing §12.5), rather
// than the interface-obligation wording ("does not implement interface
// `X`'s indexer", §11.2 / §23.4).
//
// This is `src/sema/TypeResolution_Checks.cpp`. Note (from the per-fix
// review of `narrow (census row 12)`, Run 10 cycle 1): an earlier revision
// of a comment in this area misattributed the `abstract override`
// mutual-exclusivity check to `checkClassModifierCombinations`
// (TypeResolution.cpp:334) — the correct site is
// `Resolver::checkTypeStaticNotVirtual` (TypeResolution_Checks.cpp:467-499);
// `checkTypeModifierCombinations` (TypeResolution_Checks.cpp:~1781) is a
// third, differently-named function. None of the three is the function this
// commit touches (`checkAbstractImplementations`); recorded here because a
// wrong-but-plausible function name pointed at by a real line number is
// exactly the kind of thing the next reader spends time chasing.

#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFileRow12 {
    std::string path;
    explicit TempFileRow12(std::string p) : path(std::move(p)) {}
    ~TempFileRow12() { std::remove(path.c_str()); }
};

void writeSourceRow12(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObjRow12(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Redirects stderr to a scratch file for the duration of `fn`, returning
// whatever was written — the same capture technique
// xmod_iface_witness_from_imported_base_3194_test.cpp uses (`captureStderr3194`),
// duplicated here rather than shared because each hand-rolled module-test TU
// keeps its own anonymous-namespace helpers self-contained.
template <typename Fn>
std::string captureStderrRow12(Fn&& fn) {
    namespace fs = std::filesystem;
    std::string errPath = (fs::temp_directory_path() /
                           ("bzrow12_err_" + unique_stem("") + ".txt"))
                              .string();
    std::fflush(stderr);
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    fn();
    std::fflush(stderr);
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);
    std::ifstream in(errPath);
    std::string out((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return out;
}

}  // namespace

// ===========================================================================
// A concrete class (`Derived`) inherits, without overriding, an IMPORTED
// abstract class's (`AB`) abstract indexer. Per §12.5 ("every concrete
// subclass must override [an abstract member]") this is a compile error
// naming the missing override — the SAME diagnostic family §12.5's own BFS
// already reports for an abstract METHOD or named PROPERTY left
// unimplemented. FIXED (#3641): the app compile now fails with a §12.5
// diagnostic and no executable is produced, so the row-12 runtime SIGSEGV is
// no longer reachable through this shape at all.
// ===========================================================================
BEZEL_TEST(xmod_abstract_class_indexer_unoverridden_row12,
           unoverridden_imported_abstract_class_indexer_rejected) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bzrow12_");
    TempFileRow12 libSrc((dir / (stem + "_lib.bzl")).string());
    TempFileRow12 libObj((dir / (stem + "_lib.o")).string());
    writeSourceRow12(libSrc.path,
                     "module Row12Lib; "
                     "public abstract class AB "
                     "  public abstract property [i: int]: int; "
                     "end AB;");
    if (!compileToObjRow12(libSrc.path, libObj.path)) return;  // toolchain unavailable — skip
    if (!std::ifstream(libObj.path).good()) return;

    TempFileRow12 appSrc((dir / (stem + "_app.bzl")).string());
    TempFileRow12 appExe((dir / stem).string());
    writeSourceRow12(appSrc.path,
                     "module Row12App; import Row12Lib; "
                     "public class Derived: AB "
                     "end Derived; "
                     "public class C "
                     "  public static method Main(): int "
                     "    var d := new Derived(); "
                     "    return d[5]; "
                     "  end Main; "
                     "end C;");
    bool compiled = false;
    std::string err = captureStderrRow12([&] {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = libObj.path;
        d.objectPath = libObj.path;
        deps.push_back(d);
        compiled = bezel::test::compileFileForTest(driver, appSrc.path,
                                                    appExe.path, deps)
                       .success;
    });

    // fix #3641 / §12.5: the compile must now FAIL — no vtable slot is ever
    // left null for this shape, because the program never reaches codegen.
    BEZEL_EXPECT_FALSE(compiled);
    if (compiled) return;  // would be the regression this test exists to catch

    // Same diagnostic family checkAbstractImplementations already uses for
    // an unimplemented abstract METHOD/PROPERTY (see
    // sema_abs_impl__unimplemented_abstract_method_diagnoses.bzl), with
    // "indexer" standing in for the (nameless) member — see
    // TypeResolution_Checks.cpp's `!obligationIsInterface` branch.
    BEZEL_EXPECT_TRUE(
        err.find("does not implement inherited abstract member") !=
        std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("`indexer`") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("(§12.5)") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("Derived") != std::string::npos);

    // No executable was produced — the row-12 SIGSEGV this file used to pin
    // as a known gap is no longer reachable through this shape.
    BEZEL_EXPECT_FALSE(std::ifstream(appExe.path).good());
}

}  // namespace bezel::module
