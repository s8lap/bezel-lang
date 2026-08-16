// fix #3152 family (§9.2 / §9.5 / §9.8 / §22.7 / §22.8 / §2.8.2 clause 1,
// spec v0.252): ONE question — "is this member auto-property storage?" — had
// FOUR bodies answering it, and the two spellings disagreed across a module
// boundary.
//
// `sema::isAutoPropertySymbol` and `Emitter::isAutoProperty` both required
// `dynamic_cast<const ast::PropertyDecl*>(ps->decl)` to succeed. `ModuleLoader`
// never assigns `Symbol::decl` for an imported member (there is no AST for a
// dependency's types), so both answered `false` for EVERY imported
// auto-property — a silent wrong answer, not a null-deref. The wire bit that
// settles it has existed since #2478 (`MetaMember::isAutoProperty` ->
// `ctx.importedPropertyIsAutoStorage`), and #2478 deliberately widened only the
// three storage-LAYOUT call sites, listing "roughly thirty OTHER call sites"
// as follow-up. The both-sources twin `sema::isAutoPropertyStorageSymbol`
// landed with the Sendability collapse and reached exactly ONE of sixteen
// callers. Per the member-symbol audit (docs/proposals, 2026-08-13 §5) and
// CLAUDE.md's #1462 warning, that partial state is the dangerous one: two
// predicates, one question, and which one a caller reached decided the answer.
//
// THE FIX: one predicate. `sema::isAutoPropertyStorageSymbol(m, ctx)` is the
// storage question and now has ONE body, called from both sema and codegen —
// `Emitter::isAutoProperty` / `Emitter::isAutoPropertyStorage` are deleted, not
// re-pointed. The AST-only half survives, renamed `sema::hasAstAutoPropertyDecl`
// so a caller cannot reach for it meaning the other thing; its remaining callers
// are the ones that go on to dereference the `ast::PropertyDecl`, the
// declaration-site walks over `ctx.module()->children` (which require `t->decl`,
// so no imported symbol reaches them), and the EXPORTER, where the declaration
// is the authority that computes the wire bit.
//
// FIVE DEFECTS MEASURED on real two-module builds before the fix, each against
// a character-identical single-module twin:
//
//   §9.5   `r with(A := 40)` on an imported record's auto-property was rejected
//          as "`A` is a computed property" (#878). Twin: 42.        -> T1
//   §22.7  `var t := base.W;` on an imported base's auto-property, in a derived
//          constructor before a local non-nullable field is assigned, was
//          rejected as "cannot observe `this` (property `W`)". Twin: 3.  -> T3
//   §22.7  the write form `this.W := 5;`, same shape, same reject. Twin: 7. -> T4
//   §22.8  an imported GENERIC class's auto-property INITIALIZER was never
//          replayed: `property W: int := 42;` beside `private _f: int := 7;`
//          returned 7 where the twin returned 49 — silent wrong DATA, the field
//          replayed and the auto-property read its §7.9 default.        -> T6
//   §9.8   a local `class D: ImportedBase, ISerializable, IDeserializable`
//          whose base carries a single auto-property COMPILED AND RAN, while
//          the twin was correctly rejected by the #2364 guard — a silent accept
//          of exactly the shape that guard exists to refuse.            -> T10
//
// WHAT EACH TEST IS FOR (proven mechanically, not asserted):
//   T1, T3, T4, T6, T8, T10   fail without the fix, pass with it.
//   T2, T5, T7, T9            OVER-REJECTION controls — pass before AND after.
//                             Each pins that the both-sources predicate still
//                             answers `false` where it must: an imported
//                             COMPUTED property is not a `with` target (T2) and
//                             still gates the §22.7 observation (T5); an
//                             imported NON-generic auto-property initializer is
//                             still replayed exactly once by the exporter's own
//                             `bz$<Type>$init` (T7); an imported value-typed
//                             body record keeps its §9.2 auto-equality (T9).
//                             Without these, widening a predicate to admit
//                             imports would read as a pass while having
//                             replaced a wrong `false` with a wrong `true`.
//
// EVERY test runs its single-module twin too. The claim is not "this program
// compiles" but "the importer reaches the SAME verdict as the defining
// compilation" (§2.8.2 clause 1) — a twin that also failed would mean the rule
// changed, not that the seam closed.
//
// REFUTED while verifying, and recorded so it is not re-derived: the audit's
// §6 row 3 predicts a LEAK here — "collectInstanceStorage gives an imported
// auto-property a slot, emitDisposerCore's release walks skip it; the slot is
// stored into with a +1 and is never released". The slot and the skip are real
// (instrumented: `isAutoPropertyStorage=1`, `isAutoProperty=0`, `hasIdx=1` for
// an imported generic's `string` auto-property). The `+1` is not: the release
// walks are gated a SECOND time by `isStringStorageMember`, which is `false`
// for the same member because `recordMemberType`'s property arm has no
// decl-null fallback (audit §6 row 1) — instrumented `isStringStorage=0`. 200
// construct/dispose iterations of an imported `class Box<T>` with a heap
// `string` auto-property under `ASAN_OPTIONS=detect_leaks=1` reported nothing,
// before or after. So there is no leak to pin and no test here for one. The
// disposer walks are converted anyway, because the imbalance runs the OTHER
// way: the retain half (`isStringFieldLvalue`, #2504) is already both-sources
// and does NOT depend on `recordMemberType`, so an unconverted walk becomes a
// real leak the day row 1 lands, whereas a converted one stays balanced.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
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

