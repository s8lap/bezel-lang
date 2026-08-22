// Spec-derived regression test for issue #3199 — the named-property twin of
// the #2832/#2837 indexer reject on `base.Member` across modules.
//
// §12.3 (`base.Member`) / §23 (Properties) / §23.4 (Indexers — the indexer
// twin §12.3/§23.4 already rejects cleanly via #2832) / §2.8.2 clause 1 /
// §2.8.3 rule 4, v0.254.
//
// ── THE DEFECT ─────────────────────────────────────────────────────────────
//
// §12.3, verbatim: "Inside a derived member, `base.Name(...)` calls the base
// class's implementation, bypassing virtual dispatch." §23 makes this cover
// properties the same way it covers methods and indexers (§23.4 makes
// indexers participate in virtual dispatch "like methods").
//
// The named-PROPERTY form (`base.P`) across a module boundary silently
// degraded to a wrong value (or zero/null) on HEAD, where the indexer form
// (`base[i]`) on the same shape was a clean §12.3 compile error:
//
//   * `emitExportedPropertyGetters` / `-Setters` (LlvmBackend_Setup.cpp)
//     skipped a virtual property ("Skip virtual/override/abstract/static —
//     they have other dispatch paths"), so the dependency object's
//     `bz$<Class>$<Prop>$propget` accessor carried InternalLinkage (a
//     lowercase `t` in `nm` output), and the property had no
//     cross-module-stable ABI symbol;
//   * `declareImportedPropertyGetters` / `-Setters` correspondingly skipped
//     it (`if (propGetSlot_.count(ps)) continue;`);
//   * The imported-property read lowering found nothing in
//     `importedPropGetterFns_` and fell through to the #2491 zero/null
//     degradation. The WRITE half (`base.P := v`) silently dropped the
//     write through the same hole — the setter map was empty too.
//
// MEASURED on HEAD with `bezelc -c lib.bzl -o lib.o` then
// `--import lib.o app.bzl` (the issue's own repro):
//
//   spelling        pre-fix exit / value         post-#3199
//   base.P          exit 0, runtime -1          clean §12.3 ERROR
//   base.P := 7     exit 0, runtime 3           clean §12.3 ERROR
//   base.P += 1     exit 0, runtime 3           clean §12.3 ERROR
//   base.P++        exit 0, runtime 3           clean §12.3 ERROR
//
// The twin with the module boundary removed (`module` declared once,
// classes defined together) returns 8 — the base class's own
// `_v + 5 = 3 + 5` — so the defect is purely a cross-module ABI gap.
// The LOCAL (single-module) virtual `base.P` already returns 8 on HEAD.
//
// ── THE FIX (TWO-PHASE) ────────────────────────────────────────────────────
//
// Phase 1 — `rejectImportedVirtualBaseProperty` (LlvmBackend_Assign.cpp),
// the named-property twin of `rejectImportedVirtualBaseIndexer` (#2832).
// Called from the imported-property read lowering (LlvmBackend_Exprs.cpp
// ~5524), the direct-write lowering `lowerImportedPropertySet`, and the
// compound / increment path `lowerImportedPropertyCompoundCore`. Reported
// a clean §12.3 ERROR (not the silent `internal:` warning #2491 surfaces)
// — the same "a clean compile error beats a silent wrong value" rationale
// #2832's header records. The ABI addition that would actually MAKE
// `base.P` work cross-module was out of scope for the interim (#2837's
// fix direction, property analog).
//
// Phase 2 (#3231, this file's current state) — the export-gate widening
// `emitExportedPropertyGetters` / `-Setters` to emit
// `bz$<Class>$<Prop>$propget` / `$propset` for VIRTUAL members too. The
// importer side declares the matching external in the new
// `importedPropGetterDirectFns_` / `importedPropSetterDirectFns_` maps,
// and `lowerImportedPropertyRead` / `lowerImportedPropertySet` /
// `lowerImportedPropertyCompoundCore` route `base.X` through those
// direct-entry maps. `rejectImportedVirtualBaseProperty` is now an
// ABSTRACT-only gate (the §12.5-correct behaviour: an abstract base
// accessor has no OWNER body to call either way).
//
// T1 (all four sub-cases) is updated to assert the post-#3231 behaviour:
// the program compiles, runs, and returns the OWNER class's own
// implementation — the §12.3-correct answer.
//
// ── CONTROLS ───────────────────────────────────────────────────────────────
//
// T1 — the OWNER-direct path. Four sub-cases (read / write / compound /
//      increment), each compiles the cross-module build, runs it, and
//      asserts agreement with the LOCAL twin — both return the OWNER's
//      body answer (8 for read; 7 for write `base.P := 7`; 9 for the
//      compound/increment on the OWNER's 3+5 base value), proving the
//      dispatch did NOT run through the vtable to the override.
//
// T2 — the NON-virtual control. `base.P` on an IMPORTED NON-VIRTUAL
//      base property is the existing correct path (#503, #2414, #2489);
//      it must still compile and link, and the program must return the
//      base class's own getter value — the same 8 a local module would
//      return. This pins "the widening did not regress the correct
//      non-virtual shape".
//
// T3 — the LOCAL twin control. The character-identical single-module
//      program (no `import` boundary, `base.P` on a local virtual base
//      property overridden locally) must STILL compile and return 8.
//      This pins "the cross-module widening did not perturb the LOCAL
//      `base.P` path on a virtual base property".

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
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile3199 {
    std::string path;
    explicit TempFile3199(std::string p) : path(std::move(p)) {}
    ~TempFile3199() { std::remove(path.c_str()); }
};

