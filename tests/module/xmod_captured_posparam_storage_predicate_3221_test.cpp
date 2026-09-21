// fix #3221 (§9.8 "Serialization synthesis" / §22.10 "primary-constructor
// parameters and capture" / §2.8.3 rule 4; v0.254, guard #2364) — the
// CAPTURED PRIMARY-CONSTRUCTOR PARAMETER half of the imported base-with-
// storage predicate. The direct follow-up to #3182's FIELD arm (#3182 fixed
// the plain body-field half of `isInstanceFieldStorageSymbol` and recorded
// this residual in its commit message); this fix closes it.
//
// WHAT THE DEFECT WAS. `Resolver::checkClassSerializationEligibilityLate`'s
// `classHasOwnStorage` (`src/sema/TypeResolution_Checks.cpp`) — the ONE walk
// in that function that leaves the module being compiled: it climbs
// `t->baseTypes`, so `cls` is an IMPORTED `TypeSymbol` whenever the base
// lives in a dependency — uses `sema::isInstanceFieldStorageSymbol` to ask
// "does this level carry its own instance storage?" The predicate's
// positional arm was
//
//     if (fs->posParam) return fs->posParamBodyRef;
//
// `FieldSymbol::posParam` is reconstructed on every import (#2516
// synthesizes the synthetic `ast::Param`), but `posParamBodyRef` was set
// ONLY by ExprTypecheck on the local side (#2325's capture enumeration: a
// §22.10 primary-ctor parameter referenced in a body position is captured
// into a synthetic private instance field) and NOTHING in `ModuleLoader`
// bridged it — so the importer always answered FALSE on the positional
// arm, and an imported base whose only storage was a captured primary-ctor
// parameter slipped past #2364's guard exactly the way a plain body field
// did before #3152/#3182. MEASURED on `226aca1` (HEAD before this fix),
// `bezelc -c lib.bzl -o lib.o` then `bezelc --import lib.o -o app app.bzl`,
// against a character-identical single-module twin:
//
//   base = `class Base3221T1(v: int); public property V: int => v; end Base3221T1;`
//   single-module twin -> REJECTED with #2364's "carries its own serialized storage"
//   IMPORTED            -> COMPILED CLEAN, exit 9 — silent accept of exactly
//                          the shape #2364 exists to refuse
//
// The storage really is there — #2325's capture enumeration records the
// parameter as a private instance field of the same name (the synthetic
// capture field), and `ModuleLoader` already preserves the §22.8 instance
// layout for the parameter via the existing `#2516 posParam` synthesis.
// So this was a classification error over a CHANNEL THAT DID NOT CROSS —
// which is why the fix is a one-bit wire carrier, exactly the precedent
// `#3164` set for the positional parameter's `owned` marker.
//
// THE FIX. `MetaMember::posParamBodyRef` (the 18th trailing scalar in
// MEMBER_BLOCK, additive within v7 — same shape as `isPositional` at
// #2516 / 17th scalar). The encoder (`ModuleMetadataBuilder.cpp`'s
// positional-field arm) writes `fs->posParamBodyRef`; the decoder
// (`ModuleLoader.cpp`'s FieldSymbol arm, `convertMember`) reads it back.
// The predicate's existing `if (fs->posParam) return fs->posParamBodyRef;`
// now answers correctly on both sides, and nothing about RECORDS changes:
// a §9.1 record's positional field carries no body references and
// `posParamBodyRef` is structurally false on both sides, so imported
// records keep their pre-fix classification.
//
// ⚠ THIS FIX MOVES IN THE REJECT DIRECTION, SO THE CONTROLS ARE HALF THE
// TEST. A set that only pinned what must be rejected would also pass on a
// compiler that rejected everything. T2 (an imported base whose primary-
// ctor parameter is NOT captured into a body field — used only in an
// initializer, never in a body, so `posParamBodyRef == false` on both
// sides — and the base carries no other storage) is the row that fails
// if the new predicate is over-broad, and it asserts a VALUE from a
// program that must still run. T2's twin pins the same shape on the local
// side. T1/T1b pin the REJECT on both sides.

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
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile3221 {
    std::string path;
    explicit TempFile3221(std::string p) : path(std::move(p)) {}
    ~TempFile3221() { std::remove(path.c_str()); }
};

