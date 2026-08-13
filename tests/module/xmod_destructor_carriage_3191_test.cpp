// fix #3191 (§3.6 "Derived type properties" / §28.9 "Runtime-managed types" /
// §21.9 "Statement form of expressions" / §2.8.2 "Artifact interface"; v0.254) —
// an IMPORTED class's trivial-dispose verdict. Family M2 of umbrella #3152, a
// spin-off found while fixing #3189.
//
// WHAT THE DEFECT WAS. §3.6 makes trivial-dispose a DERIVED TYPE PROPERTY, and
// §2.8.2 requires the artifact interface to carry the derived properties: "carried
// as computed verdicts for non-generic public types", because — in §2.8.2's own
// words — "These properties are computed from structure the public surface does
// not show — private fields, destructor presence — yet §3.6 rule 3 makes them
// part of the public contract, and rules dispatch on them at the consumer: …
// trivial-dispose (§28.9) …. An interface that omitted them would force the
// consumer to either reject imported types at those rules or guess."
//
// The consumer was guessing, and guessing "trivial". A class's destructor is
// deliberately NOT part of the exported member list — `convertMember` skips it
// ("§13.3: not part of the public surface — Dispose() is") and there is no
// `MetaMemberKind::Destructor` — while both copies of §28.9's classification
// decide "does this class have a destructor?" by scanning
// `sema::TypeSymbol::members` for a `DestructorSymbol`. `ModuleLoader` builds one
// on exactly one path (the generic body re-parse), so EVERY NON-GENERIC IMPORTED
// CLASS scanned as "no destructor" and was classified trivial-dispose, whatever
// it declared.
//
// Reachable with NO generics and NO `owned` marker anywhere in either module.
//
// MEASURED PRE-FIX on cf84884 (`bezelc -c lib.bzl -o lib.o`, then
// `bezelc --import lib.o app.bzl`), against a character-identical single-module
// twin, for a `public class Res` declaring a destructor:
//
//   shape                        single-module twin      two-module
//   `new Derived(1);`  (§21.9)   REJECTED, §21.9 error   COMPILED SILENTLY
//   `new Res(1);`      (§21.9)   REJECTED, §21.9 error   COMPILED SILENTLY
//
// §2.8.2's carriage requirement is explicit that an implementation "must not
// alter the instantiation's semantics"; a HARD ERROR that fires in one artifact
// and not the other does exactly that.
//
// ⚠ THE OBSERVABLE IS THE DIAGNOSTIC, NOT A DISPOSAL COUNT. The filed issue and
// the lane brief both framed this as "the destructor never fires"; that is
// REFUTED by measurement. It does fire — the `NewExpr` arm already emits the free
// and `dynamicDispose` — so the pre-fix two-module program RAN and returned a
// disposal count of 1, identical to the post-fix remedy row. A counter-based test
// would therefore have PASSED on the broken compiler: vacuous. What is missing
// pre-fix is only §21.9's compile-time reject, so every reject row below asserts
// the diagnostic TEXT.
//
// ⚠ FOUR INDEPENDENT COPIES OF §28.9's CLASSIFICATION EXIST, and fixing the
// shared one was NOT sufficient — verified by measurement, not assumed. After
// `common::isNonTrivialDisposeClass` consulted the carried verdict, this test's
// T1 STILL compiled silently, because the §21.9 site in `ExprTypecheck.cpp` runs
// its OWN inline copy of the walk and never calls the shared predicate. (The
// other two are `Ownership.cpp`'s §28.3 auto-drop gate and `PureChecker.cpp`'s
// `isTrivialDisposeClass`.) Both copies on the reject path are fixed here;
// collapsing all four is #2666's subject and is deliberately not attempted.
//
// ⚠ THIS FIX MOVES IN THE REJECT DIRECTION, SO THE CONTROLS ARE HALF THE TEST.
// A set that only pinned what must be rejected would also pass on a compiler that
// rejected every bare discard of an imported class — which is exactly the
// over-rejection the issue warns against ("Do NOT close it by treating 'imported'
// as 'non-trivial': that over-rejects a bare discard of every imported
// trivial-dispose class, where the single-module twin compiles"). T3/T3b are that
// control and both assert a VALUE from a program that must still run.

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
#include "support/driver_run_support.hpp"  // #3156: the sanitizer-honouring compileFile
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile3191 {
    std::string path;
    explicit TempFile3191(std::string p) : path(std::move(p)) {}
    ~TempFile3191() { std::remove(path.c_str()); }
};