void writeSource3199(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile lib -> object, compile app (importing lib) -> exe WITHOUT running,
// capturing stderr so a text assertion can pin the diagnostic.
// Returns true if the compile SUCCEEDED (so a test that wants a rejected
// build asserts `!success`).
bool compileImportCaptureErr3199(const std::string& libSrc,
                                 const std::string& appSrc,
                                 std::string& err) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3199c_");
    TempFile3199 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3199 aObj((dir / (stem + "_a.o")).string());
    writeSource3199(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success ||
            !std::ifstream(aObj.path).good())
            return false;
    }
    TempFile3199 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3199 bExe((dir / (stem + "_b")).string());
    writeSource3199(bSrc.path, appSrc);

    std::string errPath =
        (dir / ("bz3199_err_" + unique_stem("") + ".txt")).string();
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
        success = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path,
                                                  deps)
                      .success;
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

// Compile lib -> object, compile app -> exe, run it in a separate process,
// return its exit code. -1 on compile/link/run failure, -2 if the dependency
// itself could not be produced (toolchain unavailable).
int compileImportAndRun3199(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3199_");
    TempFile3199 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3199 aObj((dir / (stem + "_a.o")).string());
    writeSource3199(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success ||
            !std::ifstream(aObj.path).good())
            return -2;
    }
    TempFile3199 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3199 bExe((dir / stem).string());
    writeSource3199(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
             .success)
        return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// Single-module twin runner: same declarations with the module boundary
// removed. The twin is the oracle for the correct value of `base.P` on a
// non-virtual imported base property (T2) and for the local virtual
// `base.P` control (T3).
int compileLocalAndRun3199(const std::string& src) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3199l_");
    TempFile3199 aSrc((dir / (stem + ".bzl")).string());
    TempFile3199 aExe((dir / stem).string());
    writeSource3199(aSrc.path, src);
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, aSrc.path, aExe.path).success)
        return -1;
    int status = std::system(aExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// The dependency's declarations. `_v` starts at 3, so a base-class read of
// `P` returns 8 and the corresponding setter leaves `_v` at the assigned
// value. The `override` exists in the app so §12.3's "base class's own
// implementation" reading is the one the program is asking for.
const char* kLib3199Head = "module LibM6C3199; ";
const char* kLib3199Body =
    "public class BaseM6C3199 "
    "  private _v: int := 3; "
    "  public virtual property P: int "
    "    get => _v + 5; "
    "    set => _v := value; "
    "  end; "
    "end BaseM6C3199; ";

// T2 / T3 use this variant — a NON-virtual `P` on the same base. The read
// must still reach the base class's own getter (returns 8); the new reject
// must NOT fire (the property has no vtable slot, so the importer-side
// `importedPropGetterFns_` already declares the direct `bz$…$propget` stub
// and the call dispatches through it correctly).
const char* kLib3199NVBody =
    "public class BaseM6CNV3199 "
    "  private _v: int := 3; "
    "  public property P: int "
    "    get => _v + 5; "
    "    set => _v := value; "
    "  end; "
    "end BaseM6CNV3199; ";

std::string imported3199(const std::string& libBody, const std::string& appBody) {
    return "module AppM6C3199; import LibM6C3199; " + libBody + appBody;
}

}  // namespace

