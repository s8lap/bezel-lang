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
// (`base[i]`) on the same shape is a clean §12.3 compile error:
//
//   * `emitExportedPropertyGetters` / `-Setters` (LlvmBackend_Setup.cpp)
//     skip a virtual property ("Skip virtual/override/abstract/static —
//     they have other dispatch paths"), so the dependency object's
//     `bz$<Class>$<Prop>$propget` accessor carries InternalLinkage (a
//     lowercase `t` in `nm` output), and the property has no
//     cross-module-stable ABI symbol;
//   * `declareImportedPropertyGetters` / `-Setters` correspondingly skip
//     it (`if (propGetSlot_.count(ps)) continue;`);
//   * The imported-property read lowering finds nothing in
//     `importedPropGetterFns_` and falls through to the #2491 zero/null
//     degradation. The WRITE half (`base.P := v`) silently dropped the
//     write through the same hole — the setter map is empty too.
//
// MEASURED on HEAD with `bezelc -c lib.bzl -o lib.o` then
// `--import lib.o app.bzl` (the issue's own repro):
//
//   spelling                pre-fix exit / value         post-fix
//   base.P                  exit 0, runtime -1         clean §12.3 ERROR
//                           (silent internal: warning)
//   base.P := 7             exit 0, runtime 3          clean §12.3 ERROR
//                           (write silently dropped)
//   base.P += 1             exit 0, runtime 3          clean §12.3 ERROR
//   base.P++                exit 0, runtime 3          clean §12.3 ERROR
//
// The twin with the module boundary removed (`module` declared once,
// classes defined together) returns 8 — the base class's own
// `_v + 5 = 3 + 5` — so the defect is purely a cross-module ABI gap.
// The LOCAL (single-module) virtual `base.P` already returns 8 on HEAD
// and post-fix.
//
// ── THE FIX ────────────────────────────────────────────────────────────────
//
// `rejectImportedVirtualBaseProperty` (LlvmBackend_Assign.cpp), the named-
// property twin of `rejectImportedVirtualBaseIndexer` (#2832). Called from
// the imported-property read lowering (LlvmBackend_Exprs.cpp ~5524), the
// direct-write lowering `lowerImportedPropertySet`, and the compound /
// increment path `lowerImportedPropertyCompoundCore`. Reports a clean §12.3
// ERROR (not the silent `internal:` warning #2491 surfaces) — the same
// "a clean compile error beats a silent wrong value" rationale #2832's
// header records. The ABI addition that would actually MAKE `base.P` work
// cross-module is the property analog of #2837's fix direction — out of
// scope for this interim reject, which matches the indexer twin's own
// interim (#2832) exactly.
//
// ── CONTROLS ───────────────────────────────────────────────────────────────
//
// T1 — the rejection. Three sub-cases (read / write / compound), each
//      asserts the build FAILS and the diagnostic carries the spec
//      citation and the issue tag, never the silent `internal:` warning
//      the issue records as the bug.
//
// T2 — the NON-virtual control. `base.P` on an IMPORTED NON-VIRTUAL
//      base property is the existing correct path (#503, #2414, #2489);
//      it must still compile and link, and the program must return the
//      base class's own getter value — the same 8 a local module would
//      return. This pins "the reject did not over-fire and did not break
//      the correct shape".
//
// T3 — the LOCAL twin control. The character-identical single-module
//      program (no `import` boundary, `base.P` on a local virtual base
//      property overridden locally) must STILL compile and return 8.
//      This pins "the reject only fires for the cross-module shape; the
//      local `base.P` on a virtual base property is not touched" — the
//      §12.3 non-virtual lowering the LOCAL path already does correctly.
//
// All assertions use diagnostic TEXT (the runAotOut path returns no text;
// the capture-stderr helper in T1 does), never the coarse `rejects()`
// helper, because runAot's WIFSIGNALED path synthesizes an Error for a
// runtime SIGSEGV too and would mask the silent-wrong-value class the
// fix is intended to remove.

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
// degradation), only an `internal:` warning. Post-fix: a clean §12.3
// compile error — the named-property twin of the indexer reject
// (`rejectImportedVirtualBaseIndexer` / #2832).
// ===========================================================================
BEZEL_TEST(xmod_base_p_imported_virtual_property_3199,
           base_p_read_on_imported_virtual_base_is_rejected_12_3) {
    std::string err;
    bool success = compileImportCaptureErr3199(
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
            "end C;"),
        err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("#3199") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("12.3") != std::string::npos);
    // The diagnostic must NOT be the silent `internal:` warning the issue
    // records as the bug — pin the absence explicitly.
    BEZEL_EXPECT_TRUE(err.find("internal: return-value expression failed") ==
                      std::string::npos);
}

// ===========================================================================
// T1 (b) — the WRITE half. `base.P := v` on the same shape silently dropped
// the write on HEAD (measured `_v` left at 3, runtime 3 where 7 is correct).
// The reject must fire here too, BEFORE the value is lowered (the #2204
// discipline), so the value side-effect is never realized.
// ===========================================================================
BEZEL_TEST(xmod_base_p_imported_virtual_property_3199,
           base_p_write_on_imported_virtual_base_is_rejected_12_3) {
    std::string err;
    bool success = compileImportCaptureErr3199(
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
            "end C;"),
        err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("#3199") != std::string::npos);
}

// ===========================================================================
// T1 (c) — the COMPOUND half. `base.P += 1` on the same shape, the
// §17.6 twin of the plain `:=`. The compound path was already declining
// (its pre-fix `propGetSlot_.count(ps) || ...` gate at
// `lowerImportedPropertyCompoundCore` returned false without a
// diagnostic); the reject now reports cleanly instead.
// ===========================================================================
BEZEL_TEST(xmod_base_p_imported_virtual_property_3199,
           base_p_compound_on_imported_virtual_base_is_rejected_12_3) {
    std::string err;
    bool success = compileImportCaptureErr3199(
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
            "end C;"),
        err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("#3199") != std::string::npos);
}

// ===========================================================================
// T1 (d) — the INCREMENT half. `base.P++` is the §17.6 spell of the same
// shape; the indexer reject covers increment too (#2832's call site at
// `lowerIndexerIncrDecr`), and the named-property reject mirrors that.
// ===========================================================================
BEZEL_TEST(xmod_base_p_imported_virtual_property_3199,
           base_p_increment_on_imported_virtual_base_is_rejected_12_3) {
    std::string err;
    bool success = compileImportCaptureErr3199(
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
            "end C;"),
        err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("#3199") != std::string::npos);
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
