// fix #2464 (§9.1, §32.13 clause 2, v0.235): `Emitter::buildRecordValue`'s
// positional fast path mapped a constructor argument to `fields[i]` (raw
// storage index) rather than to the i-th POSITIONAL field specifically —
// wrong the moment a combined-form record had ANY body storage, since
// `collectRecordStorage`/`t->members` always order body storage AHEAD of
// positional fields, regardless of source declaration order. See
// tests/codegen/record_positional_body_field_order_2464_test.cpp for the
// primary (single-file) regression coverage and the full root-cause writeup.
//
// THIS FILE covers the CROSS-MODULE angle specifically: the fix computes the
// positional-field subset as the trailing `args.size()` entries of `fields`
// rather than filtering on `sema::FieldSymbol::posParam` (a real
// `ast::Param*`) — because `ModuleLoader.cpp` never populates `posParam` for
// an IMPORTED field (there is no wire-level "is this field positional" tag
// on `MetaMember`, only the whole-type `MetaTypeDecl::hasPositionalFields`
// boolean). A posParam-based approach would have left EVERY imported
// record's positional set empty, breaking cross-module positional-record
// construction entirely — this was caught by testing the cross-module case
// specifically after an initial (locally-only-correct) draft of the fix.
//
// T1 below is a combined-form record with a PUBLIC (not private) body field,
// imported across a module boundary — already reachable TODAY independent of
// issue #2456 (which concerns PRIVATE/internal field layout-visibility
// specifically; a PUBLIC body field is already exported and already lands in
// the importer's reconstructed `t->members`, in the same body-before-
// positional order the exporter used). T2 is the pure-positional cross-
// module control (must keep working exactly as before). Neither test depends
// on #2456 landing.
//
// A record with a PRIVATE body field imported across a module boundary
// (matching #2456's own repro shape once its layout-visibility carriage is
// widened to records) was verified via a disposable scratch worktree
// (cherry-picking #2456 + this fix + an experimental widening of
// ModuleMetadataBuilder.cpp's `includeForLayout` gate to
// `TypeDeclKind::Record`): the exact issue #2464 repro
// (`Rec2457(x: int) private _hidden: int := 77; ...; return r.x;`) now
// returns 9 (previously 0) — confirming this fix unblocks #2456's records
// extension for the ordering defect. That extension is NOT shipped by this
// commit: #2456 is not yet on `origin/main` in this worktree, so there is no
// `includeForLayout` gate here to widen. A residual, SEPARATE gap was also
// found during that verification and is out of this issue's scope: a
// record's private body field's own declared initializer value is not
// replayed cross-module at all (its metadata carries no initializer text,
// mirroring #2456's "layout-only, no ABI change" design for classes) — filed
// as a new issue for whoever extends #2456's carriage to records.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sys/wait.h>
#include <unistd.h>

#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile2464 {
    std::string path;
    explicit TempFile2464(std::string p) : path(std::move(p)) {}
    ~TempFile2464() { std::remove(path.c_str()); }
};

