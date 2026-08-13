// fix #3182 (§9.8 "Serialization synthesis" / §22.8 "Field initializers" /
// §2.8.2 clause 1; v0.254, guard #2364) — the FIELD half of the imported
// base-with-storage predicate. Umbrella #3149/#3152; the twin of #3152's T10,
// which fixed the auto-property half of the same walk.
//
// WHAT THE DEFECT WAS. `Resolver::checkClassSerializationEligibilityLate`'s
// `classHasOwnStorage` is the ONE walk in that function that leaves the module
// being compiled: it climbs `t->baseTypes`, so `cls` is an IMPORTED TypeSymbol
// whenever the base lives in a dependency. Its FieldSymbol arm asked
//
//     auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
//
// and, on a null cast, fell through to "this level carries no storage" unless
// the field happened to be a captured primary-constructor parameter. That cast
// is null for EVERY imported member, so an imported base's plain body field was
// invisible and #2364's guard did not fire. That guard refuses `IDeserializable`
// over a base with storage precisely because the synthesized `Deserialize` does
// not construct the base subobject — the inherited fields would silently keep
// their §7.9 defaults instead of the deserialized data.
//
// The storage really is there: #2456 carries a PRIVATE instance field across
// the boundary for layout (a class's physical layout is declaration order, not
// exported surface). So this was a classification error over a channel that
// already existed, not a missing channel.
//
// MEASURED PRE-FIX on `ec6a02f`, `bezelc -c lib.bzl -o lib.o` then
// `bezelc --import lib.o -o app app.bzl`, against a character-identical
// single-module twin:
//
//   base storage = `public property W: int := 41;` -> both reject  (#3152 fixed)
//   base storage = `private _v: int := 41;`        -> IMPORTED **ran, exit 9**
//                                                     twin REJECTED, #2364
//
// ⚠ THIS FIX MOVES IN THE REJECT DIRECTION, SO THE CONTROLS ARE HALF THE TEST.
// A set that only pinned what must be rejected would also pass on a compiler
// that rejected everything. T2 (an imported base whose only field is
// `static readonly` — process-global, no instance footprint, §22.12) and T3 (an
// imported base with NO storage at all — the §9.8 tier-4 `polymorphic` abstract
// base shape the guard was deliberately written not to catch) are the rows that
// fail if the new predicate is over-broad, and both assert a VALUE from a
// program that must still run.
//
// ⚠ T2's base field is `public`, not `private`, ON PURPOSE. A private STATIC
// field does not cross the boundary at all (#2456's layout carriage is scoped
// to non-static members), so a private-static control would be vacuous — it
// would pass because the member is absent, not because the `static` test works.
// `public static readonly` really is on the wire, carrying "static" in
// `MetaMember::modifiers`, which is what `isInstanceFieldStorageSymbol` reads.

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

struct TempFile3182 {
    std::string path;
    explicit TempFile3182(std::string p) : path(std::move(p)) {}
    ~TempFile3182() { std::remove(path.c_str()); }
};