void writeSource3221(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Return codes, chosen so an admitted program's own exit status can never be
// confused with a rejected compile (mirrors #3152/#3182's runner):
//   -2  the DEPENDENCY did not compile — toolchain unavailable, skip.
//   -3  the app was REJECTED by the front-end.
//   -1  the app compiled but did not exit normally.
//   >=0 the app's own exit status.
constexpr int kDepUnavailable3221 = -2;
constexpr int kRejected3221 = -3;

// A reject row that asserted only `kRejected3221` would stay green if one of
// these inline sources later acquired a plain syntax error — green for the
// wrong reason. Capture stderr so every reject pins the actual #2364 text.
class StderrCapture3221 {
public:
    explicit StderrCapture3221(std::string path) : path_(std::move(path)) {
        std::fflush(stderr);
        saved_ = ::dup(STDERR_FILENO);
        int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) { ::dup2(fd, STDERR_FILENO); ::close(fd); }
    }
    ~StderrCapture3221() { restore(); }
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

bool contains3221(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

int compileImportAndRun3221(const std::string& libSrc,
                            const std::string& appSrc,
                            std::string* diag = nullptr) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3221_");
    TempFile3221 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3221 aObj((dir / (stem + "_a.o")).string());
    writeSource3221(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return kDepUnavailable3221;
    }
    if (!std::ifstream(aObj.path).good()) return kDepUnavailable3221;

    TempFile3221 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3221 bExe((dir / stem).string());
    writeSource3221(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bool ok;
    {
        StderrCapture3221 cap((dir / (stem + ".err")).string());
        // compileFileForTest, not driver.compileFile: the whole-suite ASan
        // sweep only reaches hand-rolled runners through this wrapper (#3156).
        ok = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
                 .success;
        std::string t = cap.text();
        if (diag) *diag = t;
    }
    if (!ok) return kRejected3221;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// The single-module twin: the SAME declarations in one compilation.
int compileAndRun3221(const std::string& src, std::string* diag = nullptr) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3221L_");
    TempFile3221 s((dir / (stem + ".bzl")).string());
    TempFile3221 exe((dir / stem).string());
    writeSource3221(s.path, src);
    bezel::Driver driver;
    bool ok;
    {
        StderrCapture3221 cap((dir / (stem + ".err")).string());
        ok = bezel::test::compileFileForTest(driver, s.path, exe.path).success;
        std::string t = cap.text();
        if (diag) *diag = t;
    }
    if (!ok) return kRejected3221;
    int status = std::system(exe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

const char kStorageDiag3221[] = "which carries its own serialized storage";

}  // namespace

// ===========================================================================
// T1 — §9.8 / #2364 / §22.10: an IMPORTED base whose only storage is a
// CAPTURED primary-ctor parameter is seen by the base-with-storage guard.
//
// The base declares `v: int` (§22.10 positional parameter) and uses `v` in a
// property accessor body (`public property V: int => v;`) — that body
// reference is exactly #2325's capture enumeration: the typechecker sets
// `fs->posParamBodyRef = true` locally, and the parameter is captured into a
// synthetic private instance field of the same name. Before #3221, the
// importer never received `posParamBodyRef`, so the predicate's positional
// arm answered FALSE, and the imported build COMPILED CLEAN (exit 9) while
// the twin was REJECTED with #2364. After #3221, both reject.
//
// This is the direct follow-up to #3182's T1: same family, captured-
// positional-parameter variant.
// ===========================================================================
BEZEL_TEST(xmod_captured_posparam_storage_predicate_3221,
           t1_imported_captured_posparam_blocks_ideserializable_9_8) {
    std::string diag;
    int r = compileImportAndRun3221(
        "module Lib3221T1; "
        "public class Base3221T1(v: int) "
        "  public property V: int => v; "
        "end Base3221T1;",
        "module App3221T1; import Lib3221T1; "
        "public class D3221T1: Base3221T1, ISerializable, IDeserializable "
        "  public property N: int := 3; "
        "  public constructor(n: int) "
        "    N := n; "
        "  end constructor; "
        "end D3221T1; "
        "public class C3221T1 "
        "  public static method Main(): int "
        "    using var d := new D3221T1(9); "
        "    return d.N; "
        "  end Main; "
        "end C3221T1;",
        &diag);
    if (r == kDepUnavailable3221) return;
    BEZEL_EXPECT_EQ(r, kRejected3221);
    BEZEL_EXPECT_TRUE(contains3221(diag, kStorageDiag3221));
    BEZEL_EXPECT_TRUE(contains3221(diag, "2364"));
}

// ===========================================================================
// T1b — the SAME-MODULE twin of T1 (§2.8.3 rule 4). The differential control
// that makes T1 a cross-module claim rather than a #2364 claim.
// ===========================================================================
BEZEL_TEST(xmod_captured_posparam_storage_predicate_3221,
           t1b_same_module_captured_posparam_blocks_ideserializable_9_8) {
    std::string diag;
    int r = compileAndRun3221(
        "module Loc3221T1; "
        "public class BaseL3221T1(v: int) "
        "  public property V: int => v; "
        "end BaseL3221T1; "
        "public class DL3221T1: BaseL3221T1, ISerializable, IDeserializable "
        "  public property N: int := 3; "
        "  public constructor(n: int) "
        "    N := n; "
        "  end constructor; "
        "end DL3221T1; "
        "public class CL3221T1 "
        "  public static method Main(): int "
        "    using var d := new DL3221T1(9); "
        "    return d.N; "
        "  end Main; "
        "end CL3221T1;",
        &diag);
    BEZEL_EXPECT_EQ(r, kRejected3221);
    BEZEL_EXPECT_TRUE(contains3221(diag, kStorageDiag3221));
}

// ===========================================================================
// T2 — OVER-REJECTION CONTROL: an IMPORTED base whose primary-ctor parameter
// is NOT captured into a body field stays ADMITTED.
//
// The base declares `v: int` (positional parameter) and never references it
// in any body — only `Get` returns a literal. `posParamBodyRef` stays false
// on both sides. The base has no body field, no auto-property, and no other
// storage; the §9.8 tier-4 `polymorphic` pattern's abstract-base shape is
// the analogue #3182 T3 pins.
//
// This is the row that fails if the new predicate is over-broad — e.g. if
// the fix had widened the positional arm to "posParam ⟹ storage" (the
// obvious-but-wrong widening — it would flip the answer for imported
// RECORDS too, which is a different question with different consumers).
// The fix uses a one-bit wire carrier, so an uncaptured parameter crosses
// as `posParamBodyRef = false` and the arm returns false, and the base is
// storage-free exactly as the local twin shows.
// ===========================================================================
BEZEL_TEST(xmod_captured_posparam_storage_predicate_3221,
           t2_imported_uncaptured_posparam_still_admitted_9_8) {
    std::string diag;
    int r = compileImportAndRun3221(
        "module Lib3221T2; "
        "public class Base3221T2(v: int) "
        "  public method Get(): int => 7; "
        "end Base3221T2;",
        "module App3221T2; import Lib3221T2; "
        "public class D3221T2: Base3221T2, ISerializable, IDeserializable "
        "  public property N: int := 3; "
        // fix #4549 (§12.8, v0.306): the derived constructor now
        // WRITES its `base(...)` chain. §12.8 inserts an implicit
        // `base()` when none is written and "requires the base
        // class to have a parameterless constructor"; this base
        // declares the required positional parameter `v: int`, so
        // the program was ill-formed and is rejected as of #4549.
        // The ORACLE IS UNCHANGED (still `== 11`) and so is this
        // row's subject: `v` is passed but never referenced in any
        // body, so `posParamBodyRef` stays false and the base is
        // storage-free exactly as before.
        "  public constructor(n: int) "
        "    base(0); "
        "    N := n; "
        "  end constructor; "
        "end D3221T2; "
        "public class C3221T2 "
        "  public static method Main(): int "
        "    using var d := new D3221T2(4); "
        "    return d.N + d.Get(); "
        "  end Main; "
        "end C3221T2;",
        &diag);
    if (r == kDepUnavailable3221) return;
    BEZEL_EXPECT_EQ(r, 11);  // 4 + 7 — the base's method really runs
    BEZEL_EXPECT_TRUE(!contains3221(diag, kStorageDiag3221));
}

// ===========================================================================
// T2b — the SAME-MODULE twin of T2. Both must agree on the admission just
// as T1/T1b agree on the rejection.
// ===========================================================================
BEZEL_TEST(xmod_captured_posparam_storage_predicate_3221,
           t2b_same_module_uncaptured_posparam_still_admitted_9_8) {
    std::string diag;
    int r = compileAndRun3221(
        "module Loc3221T2; "
        "public class BaseL3221T2(v: int) "
        "  public method Get(): int => 7; "
        "end BaseL3221T2; "
        "public class DL3221T2: BaseL3221T2, ISerializable, IDeserializable "
        "  public property N: int := 3; "
        // fix #4549 (§12.8, v0.306): the derived constructor now
        // WRITES its `base(...)` chain. §12.8 inserts an implicit
        // `base()` when none is written and "requires the base
        // class to have a parameterless constructor"; this base
        // declares the required positional parameter `v: int`, so
        // the program was ill-formed and is rejected as of #4549.
        // The ORACLE IS UNCHANGED (still `== 11`) and so is this
        // row's subject: `v` is passed but never referenced in any
        // body, so `posParamBodyRef` stays false and the base is
        // storage-free exactly as before.
        "  public constructor(n: int) "
        "    base(0); "
        "    N := n; "
        "  end constructor; "
        "end DL3221T2; "
        "public class CL3221T2 "
        "  public static method Main(): int "
        "    using var d := new DL3221T2(4); "
        "    return d.N + d.Get(); "
        "  end Main; "
        "end CL3221T2;",
        &diag);
    BEZEL_EXPECT_EQ(r, 11);
    BEZEL_EXPECT_TRUE(!contains3221(diag, kStorageDiag3221));
}

}  // namespace bezel::module