struct TempFile3152P {
    std::string path;
    explicit TempFile3152P(std::string p) : path(std::move(p)) {}
    ~TempFile3152P() { std::remove(path.c_str()); }
};

void writeSource3152P(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Return codes, chosen so an admitted program's own exit status can never be
// confused with a rejected compile:
//   -2  the DEPENDENCY did not compile — toolchain unavailable, skip.
//   -3  the app was REJECTED by the front-end.
//   -1  the app compiled but did not exit normally.
//   >=0 the app's own exit status.
constexpr int kDepUnavailable3152P = -2;
constexpr int kRejected3152P = -3;

// The front-end prints diagnostics to stderr and `CompileResult` carries only a
// bool + a count, so a REJECT test asserting nothing but `kRejected3152P` would
// stay green if one of these inline sources later acquired a plain syntax
// error — green for the wrong reason. This captures stderr around the compile
// so every reject below pins the actual diagnostic. The fd is restored in the
// destructor on every path, so a failing assertion still reports through the
// real stderr.
class StderrCapture3152P {
public:
    explicit StderrCapture3152P(std::string path) : path_(std::move(path)) {
        std::fflush(stderr);
        saved_ = ::dup(STDERR_FILENO);
        int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) { ::dup2(fd, STDERR_FILENO); ::close(fd); }
    }
    ~StderrCapture3152P() { restore(); }
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

bool contains3152P(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

int compileImportAndRun3152P(const std::string& libSrc,
                             const std::string& appSrc,
                             std::string* diag = nullptr) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3152p_");
    TempFile3152P aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3152P aObj((dir / (stem + "_a.o")).string());
    writeSource3152P(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return kDepUnavailable3152P;
    }
    if (!std::ifstream(aObj.path).good()) return kDepUnavailable3152P;

    TempFile3152P bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3152P bExe((dir / stem).string());
    writeSource3152P(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bool ok;
    {
        StderrCapture3152P cap((dir / (stem + ".err")).string());
        // compileFileForTest, not driver.compileFile: the whole-suite ASan
        // sweep (`gate.sh --full`) only reaches hand-rolled runners through
        // this wrapper (tests/support/driver_run_support.hpp).
        ok = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
                 .success;
        std::string t = cap.text();
        if (diag) *diag = t;
    }
    if (!ok) return kRejected3152P;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// The single-module twin: the SAME declarations in one compilation.
int compileAndRun3152P(const std::string& src, std::string* diag = nullptr) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3152pL_");
    TempFile3152P s((dir / (stem + ".bzl")).string());
    TempFile3152P exe((dir / stem).string());
    writeSource3152P(s.path, src);
    bezel::Driver driver;
    bool ok;
    {
        StderrCapture3152P cap((dir / (stem + ".err")).string());
        ok = bezel::test::compileFileForTest(driver, s.path, exe.path).success;
        std::string t = cap.text();
        if (diag) *diag = t;
    }
    if (!ok) return kRejected3152P;
    int status = std::system(exe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// T1 — §9.5 `with` on an IMPORTED record's auto-property.
//
// §9.5: "For body-only records, `with` targets any auto-property that has
// backing storage; expression-bodied (computed) properties are not valid
// targets." The AST-only predicate made every imported auto-property look
// computed, so ExprTypecheck_Exprs reported #878 and the program did not build.
// MEASURED before the fix: twin 42, imported rejected.
// ===========================================================================
BEZEL_TEST(xmod_auto_property_storage_predicate_3152,
           with_targets_imported_record_auto_property_9_5) {
    int r = compileImportAndRun3152P(
        "module Lib3152PT1; "
        "public record Rec3152PT1 "
        "  public property A: int := 1; "
        "  public property B: int := 2; "
        "end Rec3152PT1;",
        "module App3152PT1; import Lib3152PT1; "
        "public class C3152PT1 "
        "  public static method Main(): int "
        "    var r := new Rec3152PT1(); "
        "    var s := r with(A := 40); "
        "    return s.A + s.B; "
        "  end Main; "
        "end C3152PT1;");
    if (r == kDepUnavailable3152P) return;  // toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 42);  // 40 (replaced) + 2 (carried)

    int twin = compileAndRun3152P(
        "module Loc3152PT1; "
        "public record RecL3152PT1 "
        "  public property A: int := 1; "
        "  public property B: int := 2; "
        "end RecL3152PT1; "
        "public class CL3152PT1 "
        "  public static method Main(): int "
        "    var r := new RecL3152PT1(); "
        "    var s := r with(A := 40); "
        "    return s.A + s.B; "
        "  end Main; "
        "end CL3152PT1;");
    BEZEL_EXPECT_EQ(twin, 42);
}

// ===========================================================================
// T2 — OVER-REJECTION CONTROL for T1. An imported COMPUTED property is still
// not a `with` target. Passes before AND after: the point is that the fix
// admitted storage without admitting everything — the #2478 wire bit is `false`
// for a computed property, so the both-sources predicate still says `false`.
// The diagnostic TEXT is pinned so a syntax slip in the inline source cannot
// keep this green for the wrong reason.
// ===========================================================================
BEZEL_TEST(xmod_auto_property_storage_predicate_3152,
           with_still_rejects_imported_computed_property_9_5_878) {
    std::string diag;
    int r = compileImportAndRun3152P(
        "module Lib3152PT2; "
        "public record Rec3152PT2 "
        "  public property A: int := 1; "
        "  public property C: int => 5; "
        "end Rec3152PT2;",
        "module App3152PT2; import Lib3152PT2; "
        "public class C3152PT2 "
        "  public static method Main(): int "
        "    var r := new Rec3152PT2(); "
        "    var s := r with(C := 40); "
        "    return s.A; "
        "  end Main; "
        "end C3152PT2;",
        &diag);
    if (r == kDepUnavailable3152P) return;
    BEZEL_EXPECT_EQ(r, kRejected3152P);
    BEZEL_EXPECT_TRUE(contains3152P(
        diag, "is a computed property and cannot be a `with` target"));

    std::string twinDiag;
    int twin = compileAndRun3152P(
        "module Loc3152PT2; "
        "public record RecL3152PT2 "
        "  public property A: int := 1; "
        "  public property C: int => 5; "
        "end RecL3152PT2; "
        "public class CL3152PT2 "
        "  public static method Main(): int "
        "    var r := new RecL3152PT2(); "
        "    var s := r with(C := 40); "
        "    return s.A; "
        "  end Main; "
        "end CL3152PT2;",
        &twinDiag);
    BEZEL_EXPECT_EQ(twin, kRejected3152P);
    BEZEL_EXPECT_TRUE(contains3152P(
        twinDiag, "is a computed property and cannot be a `with` target"));
}

// ===========================================================================
// T3 — §22.7 rule 1, READ form. A field or auto-property read in a constructor
// body is backing storage, NOT an observation; a computed-property read is an
// accessor invocation and IS one. `base.W` on an IMPORTED base's auto-property
// was classified as the latter, so the §22.7 gate fired before the derived
// class's own non-nullable `_s` was assigned.
// MEASURED before the fix: twin 3, imported rejected.
// ===========================================================================
BEZEL_TEST(xmod_auto_property_storage_predicate_3152,
           ctor_read_of_imported_base_auto_property_is_not_an_observation_22_7) {
    int r = compileImportAndRun3152P(
        "module Lib3152PT3; "
        "public class Base3152PT3 "
        "  public property W: int := 41; "
        "end Base3152PT3;",
        "module App3152PT3; import Lib3152PT3; "
        "public class D3152PT3: Base3152PT3 "
        "  private _s: string; "
        "  public constructor() "
        "    var t := base.W; "
        "    _s := \"v\" + t.ToString(); "
        "  end constructor; "
        "  public property S: string => _s; "
        "end D3152PT3; "
        "public class C3152PT3 "
        "  public static method Main(): int "
        "    using var d := new D3152PT3(); "
        "    return d.S.Length; "
        "  end Main; "
        "end C3152PT3;");
    if (r == kDepUnavailable3152P) return;
    BEZEL_EXPECT_EQ(r, 3);  // "v41".Length

    int twin = compileAndRun3152P(
        "module Loc3152PT3; "
        "public class BaseL3152PT3 "
        "  public property W: int := 41; "
        "end BaseL3152PT3; "
        "public class DL3152PT3: BaseL3152PT3 "
        "  private _s: string; "
        "  public constructor() "
        "    var t := base.W; "
        "    _s := \"v\" + t.ToString(); "
        "  end constructor; "
        "  public property S: string => _s; "
        "end DL3152PT3; "
        "public class CL3152PT3 "
        "  public static method Main(): int "
        "    using var d := new DL3152PT3(); "
        "    return d.S.Length; "
        "  end Main; "
        "end CL3152PT3;");
    BEZEL_EXPECT_EQ(twin, 3);
}

// ===========================================================================
// T4 — §22.7 rule 1, WRITE form. A `:=` store to an auto-property of `this` is
// a direct field store that counts toward the gate and is not itself an
// observation. `this.W := 5;` on an IMPORTED base's auto-property fell through
// `isThisFieldOrAutoPropTarget` into the generic expression visit, which read
// it as a setter invocation. Separate from T3: two call sites, one on the
// assignment path and one on the read path, and each had to be converted.
// MEASURED before the fix: twin 7, imported rejected.
// ===========================================================================
BEZEL_TEST(xmod_auto_property_storage_predicate_3152,
           ctor_write_to_imported_base_auto_property_is_a_direct_store_22_7) {
    int r = compileImportAndRun3152P(
        "module Lib3152PT4; "
        "public class Base3152PT4 "
        "  public property W: int := 41; "
        "end Base3152PT4;",
        "module App3152PT4; import Lib3152PT4; "
        "public class D3152PT4: Base3152PT4 "
        "  private _s: string; "
        "  public constructor() "
        "    this.W := 5; "
        "    _s := \"ok\"; "
        "  end constructor; "
        "  public property S: string => _s; "
        "end D3152PT4; "
        "public class C3152PT4 "
        "  public static method Main(): int "
        "    using var d := new D3152PT4(); "
        "    return d.W + d.S.Length; "
        "  end Main; "
        "end C3152PT4;");
    if (r == kDepUnavailable3152P) return;
    BEZEL_EXPECT_EQ(r, 7);  // 5 (written through the imported slot) + 2

    int twin = compileAndRun3152P(
        "module Loc3152PT4; "
        "public class BaseL3152PT4 "
        "  public property W: int := 41; "
        "end BaseL3152PT4; "
        "public class DL3152PT4: BaseL3152PT4 "
        "  private _s: string; "
        "  public constructor() "
        "    this.W := 5; "
        "    _s := \"ok\"; "
        "  end constructor; "
        "  public property S: string => _s; "
        "end DL3152PT4; "
        "public class CL3152PT4 "
        "  public static method Main(): int "
        "    using var d := new DL3152PT4(); "
        "    return d.W + d.S.Length; "
        "  end Main; "
        "end CL3152PT4;");
    BEZEL_EXPECT_EQ(twin, 7);
}

// ===========================================================================
// T5 — OVER-REJECTION CONTROL for T3/T4. The same shape, but the imported
// base's `W` is COMPUTED (`=> _w`). §22.7 rule 1 says an accessor invocation
// IS an observation, so this must STAY rejected — and with the same diagnostic
// its single-module twin produces. Passes before AND after. Without this, T3
// would be satisfied by a predicate that answered `true` for every imported
// property, which is the #1462 direction (a wrong `true` in place of a wrong
// `false`).
// ===========================================================================
BEZEL_TEST(xmod_auto_property_storage_predicate_3152,
           ctor_read_of_imported_base_computed_property_still_gated_22_7) {
    std::string diag;
    int r = compileImportAndRun3152P(
        "module Lib3152PT5; "
        "public class Base3152PT5 "
        "  private _w: int := 41; "
        "  public property W: int => _w; "
        "end Base3152PT5;",
        "module App3152PT5; import Lib3152PT5; "
        "public class D3152PT5: Base3152PT5 "
        "  private _s: string; "
        "  public constructor() "
        "    var t := base.W; "
        "    _s := \"v\" + t.ToString(); "
        "  end constructor; "
        "  public property S: string => _s; "
        "end D3152PT5; "
        "public class C3152PT5 "
        "  public static method Main(): int "
        "    using var d := new D3152PT5(); "
        "    return d.S.Length; "
        "  end Main; "
        "end C3152PT5;",
        &diag);
    if (r == kDepUnavailable3152P) return;
    BEZEL_EXPECT_EQ(r, kRejected3152P);
    BEZEL_EXPECT_TRUE(
        contains3152P(diag, "cannot observe `this` (property `W`)"));

    std::string twinDiag;
    int twin = compileAndRun3152P(
        "module Loc3152PT5; "
        "public class BaseL3152PT5 "
        "  private _w: int := 41; "
        "  public property W: int => _w; "
        "end BaseL3152PT5; "
        "public class DL3152PT5: BaseL3152PT5 "
        "  private _s: string; "
        "  public constructor() "
        "    var t := base.W; "
        "    _s := \"v\" + t.ToString(); "
        "  end constructor; "
        "  public property S: string => _s; "
        "end DL3152PT5; "
        "public class CL3152PT5 "
        "  public static method Main(): int "
        "    using var d := new DL3152PT5(); "
        "    return d.S.Length; "
        "  end Main; "
        "end CL3152PT5;",
        &twinDiag);
    BEZEL_EXPECT_EQ(twin, kRejected3152P);
    BEZEL_EXPECT_TRUE(
        contains3152P(twinDiag, "cannot observe `this` (property `W`)"));
}

// ===========================================================================
// T6 — §22.8 step 2: an IMPORTED GENERIC class's auto-property initializer.
//
// An imported non-generic class replays its initializers through the
// exporter's own `bz$<Type>$init` (#2452). A generic one is monomorphized by
// the importer, so `emitOwnFieldInit` runs with `t->decl == nullptr` and must
// replay the wire-carried initializer itself (#2874 / #2524). It filtered its
// storage list with the AST-only predicate, so the auto-property never entered
// the list and both replay branches — written FOR imported members — were dead
// for it while working for fields.
//
// The differential is INSIDE one class: `_f` (a private field, `:= 7`) and `W`
// (an auto-property, `:= 42`) are read by the same method. Access is routed
// through `Sum()` because direct property access on an imported generic is
// refused by #2884, so a test written on `b.W` would be blocked before it
// reached the site under test.
// MEASURED before the fix: twin 49, imported 7 — the field replayed, the
// auto-property read its §7.9 default.
// ===========================================================================
BEZEL_TEST(xmod_auto_property_storage_predicate_3152,
           imported_generic_auto_property_initializer_is_replayed_22_8) {
    int r = compileImportAndRun3152P(
        "module Lib3152PT6; "
        "public class Box3152PT6<T> "
        "  public property W: int := 42; "
        "  private _f: int := 7; "
        "  public property Val: T; "
        "  public constructor(v: T) "
        "    Val := v; "
        "  end constructor; "
        "  public method Sum(): int => W + _f; "
        "end Box3152PT6;",
        "module App3152PT6; import Lib3152PT6; "
        "public class C3152PT6 "
        "  public static method Main(): int "
        "    using var b := new Box3152PT6<int>(5); "
        "    return b.Sum(); "
        "  end Main; "
        "end C3152PT6;");
    if (r == kDepUnavailable3152P) return;
    BEZEL_EXPECT_EQ(r, 49);  // 42 (auto-property) + 7 (field)

    int twin = compileAndRun3152P(
        "module Loc3152PT6; "
        "public class BoxL3152PT6<T> "
        "  public property W: int := 42; "
        "  private _f: int := 7; "
        "  public property Val: T; "
        "  public constructor(v: T) "
        "    Val := v; "
        "  end constructor; "
        "  public method Sum(): int => W + _f; "
        "end BoxL3152PT6; "
        "public class CL3152PT6 "
        "  public static method Main(): int "
        "    using var b := new BoxL3152PT6<int>(5); "
        "    return b.Sum(); "
        "  end Main; "
        "end CL3152PT6;");
    BEZEL_EXPECT_EQ(twin, 49);
}

// ===========================================================================
// T7 — OVER-REJECTION CONTROL for T6. An imported NON-generic class's
// auto-property initializer must still be replayed exactly ONCE, by the
// exporter's `bz$<Type>$init`. `emitOwnFieldInit` is not called for such a type
// at all (measured: it is called for the imported GENERIC and not for the
// imported non-generic), so widening its filter cannot double-init — this test
// is what makes that a checked claim rather than a reading. It also covers a
// `string` auto-property, whose replay is the one with ARC consequences.
// Passes before AND after.
// ===========================================================================
BEZEL_TEST(xmod_auto_property_storage_predicate_3152,
           imported_non_generic_auto_property_initializer_runs_once_22_8) {
    int r = compileImportAndRun3152P(
        "module Lib3152PT7; "
        "public class Plain3152PT7 "
        "  public property W: int := 42; "
        "  public property S: string := \"abc\"; "
        "  private _f: int := 7; "
        "  public property F: int => _f; "
        "end Plain3152PT7;",
        "module App3152PT7; import Lib3152PT7; "
        "public class C3152PT7 "
        "  public static method Main(): int "
        "    using var b := new Plain3152PT7(); "
        "    return b.W + b.F + b.S.Length; "
        "  end Main; "
        "end C3152PT7;");
    if (r == kDepUnavailable3152P) return;
    BEZEL_EXPECT_EQ(r, 52);  // 42 + 7 + 3

    int twin = compileAndRun3152P(
        "module Loc3152PT7; "
        "public class PlainL3152PT7 "
        "  public property W: int := 42; "
        "  public property S: string := \"abc\"; "
        "  private _f: int := 7; "
        "  public property F: int => _f; "
        "end PlainL3152PT7; "
        "public class CL3152PT7 "
        "  public static method Main(): int "
        "    using var b := new PlainL3152PT7(); "
        "    return b.W + b.F + b.S.Length; "
        "  end Main; "
        "end CL3152PT7;");
    BEZEL_EXPECT_EQ(twin, 52);
}

// ===========================================================================
// T8 — §9.2 auto-equality over an IMPORTED body record.
//
// This is the audit's "correct predicate fed by an empty source" shape.
// `isAutoEqualityBearingBodyRecord` checks each auto-property's type against
// §9.2's admissible-component rule; every OTHER `PropertySymbol` falls through
// as computed and non-disqualifying. With the AST-only predicate an imported
// body record's auto-properties all took the computed arm, so the function
// checked NO components and returned `true` — a record whose auto-property is
// class-typed got `=` across a module boundary that §9.2 rejects in one.
// MEASURED before the fix: twin rejected, imported compiled and ran.
// ===========================================================================
BEZEL_TEST(xmod_auto_property_storage_predicate_3152,
           imported_body_record_class_typed_component_is_not_auto_hashable_9_2) {
    std::string diag;
    int r = compileImportAndRun3152P(
        "module Lib3152PT8; "
        "public class Res3152PT8 "
        "  public property N: int := 1; "
        "end Res3152PT8; "
        "public record Bad3152PT8 "
        "  public property R: Res3152PT8; "
        "end Bad3152PT8;",
        "module App3152PT8; import Lib3152PT8; "
        "public class C3152PT8 "
        "  public static method Main(): int "
        "    using var r := new Res3152PT8(); "
        "    var a := new Bad3152PT8(r); "
        "    var b := new Bad3152PT8(r); "
        "    if a = b then "
        "      return 1; "
        "    end if; "
        "    return 2; "
        "  end Main; "
        "end C3152PT8;",
        &diag);
    if (r == kDepUnavailable3152P) return;
    BEZEL_EXPECT_EQ(r, kRejected3152P);
    BEZEL_EXPECT_TRUE(contains3152P(diag, "it is not auto-hashable"));

    std::string twinDiag;
    int twin = compileAndRun3152P(
        "module Loc3152PT8; "
        "public class ResL3152PT8 "
        "  public property N: int := 1; "
        "end ResL3152PT8; "
        "public record BadL3152PT8 "
        "  public property R: ResL3152PT8; "
        "end BadL3152PT8; "
        "public class CL3152PT8 "
        "  public static method Main(): int "
        "    using var r := new ResL3152PT8(); "
        "    var a := new BadL3152PT8(r); "
        "    var b := new BadL3152PT8(r); "
        "    if a = b then "
        "      return 1; "
        "    end if; "
        "    return 2; "
        "  end Main; "
        "end CL3152PT8;",
        &twinDiag);
    BEZEL_EXPECT_EQ(twin, kRejected3152P);
    BEZEL_EXPECT_TRUE(contains3152P(twinDiag, "it is not auto-hashable"));
}

// ===========================================================================
// T9 — OVER-REJECTION CONTROL for T8. An imported body record whose only
// component is VALUE-typed keeps its §9.2 auto-equality. Passes before AND
// after: T8's fix works by making the walk SEE the imported components, and a
// walk that saw them but judged them inadmissible would reject this too.
// The exit code is `1` — `a = b` is true — so this also proves the synthesized
// `=` runs and compares the imported storage, not merely that it compiled.
// ===========================================================================
BEZEL_TEST(xmod_auto_property_storage_predicate_3152,
           imported_body_record_value_typed_component_keeps_equality_9_2) {
    int r = compileImportAndRun3152P(
        "module Lib3152PT9; "
        "public record Good3152PT9 "
        "  public property V: int := 3; "
        "end Good3152PT9;",
        "module App3152PT9; import Lib3152PT9; "
        "public class C3152PT9 "
        "  public static method Main(): int "
        "    var a := new Good3152PT9(); "
        "    var b := new Good3152PT9(); "
        "    if a = b then "
        "      return 1; "
        "    end if; "
        "    return 2; "
        "  end Main; "
        "end C3152PT9;");
    if (r == kDepUnavailable3152P) return;
    BEZEL_EXPECT_EQ(r, 1);

    int twin = compileAndRun3152P(
        "module Loc3152PT9; "
        "public record GoodL3152PT9 "
        "  public property V: int := 3; "
        "end GoodL3152PT9; "
        "public class CL3152PT9 "
        "  public static method Main(): int "
        "    var a := new GoodL3152PT9(); "
        "    var b := new GoodL3152PT9(); "
        "    if a = b then "
        "      return 1; "
        "    end if; "
        "    return 2; "
        "  end Main; "
        "end CL3152PT9;");
    BEZEL_EXPECT_EQ(twin, 1);
}

// ===========================================================================
// T10 — §9.8 / #2364: the base-with-storage guard over an IMPORTED base.
//
// `classHasOwnStorage` is the one walk in `checkClassSerializationEligibility
// Late` that leaves the module being compiled — it climbs `t->baseTypes`. With
// the AST-only predicate an imported base whose only storage is an
// auto-property reported "no own storage", so the guard never fired and a
// class whose synthesized `Deserialize` would silently leave inherited fields
// at their defaults was ADMITTED.
// MEASURED before the fix: twin rejected with the #2364 diagnostic, imported
// compiled and ran (returning 9). After: both reject, same text.
// ===========================================================================
BEZEL_TEST(xmod_auto_property_storage_predicate_3152,
           imported_base_auto_property_storage_blocks_ideserializable_9_8) {
    std::string diag;
    int r = compileImportAndRun3152P(
        "module Lib3152PTA; "
        "public class Base3152PTA "
        "  public property W: int := 41; "
        "end Base3152PTA;",
        "module App3152PTA; import Lib3152PTA; "
        "public class D3152PTA: Base3152PTA, ISerializable, IDeserializable "
        "  public property N: int := 3; "
        "  public constructor(n: int) "
        "    N := n; "
        "  end constructor; "
        "end D3152PTA; "
        "public class C3152PTA "
        "  public static method Main(): int "
        "    using var d := new D3152PTA(9); "
        "    return d.N; "
        "  end Main; "
        "end C3152PTA;",
        &diag);
    if (r == kDepUnavailable3152P) return;
    BEZEL_EXPECT_EQ(r, kRejected3152P);
    BEZEL_EXPECT_TRUE(
        contains3152P(diag, "which carries its own serialized storage"));

    std::string twinDiag;
    int twin = compileAndRun3152P(
        "module Loc3152PTA; "
        "public class BaseL3152PTA "
        "  public property W: int := 41; "
        "end BaseL3152PTA; "
        "public class DL3152PTA: BaseL3152PTA, ISerializable, IDeserializable "
        "  public property N: int := 3; "
        "  public constructor(n: int) "
        "    N := n; "
        "  end constructor; "
        "end DL3152PTA; "
        "public class CL3152PTA "
        "  public static method Main(): int "
        "    using var d := new DL3152PTA(9); "
        "    return d.N; "
        "  end Main; "
        "end CL3152PTA;",
        &twinDiag);
    BEZEL_EXPECT_EQ(twin, kRejected3152P);
    BEZEL_EXPECT_TRUE(
        contains3152P(twinDiag, "which carries its own serialized storage"));
}

}  // namespace bezel::module