// ===========================================================================
// T1 — THE ISSUE'S OWN REPRO. Pre-fix: exit 0, runtime -1 (zero/null
// degradation), only an `internal:` warning. Post-fix (#3199's interim):
// a clean §12.3 compile error — the named-property twin of the indexer
// reject (`rejectImportedVirtualBaseIndexer` / #2832).
// Post-post-fix (#3231): the §12.3 reject is REPLACED by an OWNER-direct
// call — `base.P` now compiles, runs, and returns the BASE class's own
// implementation (8 = `_v + 5`), which is exactly what §12.3 requires.
// The test is updated to assert the program compiles and returns 8 —
// the §12.3-correct answer — rather than rejecting.
// ===========================================================================
BEZEL_TEST(xmod_base_p_imported_virtual_property_3199,
           base_p_read_on_imported_virtual_base_uses_owner_body_12_3) {
    // Single-module twin: same declarations with the module boundary removed.
    // Both sides must agree on the OWNER's getter value (8), proving the
    // imported `base.P` read the BASE class's implementation, NOT a
    // vtable-dispatched override (the override returns 200 + _w = 200) and
    // NOT a silent zero.
    const std::string twinSrc =
        std::string("module LocalM6C3199; ") + kLib3199Body +
        "public class DM6C3199: BaseM6C3199 "
        "  private _w: int := 0; "
        "  public override property P: int "
        "    get => 200 + _w; "
        "    set => _w := value; "
        "  end; "
        "  public method ViaBase(): int => base.P; "
        "end DM6C3199; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new DM6C3199(); "
        "    return d.ViaBase(); "
        "  end Main; "
        "end C;";
    int twin = compileLocalAndRun3199(twinSrc);
    BEZEL_EXPECT_EQ(twin, 8);
    int imported = compileImportAndRun3199(
        std::string(kLib3199Head) + kLib3199Body,
        imported3199("",
            "public class DM6C3199: BaseM6C3199 "
            "  private _w: int := 0; "
            "  public override property P: int "
            "    get => 200 + _w; "
            "    set => _w := value; "
            "  end; "
            "  public method ViaBase(): int => base.P; "
            "end DM6C3199; "
            "public class C "
            "  public static method Main(): int "
            "    using var d := new DM6C3199(); "
            "    return d.ViaBase(); "
            "  end Main; "
            "end C;"));
    if (imported == -2) return;  // dependency toolchain unavailable — skip
    // §12.3: base.P must return the OWNER's body (8 = _v + 5), NOT the
    // override (200) and NOT a silent 0.
    BEZEL_EXPECT_EQ(imported, 8);
    BEZEL_EXPECT_EQ(imported, twin);
}

// ===========================================================================
// T1 (b) — the WRITE half. `base.P := v` on the same shape silently dropped
// the write on HEAD (measured `_v` left at 3, runtime 3 where 7 is correct);
// the interim #3199 fix made it reject with §12.3. Post-#3231 the
// OWNER-direct setter is invoked: `base.P := 7` writes 7 into the OWNER's
// own `_v` (NOT the override's `_w`), so `return _v` reads 7.
// ===========================================================================
BEZEL_TEST(xmod_base_p_imported_virtual_property_3199,
           base_p_write_on_imported_virtual_base_writes_owner_slot_12_3) {
    const std::string twinSrc =
        std::string("module LocalM6CW3199; ") + kLib3199Body +
        "public class DM6CW3199: BaseM6C3199 "
        "  private _w: int := 0; "
        "  public override property P: int "
        "    get => 200 + _w; "
        "    set => _w := value; "
        "  end; "
        "  public method ViaBaseSet(): int "
        "    base.P := 7; "
        "    return _v; "
        "  end ViaBaseSet; "
        "end DM6CW3199; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new DM6CW3199(); "
        "    return d.ViaBaseSet(); "
        "  end Main; "
        "end C;";
    int twin = compileLocalAndRun3199(twinSrc);
    BEZEL_EXPECT_EQ(twin, 7);
    int imported = compileImportAndRun3199(
        std::string(kLib3199Head) + kLib3199Body,
        imported3199("",
            "public class DM6CW3199: BaseM6C3199 "
            "  private _w: int := 0; "
            "  public override property P: int "
            "    get => 200 + _w; "
            "    set => _w := value; "
            "  end; "
            "  public method ViaBaseSet(): int "
            "    base.P := 7; "
            "    return _v; "
            "  end ViaBaseSet; "
            "end DM6CW3199; "
            "public class C "
            "  public static method Main(): int "
            "    using var d := new DM6CW3199(); "
            "    return d.ViaBaseSet(); "
            "  end Main; "
            "end C;"));
    if (imported == -2) return;
    BEZEL_EXPECT_EQ(imported, 7);
    BEZEL_EXPECT_EQ(imported, twin);
}

