// Regression tests for #3358 — a `Span<T>` value the DEPENDENCY boxed was
// misreported as a `string` by the runtime discriminator
// (`bz_obj_is_string`'s whitelist NEGATION fell through), so every
// `is`/`case`/`ToString`/`Equals` site over it answered wrongly.  This is
// the SPAN residual of #2797: #2797's fix closed the record/enum half
// (ExternalLinkage + module-qualified `bvt$box$<M>.<R>` plus consumer-side
// declaration of that symbol in `bz$vtable$table`) but the SPAN box vtable
// machinery (`ensureSpanBoxVtable`) is a SEPARATE code path keyed on
// `isSpanLikeTypeName` rather than on `TypeDeclKind::Record` — so the same
// InternalLinkage defect lived on, and the analogous three modifications are
// the SPAN-shaped fix.
//
// WHAT THE DOCUMENTS REQUIRE.  spec §2.8.3 rule 4 (v0.240): a type has "one
// runtime type identity — `is` tests, `catch` clauses, and §19 patterns over
// values of the instantiation match identically regardless of which artifact
// produced the value or performs the test".  A Span value the dependency
// boxed is exactly such a cross-artifact value.
//
// THE FIX has three sides, mirroring #2797:
//
//   EXPORTER (`ensureSpanBoxVtable` in LlvmBackend_Setup.cpp):
//     - `btd$spanbox` (the descriptor at the head of every Span box's chain)
//       is now LinkOnceODR.  Without this the dependency writes its OWN
//       `btd$spanbox` (InternalLinkage, bare name) into its vtable[0]; the
//       consumer has a SEPARATE `btd$spanbox` (also InternalLinkage, bare
//       name) at a different address; `emitIsA`'s descriptor-chain walk
//       compares against the consumer's `btd$spanbox` and silently fails —
//       even when the vtable-table entry is correct (the discriminator's
//       "string" mis-fire is the FIRST failure, but without LinkOnceODR the
//       second-stage `emitIsA` walk is also broken).  LinkOnceODR unifies the
//       address across modules (the #2301 `ptd$<nm>` precedent).
//     - `bvt$spanbox$<head>` is now ExternalLinkage with a module-QUALIFIED
//       name (`bvt$spanbox$<own-module>.<head>`, mirroring #1488's class-
//       vtable mangling + #2797's `bvt$box$<M>.<R>`).  Before: each
//       module's mint was InternalLinkage at a private address the
//       dependency could not reference from its `bz$vtable$table`; the
//       discriminator's "not in table ⇒ string" premise mis-fired.
//     - All four heads (`Span`/`ReadOnlySpan`/`ArraySpan`/
//       `ReadOnlyArraySpan`) are now pre-minted unconditionally at module
//       finalize time.  A dependency's export of a Span box might use only
//       one head (e.g. `bvt$spanbox$<Lib>.Span` for `Span<int>`), but the
//       consumer's declaration block below declares all four heads per
//       imported module name and expects the dependency to satisfy every
//       declaration — a partial emit leaves three of the four unresolved
//       externals → link error.  The pre-mint loop eliminates that hazard by
//       making every dependency emit all four heads (with `i32` as the
//       sentinel element type; the body is the same `ensureSpanBoxVtable`
//       body keyed by head alone, and the runtime's `is`-discriminator
//       reads only the address, not the elemTy).
//
//   CONSUMER (`declareVtables` in LlvmBackend_Setup.cpp, after #2797's
//   record/enum block):
//     - For every IMPORTED MODULE NAME (via `importedByModule` — keyed on
//       `md.moduleName` for every loaded dependency), declare the four
//       heads `bvt$spanbox$<module>.<head>` ExternalLinkage with no
//       initializer and push into `allVtables_`.  A consumer that reaches
//       the dependency through `Object`/`IReadOnlySpan` has no Span
//       TypeSymbol in `importedSymbols` — the per-MODULE walk finds the
//       dependency by name and declares for it.  At link time the
//       consumer's reference resolves to the dependency's pre-mint
//       definition (the #2301 `ptd$<nm>` ODR-merge precedent).

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace {
namespace fs = std::filesystem;

const std::filesystem::path& wtDir3358() {
    return wtScratchDir("wt-3358");
}

struct TempFile3358 {
    std::string path;
    explicit TempFile3358(std::string p) : path(std::move(p)) {}
    ~TempFile3358() { std::remove(path.c_str()); }
};

void writeSource3358(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile lib → object, compile main importing lib → executable, run.
// Returns the exit code, or -1 when either compile or the link fails (a link
// failure is a meaningful outcome here: the consumer-side fix adds an
// external reference to the dependency's `bvt$spanbox$<module>.<head>`
// for all four heads; a link failure on a defined head is a real defect,
// but a head the build never mints would have been the silent-misfire
// this fix addresses — the pre-mint loop on the exporter side prevents
// that case by ensuring every dependency emits all four heads).
int runXmod3358(const std::string& libSrc, const std::string& mainSrc) {
    auto dir = wtDir3358();
    std::string stem = unique_stem("bezel_xmod3358");
    TempFile3358 lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile3358 lObj((dir / (stem + "_lib.o")).string());
    TempFile3358 mSrc((dir / (stem + "_main.bzl")).string());
    TempFile3358 mExe((dir / (stem + "_main")).string());

    writeSource3358(lSrc.path, libSrc);
    writeSource3358(mSrc.path, mainSrc);

    bezel::Driver driver;
    if (!driver.compileObject(lSrc.path, lObj.path).success) return -1;

    bezel::Dependency dep;
    dep.metadataPath = lObj.path;
    dep.objectPath   = lObj.path;
    std::vector<bezel::Dependency> deps{dep};

    if (!driver.compileFile(mSrc.path, mExe.path, deps).success) return -1;

    int status = std::system(mExe.path.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

// Single-module control: the SAME declarations and the SAME body in one file.
int runLocal3358(const std::string& src) {
    auto dir = wtDir3358();
    std::string stem = unique_stem("bezel_loc3358");
    TempFile3358 mSrc((dir / (stem + ".bzl")).string());
    TempFile3358 mExe((dir / stem).string());
    writeSource3358(mSrc.path, src);

    bezel::Driver driver;
    if (!driver.compileFile(mSrc.path, mExe.path, {}).success) return -1;
    int status = std::system(mExe.path.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

// The dependency: OwningBuffer<int>'s `AsSpan()` returns a `Span<int>`
// (a record, §4.14), which is then coerced to `IReadOnlySpan<int>` (the
// interface the record implements, §4.16) and then boxed to `Object`.
// The interface intermediate is what REACHES `ensureSpanBoxVtable` on the
// dependency side under current codegen — the direct `Object := sp` path
// exercises a different (unrelated) IR defect.
const char* kLib3358 =
    "module Lib; "
    "public class Maker "
    "  public static method WrapSpan(): Object "
    "    using var buf := new OwningBuffer<int>(4); "
    "    var sp: Span<int> := buf.AsSpan(); "
    "    var irs: IReadOnlySpan<int> := sp; "
    "    var o: Object := irs; "
    "    return o; "
    "  end WrapSpan; "
    "end Maker; ";

std::string app3358(const std::string& body) {
    return "module App; import Lib; class C "
           "  public static method Main(): int " + body +
           "  end Main; end C; ";
}
}  // namespace

// #3358 / §2.8.3 rule 4 — THE REPRO.  The dependency boxes a `Span<int>`
// (via the IReadOnlySpan interface intermediate).  The consumer's
// discriminator, pre-fix, claimed the value was a `string` (the
// whitelist NEGATION fell through because `bvt$spanbox$Lib.Span` was
// Internal in the exporter and absent from the consumer's table), so
// `is string` was TRUE and the program returned 10.  After the fix:
// the exporter's ExternalLinkage mint resolves to the dependency's
// ExternalLinkage def at link time, the consumer's `bz$vtable$table`
// contains the canonical address, and the discriminator answers
// `is string` FALSE (20).
BEZEL_TEST(xmod_imported_span_box_vtable_3358, is_string_on_dependency_boxed_span) {
    BEZEL_EXPECT_EQ(runXmod3358(kLib3358, app3358(
        "    var o: Object := Maker.WrapSpan(); "
        "    if o is string then return 10; end if; "
        "    return 20; ")), 20);
}

// #3358 / §2.8.3 rule 4 — the cross-artifact surface of the boxed value.
// Pre-fix: the discriminator returned TRUE for `is string` and FALSE for
// `is int` (a string is not an int), so the program returned 100 (the
// `is string` arm fired).  Post-fix: `is string` is FALSE and `is int`
// is also FALSE (a Span is neither), so 0 (no arm fired).  This is the
// over-correction guard: the discriminator must NOT falsely match either
// arm.
BEZEL_TEST(xmod_imported_span_box_vtable_3358, whole_object_surface_on_dependency_boxed_span) {
    BEZEL_EXPECT_EQ(runXmod3358(kLib3358, app3358(
        "    var score: int := 0; "
        "    var o: Object := Maker.WrapSpan(); "
        "    if o is string then score := score + 100; end if; "
        "    if o is int then score := score + 100; end if; "
        "    return score; ")), 0);
}

// #3358 — CONTROL, single module.  The identical declarations and body
// in ONE file already worked and must be unperturbed: this is the
// baseline §2.5 / §2.8.3 rule 4 require the cross-module spelling to
// match.  Pre-fix AND post-fix: 20.
BEZEL_TEST(xmod_imported_span_box_vtable_3358, local_single_module_control) {
    BEZEL_EXPECT_EQ(runLocal3358(
        "module App; "
        "class C "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<int>(4); "
        "    var sp: Span<int> := buf.AsSpan(); "
        "    var irs: IReadOnlySpan<int> := sp; "
        "    var o: Object := irs; "
        "    if o is string then return 10; end if; "
        "    return 20; "
        "  end Main; "
        "end C; "), 20);
}

// #3358 — LINK SAFETY.  The consumer now declares all four span heads
// (`Span` / `ReadOnlySpan` / `ArraySpan` / `ReadOnlyArraySpan`) with
// ExternalLinkage in its `bz$vtable$table` for every imported module.
// The exporter's pre-mint loop ensures every dependency emits all four
// heads (the canonical address per head per dependency).  This test
// confirms the link line still succeeds end-to-end when the dependency
// exports a Span box — exit code 33 is the sentinel for "linked and ran".
BEZEL_TEST(xmod_imported_span_box_vtable_3358, dependency_with_span_box_links_and_runs) {
    BEZEL_EXPECT_EQ(runXmod3358(kLib3358, app3358(
        "    var o: Object := Maker.WrapSpan(); "
        "    return 33; ")), 33);
}