void writeSource3182(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Return codes, chosen so an admitted program's own exit status can never be
// confused with a rejected compile (mirrors #3152's runner):
//   -2  the DEPENDENCY did not compile — toolchain unavailable, skip.
//   -3  the app was REJECTED by the front-end.
//   -1  the app compiled but did not exit normally.
//   >=0 the app's own exit status.
constexpr int kDepUnavailable3182 = -2;
constexpr int kRejected3182 = -3;

// A reject row that asserted only `kRejected3182` would stay green if one of
// these inline sources later acquired a plain syntax error — green for the
// wrong reason. Capture stderr so every reject pins the actual #2364 text.
class StderrCapture3182 {
public:
    explicit StderrCapture3182(std::string path) : path_(std::move(path)) {
        std::fflush(stderr);
        saved_ = ::dup(STDERR_FILENO);
        int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) { ::dup2(fd, STDERR_FILENO); ::close(fd); }
    }
    ~StderrCapture3182() { restore(); }
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

bool contains3182(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

int compileImportAndRun3182(const std::string& libSrc,
                            const std::string& appSrc,
                            std::string* diag = nullptr) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3182_" + std::to_string(std::rand());
    TempFile3182 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3182 aObj((dir / (stem + "_a.o")).string());
    writeSource3182(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return kDepUnavailable3182;
    }
    if (!std::ifstream(aObj.path).good()) return kDepUnavailable3182;

    TempFile3182 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3182 bExe((dir / stem).string());
    writeSource3182(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bool ok;
    {
        StderrCapture3182 cap((dir / (stem + ".err")).string());
        // compileFileForTest, not driver.compileFile: the whole-suite ASan
        // sweep only reaches hand-rolled runners through this wrapper (#3156).
        ok = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
                 .success;
        std::string t = cap.text();
        if (diag) *diag = t;
    }
    if (!ok) return kRejected3182;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// The single-module twin: the SAME declarations in one compilation.
int compileAndRun3182(const std::string& src, std::string* diag = nullptr) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3182L_" + std::to_string(std::rand());
    TempFile3182 s((dir / (stem + ".bzl")).string());
    TempFile3182 exe((dir / stem).string());
    writeSource3182(s.path, src);
    bezel::Driver driver;
    bool ok;
    {
        StderrCapture3182 cap((dir / (stem + ".err")).string());
        ok = bezel::test::compileFileForTest(driver, s.path, exe.path).success;
        std::string t = cap.text();
        if (diag) *diag = t;
    }
    if (!ok) return kRejected3182;
    int status = std::system(exe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

const char kStorageDiag3182[] = "which carries its own serialized storage";

}  // namespace

// ===========================================================================
// T1 — §9.8 / #2364: an IMPORTED base carrying a plain PRIVATE BODY FIELD is
// seen by the base-with-storage guard.
//
// Pre-fix the imported build compiled and RAN (exit 9) while the twin was
// rejected. Both must now reject, with the same diagnostic.
// ===========================================================================
BEZEL_TEST(xmod_base_field_storage_predicate_3182,
           t1_imported_base_body_field_blocks_ideserializable_9_8) {
    std::string diag;
    int r = compileImportAndRun3182(
        "module Lib3182T1; "
        "public class Base3182T1 "
        "  private _v: int := 41; "
        "  public property V: int => _v; "
        "end Base3182T1;",
        "module App3182T1; import Lib3182T1; "
        "public class D3182T1: Base3182T1, ISerializable, IDeserializable "
        "  public property N: int := 3; "
        "  public constructor(n: int) "
        "    N := n; "
        "  end constructor; "
        "end D3182T1; "
        "public class C3182T1 "
        "  public static method Main(): int "
        "    using var d := new D3182T1(9); "
        "    return d.N; "
        "  end Main; "
        "end C3182T1;",
        &diag);
    if (r == kDepUnavailable3182) return;
    BEZEL_EXPECT_EQ(r, kRejected3182);
    BEZEL_EXPECT_TRUE(contains3182(diag, kStorageDiag3182));
    BEZEL_EXPECT_TRUE(contains3182(diag, "2364"));
}

// ===========================================================================
// T1b — the SAME-MODULE twin of T1 (§2.8.3 rule 4). This is the differential
// control that makes T1 a cross-module claim rather than a #2364 claim.
// ===========================================================================
BEZEL_TEST(xmod_base_field_storage_predicate_3182,
           t1b_same_module_base_body_field_blocks_ideserializable_9_8) {
    std::string diag;
    int r = compileAndRun3182(
        "module Loc3182T1; "
        "public class BaseL3182T1 "
        "  private _v: int := 41; "
        "  public property V: int => _v; "
        "end BaseL3182T1; "
        "public class DL3182T1: BaseL3182T1, ISerializable, IDeserializable "
        "  public property N: int := 3; "
        "  public constructor(n: int) "
        "    N := n; "
        "  end constructor; "
        "end DL3182T1; "
        "public class CL3182T1 "
        "  public static method Main(): int "
        "    using var d := new DL3182T1(9); "
        "    return d.N; "
        "  end Main; "
        "end CL3182T1;",
        &diag);
    BEZEL_EXPECT_EQ(r, kRejected3182);
    BEZEL_EXPECT_TRUE(contains3182(diag, kStorageDiag3182));
}

// ===========================================================================
// T2 — OVER-REJECTION CONTROL: an IMPORTED base whose only field is
// `static readonly` must STAY ADMITTED.
//
// §22.12 makes a static field process-global storage with no instance-layout
// footprint, so it is not "storage of its own" for #2364's purposes — the
// synthesized `Deserialize` has nothing on the base subobject to lose. The
// pre-fix code read `static` off `ast::FieldDecl::modifiers`; the fix reads it
// off `FieldSymbol::modifiers`, which is the same vector locally and is filled
// from `MetaMember::modifiers` on import. This row is what proves the modifier
// actually survives the boundary — without it, the fix would be indistinguishable
// from "count every imported field".
//
// The row asserts the program's VALUE (9), not merely that it compiled.
// ===========================================================================
BEZEL_TEST(xmod_base_field_storage_predicate_3182,
           t2_imported_base_static_field_still_admitted_9_8) {
    std::string diag;
    int r = compileImportAndRun3182(
        "module Lib3182T2; "
        "public class Base3182T2 "
        "  public static readonly _sv: int := 41; "
        "  public static property SV: int => Base3182T2._sv; "
        "end Base3182T2;",
        "module App3182T2; import Lib3182T2; "
        "public class D3182T2: Base3182T2, ISerializable, IDeserializable "
        "  public property N: int := 3; "
        "  public constructor(n: int) "
        "    N := n; "
        "  end constructor; "
        "end D3182T2; "
        "public class C3182T2 "
        "  public static method Main(): int "
        "    using var d := new D3182T2(9); "
        "    return d.N; "
        "  end Main; "
        "end C3182T2;",
        &diag);
    if (r == kDepUnavailable3182) return;
    BEZEL_EXPECT_EQ(r, 9);
    BEZEL_EXPECT_TRUE(!contains3182(diag, kStorageDiag3182));
}

// ===========================================================================
// T2b — the SAME-MODULE twin of T2. The two must agree on the admission just
// as T1/T1b agree on the rejection.
// ===========================================================================
BEZEL_TEST(xmod_base_field_storage_predicate_3182,
           t2b_same_module_base_static_field_still_admitted_9_8) {
    std::string diag;
    int r = compileAndRun3182(
        "module Loc3182T2; "
        "public class BaseL3182T2 "
        "  public static readonly _sv: int := 41; "
        "  public static property SV: int => BaseL3182T2._sv; "
        "end BaseL3182T2; "
        "public class DL3182T2: BaseL3182T2, ISerializable, IDeserializable "
        "  public property N: int := 3; "
        "  public constructor(n: int) "
        "    N := n; "
        "  end constructor; "
        "end DL3182T2; "
        "public class CL3182T2 "
        "  public static method Main(): int "
        "    using var d := new DL3182T2(9); "
        "    return d.N; "
        "  end Main; "
        "end CL3182T2;",
        &diag);
    BEZEL_EXPECT_EQ(r, 9);
    BEZEL_EXPECT_TRUE(!contains3182(diag, kStorageDiag3182));
}

// ===========================================================================
// T3 — OVER-REJECTION CONTROL: an IMPORTED base with NO storage at all stays
// admitted.
//
// This is the shape #2364's guard was explicitly written not to catch — the
// §9.8 tier-4 `polymorphic` pattern's abstract base, which carries only methods
// and a registry clause. A base with zero own components has nothing for
// `Deserialize` to lose. If the new predicate answered "storage" for any
// FieldSymbol-shaped member, or the walk started counting methods, this row
// would flip to a reject.
// ===========================================================================
BEZEL_TEST(xmod_base_field_storage_predicate_3182,
           t3_imported_storage_free_base_still_admitted_9_8) {
    std::string diag;
    int r = compileImportAndRun3182(
        "module Lib3182T3; "
        "public class Base3182T3 "
        "  public method Ping(): int => 7; "
        "end Base3182T3;",
        "module App3182T3; import Lib3182T3; "
        "public class D3182T3: Base3182T3, ISerializable, IDeserializable "
        "  public property N: int := 3; "
        "  public constructor(n: int) "
        "    N := n; "
        "  end constructor; "
        "end D3182T3; "
        "public class C3182T3 "
        "  public static method Main(): int "
        "    using var d := new D3182T3(4); "
        "    return d.N + d.Ping(); "
        "  end Main; "
        "end C3182T3;",
        &diag);
    if (r == kDepUnavailable3182) return;
    BEZEL_EXPECT_EQ(r, 11);  // 4 + 7 — the base's method really runs
    BEZEL_EXPECT_TRUE(!contains3182(diag, kStorageDiag3182));
}

// ===========================================================================
// T4 — the DEEP-CHAIN row: the storage may sit at any level of the imported
// base chain, because the guard walks the whole chain. The middle class here
// carries nothing; the grandparent carries the body field.
//
// Without this row a fix that only looked at the DIRECT base would pass T1.
// ===========================================================================
BEZEL_TEST(xmod_base_field_storage_predicate_3182,
           t4_imported_grandparent_body_field_blocks_ideserializable_9_8) {
    std::string diag;
    int r = compileImportAndRun3182(
        "module Lib3182T4; "
        "public class Root3182T4 "
        "  private _v: int := 41; "
        "  public property V: int => _v; "
        "end Root3182T4; "
        "public class Mid3182T4: Root3182T4 "
        "  public method Ping(): int => 7; "
        "end Mid3182T4;",
        "module App3182T4; import Lib3182T4; "
        "public class D3182T4: Mid3182T4, ISerializable, IDeserializable "
        "  public property N: int := 3; "
        "  public constructor(n: int) "
        "    N := n; "
        "  end constructor; "
        "end D3182T4; "
        "public class C3182T4 "
        "  public static method Main(): int "
        "    using var d := new D3182T4(9); "
        "    return d.N; "
        "  end Main; "
        "end C3182T4;",
        &diag);
    if (r == kDepUnavailable3182) return;
    BEZEL_EXPECT_EQ(r, kRejected3182);
    BEZEL_EXPECT_TRUE(contains3182(diag, kStorageDiag3182));
}

// ===========================================================================
// T5 — §9.8 control: an imported base with a body field but NO
// `IDeserializable` on the derived class is untouched. The guard is keyed on
// `wantsID`, and this row pins that the fix did not turn "the base has storage"
// into a rejection on its own.
// ===========================================================================
BEZEL_TEST(xmod_base_field_storage_predicate_3182,
           t5_imported_base_body_field_without_ideserializable_admitted_9_8) {
    std::string diag;
    int r = compileImportAndRun3182(
        "module Lib3182T5; "
        "public class Base3182T5 "
        "  private _v: int := 41; "
        "  public property V: int => _v; "
        "end Base3182T5;",
        "module App3182T5; import Lib3182T5; "
        "public class D3182T5: Base3182T5, ISerializable "
        "  public property N: int := 3; "
        "  public constructor(n: int) "
        "    N := n; "
        "  end constructor; "
        "end D3182T5; "
        "public class C3182T5 "
        "  public static method Main(): int "
        "    using var d := new D3182T5(9); "
        "    return d.N + d.V; "
        "  end Main; "
        "end C3182T5;",
        &diag);
    if (r == kDepUnavailable3182) return;
    BEZEL_EXPECT_EQ(r, 50);  // 9 + 41 — the imported base's field really reads
    BEZEL_EXPECT_TRUE(!contains3182(diag, kStorageDiag3182));
}

}  // namespace bezel::module