void writeSource3191(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Return codes, chosen so an admitted program's own exit status can never be
// confused with a rejected compile (mirrors #3182's runner):
//   -2  the DEPENDENCY did not compile — toolchain unavailable, skip.
//   -3  the app was REJECTED by the front-end.
//   -1  the app compiled but did not exit normally.
//   >=0 the app's own exit status.
constexpr int kDepUnavailable3191 = -2;
constexpr int kRejected3191 = -3;

// A reject row that asserted only `kRejected3191` would stay green if one of
// these inline sources later acquired a plain syntax error — green for the wrong
// reason. Capture stderr so every reject pins the actual §21.9 text.
class StderrCapture3191 {
public:
    explicit StderrCapture3191(std::string path) : path_(std::move(path)) {
        std::fflush(stderr);
        saved_ = ::dup(STDERR_FILENO);
        int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) { ::dup2(fd, STDERR_FILENO); ::close(fd); }
    }
    ~StderrCapture3191() { restore(); }
    std::string text() {
        restore();
        std::ifstream in(path_);
        std::string s((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
        std::remove(path_.c_str());
        return s;
    }

private:
    void restore() {
        if (saved_ < 0) return;
        std::fflush(stderr);
        ::dup2(saved_, STDERR_FILENO);
        ::close(saved_);
        saved_ = -1;
    }
    std::string path_;
    int saved_ = -1;
};

bool contains3191(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

int compileImportAndRun3191(const std::string& libSrc,
                            const std::string& appSrc,
                            std::string* diag = nullptr) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3191_" + std::to_string(std::rand());
    TempFile3191 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3191 aObj((dir / (stem + "_a.o")).string());
    writeSource3191(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return kDepUnavailable3191;
    }
    if (!std::ifstream(aObj.path).good()) return kDepUnavailable3191;

    TempFile3191 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3191 bExe((dir / stem).string());
    writeSource3191(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bool ok;
    {
        StderrCapture3191 cap((dir / (stem + ".err")).string());
        // compileFileForTest, not driver.compileFile: the whole-suite ASan
        // sweep only reaches hand-rolled runners through this wrapper (#3156).
        ok = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
                 .success;
        std::string t = cap.text();
        if (diag) *diag = t;
    }
    if (!ok) return kRejected3191;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// The single-module twin: the SAME declarations in one compilation.
int compileAndRun3191(const std::string& src, std::string* diag = nullptr) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3191L_" + std::to_string(std::rand());
    TempFile3191 s((dir / (stem + ".bzl")).string());
    TempFile3191 exe((dir / stem).string());
    writeSource3191(s.path, src);
    bezel::Driver driver;
    bool ok;
    {
        StderrCapture3191 cap((dir / (stem + ".err")).string());
        ok = bezel::test::compileFileForTest(driver, s.path, exe.path).success;
        std::string t = cap.text();
        if (diag) *diag = t;
    }
    if (!ok) return kRejected3191;
    int status = std::system(exe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// §21.9's bare-discard hard error. Matched on the property-naming fragment
// (§3.6 rule 4: "A diagnostic produced by a property-dependent rule names the
// property in the words of the table above") plus the section tag, so a row
// cannot pass on some other error that merely happens to reject.
const char kDiscardDiag3191[] = "requires disposal";
const char kPropertyNamed3191[] = "non-trivial-dispose class";
const char kSection3191[] = "§21.9";

// The dependency: a class with a USER DESTRUCTOR whose side effect is
// observable, so the accept rows can prove the destructor really crosses and
// really runs. No `owned` marker anywhere — this defect needs none.
const char kLibDtor3191[] =
    "module Lib3191D; "
    "public class Res3191 "
    "  public static _disposed: int := 0; "
    "  public v: int := 0; "
    "  public constructor(x: int) "
    "    v := x; "
    "  end constructor; "
    "  destructor "
    "    Res3191._disposed := Res3191._disposed + 1; "
    "  end destructor; "
    "  public static method GetDisposed(): int => Res3191._disposed; "
    "end Res3191;";

// The dependency's trivial-dispose counterpart: character-identical but for the
// absent destructor. Used by the over-rejection controls.
const char kLibPlain3191[] =
    "module Lib3191P; "
    "public class Plain3191 "
    "  public v: int := 0; "
    "  public constructor(x: int) "
    "    v := x; "
    "  end constructor; "
    "  public method Get(): int => v; "
    "end Plain3191;";

}  // namespace

// ===========================================================================
// T1 — §21.9 / §3.6 / §28.9: a bare discard of a LOCAL class whose BASE is an
// imported class with a destructor. The issue's headline shape: no `owned`
// marker, no generics, and the producer is a `new` of a LOCAL type — so nothing
// but the imported base's carried verdict can make this non-trivial-dispose.
//
// Pre-fix this compiled SILENTLY while the twin (T1b) was rejected.
// ===========================================================================
BEZEL_TEST(xmod_destructor_carriage_3191,
           t1_bare_discard_of_local_derived_over_imported_dtor_base_rejects_21_9) {
    std::string diag;
    int r = compileImportAndRun3191(
        kLibDtor3191,
        "module App3191T1; import Lib3191D; "
        "public class D3191T1: Res3191 "
        "  public constructor(x: int) "
        "    base(x); "
        "  end constructor; "
        "end D3191T1; "
        "public class C3191T1 "
        "  public static method Main(): int "
        "    new D3191T1(1); "
        "    return Res3191.GetDisposed(); "
        "  end Main; "
        "end C3191T1;",
        &diag);
    if (r == kDepUnavailable3191) return;
    BEZEL_EXPECT_EQ(r, kRejected3191);
    BEZEL_EXPECT_TRUE(contains3191(diag, kDiscardDiag3191));
    BEZEL_EXPECT_TRUE(contains3191(diag, kPropertyNamed3191));
    BEZEL_EXPECT_TRUE(contains3191(diag, kSection3191));
}

// ===========================================================================
// T1b — the character-identical SINGLE-MODULE twin of T1 (§2.8.3 rule 4).
// This is the differential control that makes T1 a CROSS-MODULE claim rather
// than a §21.9 claim: it was already green pre-fix, and it is what the
// two-module build must now agree with.
// ===========================================================================
BEZEL_TEST(xmod_destructor_carriage_3191,
           t1b_same_module_twin_bare_discard_rejects_21_9) {
    std::string diag;
    int r = compileAndRun3191(
        "module Loc3191T1; "
        "public class ResL3191 "
        "  public static _disposed: int := 0; "
        "  public v: int := 0; "
        "  public constructor(x: int) "
        "    v := x; "
        "  end constructor; "
        "  destructor "
        "    ResL3191._disposed := ResL3191._disposed + 1; "
        "  end destructor; "
        "  public static method GetDisposed(): int => ResL3191._disposed; "
        "end ResL3191; "
        "public class DL3191T1: ResL3191 "
        "  public constructor(x: int) "
        "    base(x); "
        "  end constructor; "
        "end DL3191T1; "
        "public class CL3191T1 "
        "  public static method Main(): int "
        "    new DL3191T1(1); "
        "    return ResL3191.GetDisposed(); "
        "  end Main; "
        "end CL3191T1;",
        &diag);
    BEZEL_EXPECT_EQ(r, kRejected3191);
    BEZEL_EXPECT_TRUE(contains3191(diag, kDiscardDiag3191));
    BEZEL_EXPECT_TRUE(contains3191(diag, kSection3191));
}

// ===========================================================================
// T2 — the DIRECT shape: a bare discard of the imported class itself, with no
// local derivation in between. T1 exercises the verdict being found part-way up
// a base walk; this one exercises it being found at the walk's head, which is a
// different line of the fix.
// ===========================================================================
BEZEL_TEST(xmod_destructor_carriage_3191,
           t2_bare_discard_of_imported_dtor_class_directly_rejects_21_9) {
    std::string diag;
    int r = compileImportAndRun3191(
        kLibDtor3191,
        "module App3191T2; import Lib3191D; "
        "public class C3191T2 "
        "  public static method Main(): int "
        "    new Res3191(1); "
        "    return Res3191.GetDisposed(); "
        "  end Main; "
        "end C3191T2;",
        &diag);
    if (r == kDepUnavailable3191) return;
    BEZEL_EXPECT_EQ(r, kRejected3191);
    BEZEL_EXPECT_TRUE(contains3191(diag, kDiscardDiag3191));
    BEZEL_EXPECT_TRUE(contains3191(diag, kPropertyNamed3191));
}

// ===========================================================================
// T3 — OVER-REJECTION CONTROL: an imported class with NO destructor and no
// `owned` field must STAY trivial-dispose across the boundary, so a bare discard
// of it — and of a local class derived from it — is still ADMITTED.
//
// §21.9: "Trivial-dispose class types (§28.9): discard is permitted." This is
// the row that fails if the fix is closed the way the issue explicitly warns
// against (treating "imported" as "non-trivial"). It asserts a VALUE from a
// program that must still run, not merely that compilation succeeded.
// ===========================================================================
BEZEL_TEST(xmod_destructor_carriage_3191,
           t3_bare_discard_of_imported_trivial_dispose_class_still_admitted_21_9) {
    int r = compileImportAndRun3191(
        kLibPlain3191,
        "module App3191T3; import Lib3191P; "
        "public class D3191T3: Plain3191 "
        "  public constructor(x: int) "
        "    base(x); "
        "  end constructor; "
        "end D3191T3; "
        "public class C3191T3 "
        "  public static method Main(): int "
        "    new D3191T3(1); "
        "    new Plain3191(2); "
        "    return 7; "
        "  end Main; "
        "end C3191T3;");
    if (r == kDepUnavailable3191) return;
    BEZEL_EXPECT_EQ(r, 7);
}

// ===========================================================================
// T3b — the single-module twin of T3, which is what makes T3 non-vacuous: it
// establishes that this shape is admitted by §21.9 at all, so T3's green is
// "the boundary agrees with the twin" rather than "this program happens to
// compile".
// ===========================================================================
BEZEL_TEST(xmod_destructor_carriage_3191,
           t3b_same_module_twin_trivial_dispose_discard_admitted_21_9) {
    int r = compileAndRun3191(
        "module Loc3191T3; "
        "public class PlainL3191 "
        "  public v: int := 0; "
        "  public constructor(x: int) "
        "    v := x; "
        "  end constructor; "
        "  public method Get(): int => v; "
        "end PlainL3191; "
        "public class DL3191T3: PlainL3191 "
        "  public constructor(x: int) "
        "    base(x); "
        "  end constructor; "
        "end DL3191T3; "
        "public class CL3191T3 "
        "  public static method Main(): int "
        "    new DL3191T3(1); "
        "    new PlainL3191(2); "
        "    return 7; "
        "  end Main; "
        "end CL3191T3;");
    BEZEL_EXPECT_EQ(r, 7);
}

// ===========================================================================
// T4 — the REMEDY, and the proof that the fix rejects the DISCARD rather than
// the type. §21.9 names `.Dispose()` as one of the three remedies; applying it
// to the very shape T1 rejects must compile, run, and observably run the
// IMPORTED destructor.
//
// The returned value is the dependency's own disposal counter, so this row also
// pins the measurement that refutes the issue's stated mechanism: the imported
// destructor DOES execute (it did pre-fix too). Only the diagnostic was missing.
// ===========================================================================
BEZEL_TEST(xmod_destructor_carriage_3191,
           t4_explicit_dispose_remedy_runs_imported_destructor_21_9) {
    int r = compileImportAndRun3191(
        kLibDtor3191,
        "module App3191T4; import Lib3191D; "
        "public class D3191T4: Res3191 "
        "  public constructor(x: int) "
        "    base(x); "
        "  end constructor; "
        "end D3191T4; "
        "public class C3191T4 "
        "  public static method Main(): int "
        "    var r := new D3191T4(1); "
        "    r.Dispose(); "
        "    return Res3191.GetDisposed(); "
        "  end Main; "
        "end C3191T4;");
    if (r == kDepUnavailable3191) return;
    BEZEL_EXPECT_EQ(r, 1);
}

}  // namespace bezel::module
