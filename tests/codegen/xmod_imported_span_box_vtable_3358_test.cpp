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
//
// WHAT THE SPEC REQUIRES NOW (v0.329+).  The four original rows used the
// span→IReadOnlySpan→Object conversion chain and tested the boxed value with
// `is string`.  spec §8.6 and §9.7 (proposal 0146, v0.329/#5645/#5659)
// forbid that chain: a value that holds borrows may not reach a borrow-erasing
// position, and a position declared `Object` is borrow-erasing.  The
// conversion `irs: IReadOnlySpan<int>` → `o: Object` is a compile error at
// the conversion site, §9.7's borrow-erasing bullet.  The four rows that
// relied on that conversion are RETIRED.  The two acceptance rows below
// replace them with the only sanctioned cross-module shape: the dependency
// returns `IReadOnlySpan<int>` (the one reference type a borrow-bearing
// record may convert to, §8.6 v0.329+), and the app calls a member on the
// boxed receiver — the second admitted position — which exercises the same
// vtable dispatch machinery as the retired rows' discriminator path.
//
// COVERAGE DELTA: the retired rows pinned the cross-module `is`-type
// discriminator (the consumer's `bz$vtable$table` entry resolving for a
// boxed span, which determines the outcome of `o is string`).  The new rows
// pin a member-call vtable dispatch on the boxed receiver (§8.6 second
// admitted position).  The #3358 fix has two distinct parts: (a) the
// ExternalLinkage on `bvt$spanbox$<module>.<head>` and LinkOnceODR on
// `btd$spanbox` correct the vtable entries for cross-module boxed spans,
// which both the discriminator and member-call paths rely on; (b) the
// pre-mint loop ensures the consumer's declaration of all four heads is
// satisfied at link time.  The new rows verify (a) through member dispatch
// rather than the discriminator.  The rejected `Object` conversion is
// already pinned by `sema_borrow_erasing_reject_5659_test.cpp`.
//
// The four retired assertions are each impossible under v0.329+: no program
// can compile with `irs: IReadOnlySpan<int>` → `o: Object` to test the
// discriminator outcome, so the assertions themselves are vacuous under the
// current spec — the compiler rejects before any runtime dispatch can occur.

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

// The dependency (v0.329+ shape): `Maker` holds the `owned OwningBuffer<int>`
// as an instance field, so the span's borrow is receiver-sourced (§9.7
// receiver-sourced return bullet; a span returned from an instance method borrows
// from `this._buf`, whose lifetime is the caller's scope).  The method returns
// `IReadOnlySpan<int>` — the one reference type a borrow-bearing record may
// convert to at a return position (§8.6, v0.329+).  The app then calls a
// member on the boxed receiver (the second admitted position, §8.6), which
// dispatches through the vtable.
const char* kLib3358 =
    "module Lib; "
    "public class Maker "
    "  private owned _buf: OwningBuffer<int>; "
    "  public constructor() _buf := new OwningBuffer<int>(4); end constructor; "
    "  public method WrapSpan(): IReadOnlySpan<int> "
    "    var sp: Span<int> := _buf.AsSpan(); "
    "    return sp; "
    "  end WrapSpan; "
    "end Maker; ";

std::string app3358(const std::string& body) {
    return "module App; import Lib; class C "
           "  public static method Main(): int " + body +
           "  end Main; end C; ";
}
}  // namespace

// #3358 / §2.8.3 rule 4 — v0.329+ shape.  The dependency returns the boxed
// span as `IReadOnlySpan<int>` (the one sanctioned reference type, §8.6).
// The app calls `.Length` on the boxed receiver (the second admitted
// position, §8.6).  The call dispatches through the vtable laid down by
// `ensureSpanBoxVtable`; the pre-mint loop ensures the consumer's table
// entry for `bvt$spanbox$Lib.Span` resolves.  Exit 1 is the sentinel for
// "linked and ran without error" (0 would mean a runtime crash or bad
// return value).
//
// This row retired: the old `is string` shape (`o: Object := irs`) is
// rejected at the conversion site by §9.7 (v0.329+ / proposal 0146).
// RETIRED rows: is_string_on_dependency_boxed_span,
//   whole_object_surface_on_dependency_boxed_span,
//   local_single_module_control, dependency_with_span_box_links_and_runs.
BEZEL_TEST(xmod_imported_span_box_vtable_3358, member_call_length_on_dependency_boxed_span) {
    BEZEL_EXPECT_EQ(runXmod3358(kLib3358, app3358(
        "    var maker := new Maker(); "
        "    var n: int := maker.WrapSpan().Length; "
        "    return 1; ")), 1);
}

// #3358 — same cross-module member-call path, verifying `.ToString()`
// (the sanctioned rendering path for a span at a borrow-erasing position,
// §8.6 / §9.7, v0.329+).  The call dispatches through the vtable.
// Exit 1 means linked, ran, and returned the expected sentinel.
BEZEL_TEST(xmod_imported_span_box_vtable_3358, member_call_to_string_on_dependency_boxed_span) {
    BEZEL_EXPECT_EQ(runXmod3358(kLib3358, app3358(
        "    var maker := new Maker(); "
        "    var s: String := maker.WrapSpan().ToString(); "
        "    return 1; ")), 1);
}