// ===========================================================================
// T1 (c) — the COMPOUND half. `base.P += 1` reads the OWNER's value (8),
// adds 1, writes back through the OWNER-direct setter: `_v` becomes 9.
// The override's `_w` is NOT touched (proves the dispatch did not run
// through the vtable to the override's getter/setter).
// ===========================================================================
BEZEL_TEST(xmod_base_p_imported_virtual_property_3199,
           base_p_compound_on_imported_virtual_base_uses_owner_12_3) {
    const std::string twinSrc =
        std::string("module LocalM6CC3199; ") + kLib3199Body +
        "public class DM6CC3199: BaseM6C3199 "
        "  private _w: int := 0; "
        "  public override property P: int "
        "    get => 200 + _w; "
        "    set => _w := value; "
        "  end; "
        "  public method ViaBaseCompound(): int "
        "    base.P += 1; "
        "    return _v; "
        "  end ViaBaseCompound; "
        "end DM6CC3199; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new DM6CC3199(); "
        "    return d.ViaBaseCompound(); "
        "  end Main; "
        "end C;";
    int twin = compileLocalAndRun3199(twinSrc);
    BEZEL_EXPECT_EQ(twin, 9);
    int imported = compileImportAndRun3199(
        std::string(kLib3199Head) + kLib3199Body,
        imported3199("",
            "public class DM6CC3199: BaseM6C3199 "
            "  private _w: int := 0; "
            "  public override property P: int "
            "    get => 200 + _w; "
            "    set => _w := value; "
            "  end; "
            "  public method ViaBaseCompound(): int "
            "    base.P += 1; "
            "    return _v; "
            "  end ViaBaseCompound; "
            "end DM6CC3199; "
            "public class C "
            "  public static method Main(): int "
            "    using var d := new DM6CC3199(); "
            "    return d.ViaBaseCompound(); "
            "  end Main; "
            "end C;"));
    if (imported == -2) return;
    BEZEL_EXPECT_EQ(imported, 9);
    BEZEL_EXPECT_EQ(imported, twin);
}

// ===========================================================================
// T1 (d) — the INCREMENT half. `base.P++` reads OWNER's value (8),
// increments, writes OWNER's slot: `_v` becomes 9 (same expected answer
// as T1c; the difference between `+= 1` and `++` is purely §17.6 spelling).
// ===========================================================================
BEZEL_TEST(xmod_base_p_imported_virtual_property_3199,
           base_p_increment_on_imported_virtual_base_uses_owner_12_3) {
    const std::string twinSrc =
        std::string("module LocalM6CI3199; ") + kLib3199Body +
        "public class DM6CI3199: BaseM6C3199 "
        "  private _w: int := 0; "
        "  public override property P: int "
            "    get => 200 + _w; "
            "    set => _w := value; "
            "  end; "
            "  public method ViaBaseIncr(): int "
            "    base.P++; "
            "    return _v; "
            "  end ViaBaseIncr; "
            "end DM6CI3199; "
            "public class C "
            "  public static method Main(): int "
            "    using var d := new DM6CI3199(); "
            "    return d.ViaBaseIncr(); "
            "  end Main; "
            "end C;";
    int twin = compileLocalAndRun3199(twinSrc);
    BEZEL_EXPECT_EQ(twin, 9);
    int imported = compileImportAndRun3199(
        std::string(kLib3199Head) + kLib3199Body,
        imported3199("",
            "public class DM6CI3199: BaseM6C3199 "
            "  private _w: int := 0; "
            "  public override property P: int "
            "    get => 200 + _w; "
            "    set => _w := value; "
            "  end; "
            "  public method ViaBaseIncr(): int "
            "    base.P++; "
            "    return _v; "
            "  end ViaBaseIncr; "
            "end DM6CI3199; "
            "public class C "
            "  public static method Main(): int "
            "    using var d := new DM6CI3199(); "
            "    return d.ViaBaseIncr(); "
            "  end Main; "
            "end C;"));
    if (imported == -2) return;
    BEZEL_EXPECT_EQ(imported, 9);
    BEZEL_EXPECT_EQ(imported, twin);
}