void writeSource2464(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2464(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile lib -> object, compile app (importing lib) -> exe, run it in a
// SEPARATE process, return its exit code (-1 on compile/link/run failure,
// -2 if the dependency itself could not be produced). Mirrors #2452/#2456's
// own compileImportAndRun helpers.
int compileImportAndRun2464(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2464_" + std::to_string(std::rand());
    TempFile2464 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2464 aObj((dir / (stem + "_a.o")).string());
    writeSource2464(aSrc.path, libSrc);
    if (!compileToObj2464(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2464 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2464 bExe((dir / stem).string());
    writeSource2464(bSrc.path, appSrc);
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

}  // namespace

// ===========================================================================
// T1 — combined-form record with a PUBLIC plain body FIELD (`public _y: int
// := 25;` — an explicit field declaration, §9.2, not an auto-property),
// imported across a module boundary, no explicit constructor. A plain field
// is used rather than an auto-property because an auto-property's imported
// reconstruction was found NOT to register with `isAutoPropertySymbol` on
// the importer side — `collectRecordStorage` then silently omits it, which
// made an earlier draft of this test using `property Y: int := 25;` pass
// EVEN WITHOUT THE FIX (a vacuous test caught while demonstrating fail-red:
// with only 2 positional args and Y excluded from importer-side storage,
// fields.size() coincidentally already equalled args.size() pre-fix). A
// plain field IS carried into `fields` on both sides (dynamic_cast<
// FieldSymbol> in collectRecordStorage), so it genuinely reproduces the
// defect: the importer reconstructs `t->members` in the SAME
// body-before-positional order the exporter used
// (ModuleMetadataBuilder.cpp's export loop and ModuleLoader.cpp's
// reconstruction loop are both single linear passes over `t.members` —
// filtering, never reordering).
// Pre-fix (confirmed by patch-out/rebuild): `r.x`/`r.z` silently stayed 0 —
// buildRecordValue's fast path never fired (importer-side fields.size()=3
// [_y, x, z] != args.size()=2; the fallback found no explicit ctor and
// returned nullptr, leaving the local's storage at its zero default).
// Post-fix: x=7, z=3 -> 10.
//
// Deliberately does NOT assert on `r._y`'s (inaccessible — private via the
// `public` FIELD's own read path is still a plain read, but its VALUE is not
// checked here) cross-module value: a genuinely separate, pre-existing
// defect was found while diagnosing this test — an imported record's
// body-declared field/auto-property initializer is never replayed
// cross-module at all (confirmed independently with a body-only record
// having ZERO positional fields, so it cannot be this fix's ordering logic:
// `new BodOnly() -> b.W` returns 0 instead of its declared `property W: int
// := 42;`, both before and after this fix). That is a distinct root cause
// (initializer TEXT/metadata propagation, mirroring what #2452 built for
// classes' `bz$<Base>$init`, but records have no equivalent
// construction-time entry point to route through) — filed separately rather
// than folded into this ordering fix.
// ===========================================================================
BEZEL_TEST(xmod_record_combined_form_import_2464,
           imported_combined_form_public_body_field_spec_9_1) {
    int r = compileImportAndRun2464(
        "module LibA2464; "
        "public record RecA2464(x: int, z: int) "
        "  public _y: int := 25; "
        "end RecA2464;",
        "module AppA2464; import LibA2464; "
        "public class C2464a "
        "  public static method Main(): int "
        "    var r := new RecA2464(7, 3); "
        "    return r.x + r.z; "
        "  end Main; "
        "end C2464a;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 10);
}

// ===========================================================================
// T2 — CONTROL: a pure POSITIONAL (no body storage) record imported across a
// module boundary. Must remain correct exactly as before the fix — this is
// the shape that "worked by luck" pre-fix, since with zero body storage
// fields.size() == posFields.size() == args.size() unconditionally.
// ===========================================================================
BEZEL_TEST(xmod_record_combined_form_import_2464,
           imported_positional_only_record_control_spec_9_1) {
    int r = compileImportAndRun2464(
        "module LibB2464; "
        "public record RecB2464(x: int, y: int);",
        "module AppB2464; import LibB2464; "
        "public class C2464b "
        "  public static method Main(): int "
        "    var r := new RecB2464(3, 4); "
        "    return r.x + r.y; "
        "  end Main; "
        "end C2464b;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 7);
}

// ===========================================================================
// T3 — CONTROL: the SAME combined-form shape as T1 (same field names/types),
// but fully LOCAL (no import at all) — guards against the cross-module test
// harness itself (rather than the fix) accounting for T1's pass. Unlike T1,
// this DOES assert on the body field's value too (`r._y`, via its own
// value — read directly since it is `public`): a local record has no
// cross-module initializer-replay gap, so `_y` is correctly 25 here,
// isolating that separate defect to the cross-module path specifically.
// ===========================================================================
BEZEL_TEST(xmod_record_combined_form_import_2464,
           local_combined_form_public_body_field_control_spec_9_1) {
    bezel::Driver driver;
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2464_t3_" + std::to_string(std::rand());
    TempFile2464 src((dir / (stem + ".bzl")).string());
    TempFile2464 exe((dir / stem).string());
    writeSource2464(src.path,
                    "module LocalC2464; "
                    "public record RecC2464(x: int, z: int) "
                    "  public _y: int := 25; "
                    "end RecC2464; "
                    "public class C2464c "
                    "  public static method Main(): int "
                    "    var r := new RecC2464(7, 3); "
                    "    return r.x + r.z + r._y; "
                    "  end Main; "
                    "end C2464c;");
    BEZEL_EXPECT_TRUE(driver.compileFile(src.path, exe.path, {}).success);
    int status = std::system(exe.path.c_str());
    int r = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(r, 35);  // 7 + 3 + 25
}

}  // namespace bezel::module