// ===========================================================================
// T2 — THE NON-VIRTUAL CONTROL. `base.P` on an IMPORTED NON-VIRTUAL base
// property is the existing correct path (#503 / #2414 / #2489): the
// exporter emits the `bz$…$propget` stub directly, the importer's
// `importedPropGetterFns_` already has the entry, and the call dispatches
// through it. The reject must NOT fire here. Asserted via RUN, not just
// compile — the rejected test path's #2491 silent-zero bug class would
// also produce a zero exit, indistinguishable from "compiled but ran the
// wrong code". Runs the import build AND its single-module twin and
// requires both to return the same 8 (the base class's own getter, no
// override exists to confuse the dispatch), proving the imported call
// really reached the base class's getter body, not the silent-zero
// backstop.
// ===========================================================================
BEZEL_TEST(xmod_base_p_imported_virtual_property_3199,
           base_p_on_imported_nonvirtual_base_still_works_12_3_23) {
    const std::string libSrc =
        std::string("module LibM6CNV3199; ") + kLib3199NVBody;
    // For the IMPORTED build, `appSrc` opens with its own module decl +
    // import; for the LOCAL twin, that header is replaced by the same
    // `module` + body prefix used everywhere in this file. The two forms
    // must agree on the value `base.P` returns (8 = `_v + 5`).
    const std::string appSrc =
        "module AppM6CNV3199; import LibM6CNV3199; "
        "public class DM6CNV3199: BaseM6CNV3199 "
        "  public method ViaBase(): int => base.P; "
        "end DM6CNV3199; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new DM6CNV3199(); "
        "    return d.ViaBase(); "
        "  end Main; "
        "end C;";
    const std::string twinSrc =
        std::string("module LocalM6CNV3199; ") + kLib3199NVBody +
        // Strip the `module AppM6CNV3199; import LibM6CNV3199; ` header —
        // a single source file cannot declare TWO modules. The classes that
        // follow the header are the same.
        appSrc.substr(std::string("module AppM6CNV3199; import LibM6CNV3199; ").size());
    int twin = compileLocalAndRun3199(twinSrc);
    BEZEL_EXPECT_EQ(twin, 8);
    int imported = compileImportAndRun3199(libSrc, appSrc);
    if (imported == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(imported, 8);
    BEZEL_EXPECT_EQ(imported, twin);
}

// ===========================================================================
// T3 — THE LOCAL TWIN CONTROL. The character-identical single-module
// program (no `import` boundary, `base.P` on a LOCAL virtual base property
// overridden locally) must STILL compile and return 8. This pins "the
// reject only fires for the cross-module shape; the LOCAL `base.P` on a
// virtual base property is not touched" — the §12.3 non-virtual lowering
// the LOCAL path already does correctly (the `lowerVirtualPropertyGet`
// `base` early-bail at LlvmBackend_Exprs.cpp ~6066, the static
// computedGetterOf path).
// ===========================================================================
BEZEL_TEST(xmod_base_p_imported_virtual_property_3199,
           base_p_on_local_virtual_base_still_works_12_3) {
    int got = compileLocalAndRun3199(
        "module LocalM6C3199; "
        "public class B "
        "  private _v: int := 3; "
        "  public virtual property P: int "
        "    get => _v + 5; "
        "    set => _v := value; "
        "  end; "
        "end B; "
        "public class D: B "
        "  private _w: int := 0; "
        "  public override property P: int "
        "    get => 200 + _w; "
        "    set => _w := value; "
        "  end; "
        "  public method ViaBase(): int => base.P; "
        "end D; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    return d.ViaBase(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(got, 8);
}

}  // namespace bezel::module
