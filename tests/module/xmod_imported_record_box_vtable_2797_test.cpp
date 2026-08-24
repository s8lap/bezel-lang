// Regression tests for #2797 — a value the DEPENDENCY boxed was misreported as
// a `string` by the runtime discriminator (`bz_obj_is_string`'s whitelist
// NEGATION fell through), so every `is`/`case`/`ToString`/`Equals` site over
// it answered wrongly (or, for `Equals`/`GetHashCode` reached through
// `Object`, SIGSEGV'd).  This is #2792's "remaining half": #2792 fixed the
// CLASS half by declaring the exporter's `vt$<module>.<Type>` in the consumer
// and pushing it into `bz$vtable$table`; a BOX vtable has no such escape hatch
// — the exporter mints it with INTERNAL linkage (`d` in `nm`), the consumer
// cannot reference it, the consumer's `bz$vtable$table` cannot list it, and
// the discriminator's "not a vtable ⇒ a string" premise mis-fires.
//
// WHAT THE DOCUMENTS REQUIRE.  spec §2.8.3 rule 4 (v0.240): a type has "one
// runtime type identity — `is` tests, `catch` clauses, and §19 patterns over
// values of the instantiation match identically regardless of which artifact
// produced the value or performs the test".  A value the dependency boxed is
// exactly such a cross-artifact value.
//
// THE FIX has two sides.  On the EXPORTER, `bvt$box$<R>` is now emitted with
// ExternalLinkage and a module-QUALIFIED name (`bvt$box$<module>.<Type>`,
// mirroring #1488's class-vtable mangling) for every public, non-generic
// record/enum.  On the CONSUMER, this test's exact path: every imported
// public, non-generic record/enum is iterated; the same module-qualified
// name is declared (ExternalLinkage, no initializer — the address is the only
// thing `bz_obj_is_string` reads) and pushed into `allVtables_`.  Same gate as
// #2792's class-vtable fix.  Non-public and generic types keep Internal
// linkage in the exporter — referencing them would be an undefined symbol at
// link time — so they are skipped here.
//
// THE REPRO is the issue's own — `Maker.BoxPt` boxes a `Pt` and hands back an
// `Object`; the consumer's `is Pt` was FALSE (it was taken for a string),
// `+ 100` was never added, and the program exited 20 instead of 10.  After
// the fix: 10.
//
// ⚠ WHAT THIS DOES NOT COVER, measured on both sides of the change and left
// to its own issue: the same linkage property holds for SPAN box vtables
// (`ensureSpanBoxVtable`) and per-monomorphization specialized vtables
// (`vt$C$specN`); only the record/enum case is reproduced mechanically here
// (the issue's own discipline: "should be probed before being claimed").

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

const std::filesystem::path& wtDir2797() {
    return wtScratchDir("wt-2797");
}

struct TempFile2797 {
    std::string path;
    explicit TempFile2797(std::string p) : path(std::move(p)) {}
    ~TempFile2797() { std::remove(path.c_str()); }
};

void writeSource2797(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile lib → object, compile main importing lib → executable, run.
// Returns the exit code, or -1 when either compile or the link fails (a link
// failure is a meaningful outcome here: the consumer-side fix adds an external
// reference to the dependency's `bvt$box$<module>.<Type>`, so a case that
// fails to link must be visible).
int runXmod2797(const std::string& libSrc, const std::string& mainSrc) {
    auto dir = wtDir2797();
    std::string stem = unique_stem("bezel_xmod2797");
    TempFile2797 lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile2797 lObj((dir / (stem + "_lib.o")).string());
    TempFile2797 mSrc((dir / (stem + "_main.bzl")).string());
    TempFile2797 mExe((dir / (stem + "_main")).string());

    writeSource2797(lSrc.path, libSrc);
    writeSource2797(mSrc.path, mainSrc);

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
int runLocal2797(const std::string& src) {
    auto dir = wtDir2797();
    std::string stem = unique_stem("bezel_loc2797");
    TempFile2797 mSrc((dir / (stem + ".bzl")).string());
    TempFile2797 mExe((dir / stem).string());
    writeSource2797(mSrc.path, src);

    bezel::Driver driver;
    if (!driver.compileFile(mSrc.path, mExe.path, {}).success) return -1;
    int status = std::system(mExe.path.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

// The dependency: the issue's exact `Pt`/`Maker` pair (so the value is
// produced and BOXED in the dependency), plus an enum and a body record that
// implement an interface — every box-vtable shape the consumer-side fix has
// to declare.
const char* kLib2797 =
    "module Lib; "
    "public record Pt(X: int, Y: int); "
    "public record Rect "
    "  public readonly property W: int; "
    "  public readonly property H: int; "
    "  public constructor(w: int, h: int) "
    "    W := w; "
    "    H := h; "
    "  end constructor; "
    "end Rect; "
    "public enum Color Red, Green, Blue; end Color; "
    "public class Maker "
    "  public static method BoxPt(x: int, y: int): Object => new Pt(x, y); "
    "  public static method BoxRect(w: int, h: int): Object => new Rect(w, h); "
    "  public static method BoxColor(c: Color): Object => c; "
    "end Maker; ";

std::string app2797(const std::string& body) {
    return "module App; import Lib; class C "
           "  public static method Main(): int " + body +
           "  end Main; end C; ";
}
}  // namespace

// #2797 / §2.8.3 rule 4 — THE ISSUE'S OWN REPRO, verbatim.  Before the fix:
// `is Pt` was FALSE on a value the dependency boxed (its `bvt$box$Pt` is `d`
// in `lib.o`, not `D`, so the consumer cannot reference it, and the
// discriminator's whitelist contains no entry for it ⇒ the value was taken
// for a string), so the program returned 20.  After the fix: 10.
BEZEL_TEST(xmod_imported_record_box_vtable_2797, is_pt_on_dependency_boxed_value) {
    BEZEL_EXPECT_EQ(runXmod2797(kLib2797, app2797(
        "    var o: Object := Maker.BoxPt(3, 4); "
        "    if o is Pt then return 10; end if; "
        "    return 20; ")), 10);
}

// #2797 / §12.7 + §8.5 + §8.3 — the whole `Object` surface over a value the
// dependency boxed.  1|2|4 = 7 (only `Equals`/`GetHashCode` would be 8|16 and
// risk SIGSEGV pre-fix; the smaller digit set is intentional — keep the test
// in 0..7 so 8-bit exit truncation cannot mis-encode the failure).  Before
// the fix: the discriminator claimed each value was a string; `is Pt` was
// FALSE (0), `as Pt` was NULL, `ToString` returned "" (length 0), the whole
// thing exited 0.
BEZEL_TEST(xmod_imported_record_box_vtable_2797, is_as_tostring_on_dependency_boxed_value) {
    BEZEL_EXPECT_EQ(runXmod2797(kLib2797, app2797(
        "    var score: int := 0; "
        "    var o: Object := Maker.BoxPt(3, 4); "
        "    if o is Pt then score := score + 1; end if; "
        "    var p: Pt? := (o as Pt); "
        "    if p <> null and p!.X = 3 and p!.Y = 4 then score := score + 2; end if; "
        "    var s: string := o.ToString(); "
        "    if s.Length > 0 then score := score + 4; end if; "
        "    return score; ")), 7);
}

// #2797 / §8.3 — `Object.Equals` / `GetHashCode` on a value the dependency
// boxed.  This one did NOT merely answer wrongly pre-fix: the
// object-equality and identity-hash lowerings dereference a receiver the
// discriminator claimed was a string (`bz.string`'s data pointer is the
// object's vtable word), so it SIGSEGV'd.  A -1 here is the
// compile/link/abnormal-exit sentinel, so the pre-fix crash is observed as
// a failed expectation rather than a value.
BEZEL_TEST(xmod_imported_record_box_vtable_2797, object_equals_and_hash_on_dependency_boxed_value) {
    BEZEL_EXPECT_EQ(runXmod2797(kLib2797, app2797(
        "    var o: Object := Maker.BoxPt(3, 4); "
        "    var o2: Object := Maker.BoxPt(3, 4); "
        "    var score: int := 0; "
        "    if o.Equals(o2) then score := score + 1; end if; "
        "    if o.GetHashCode() = o2.GetHashCode() then score := score + 2; end if; "
        "    return score; ")), 3);
}

// #2797 / §19.3 — `case` TYPE PATTERN over a value the dependency boxed.
// §2.8.3 rule 4 names §19 patterns alongside `is`.  Before the fix: took
// the `_` arm and returned 99.
BEZEL_TEST(xmod_imported_record_box_vtable_2797, case_pattern_on_dependency_boxed_value) {
    BEZEL_EXPECT_EQ(runXmod2797(kLib2797, app2797(
        "    var o: Object := Maker.BoxPt(3, 4); "
        "    return (case o "
        "      of var p: Pt => 10 + 1, "
        "         _ => 99); ")), 11);
}

// #2797 / §10 — the enum case.  An enum boxed by the dependency must also be
// recognized as that enum by the consumer (the consumer's table must contain
// the enum's box vtable, not just the record's).  Before the fix: 20.
BEZEL_TEST(xmod_imported_record_box_vtable_2797, is_color_on_dependency_boxed_value) {
    BEZEL_EXPECT_EQ(runXmod2797(kLib2797, app2797(
        "    var o: Object := Maker.BoxColor(Color.Green); "
        "    if o is Color then return 10; end if; "
        "    return 20; ")), 10);
}

// #2797 / §9.2 — the body-record case (constructor + properties).  Same
// `bvt$box$` machinery, same gate; the test exists to prove the fix is not
// specific to positional records.  Before the fix: 20.
BEZEL_TEST(xmod_imported_record_box_vtable_2797, is_rect_on_dependency_boxed_value) {
    BEZEL_EXPECT_EQ(runXmod2797(kLib2797, app2797(
        "    var o: Object := Maker.BoxRect(2, 5); "
        "    if o is Rect then return 10; end if; "
        "    return 20; ")), 10);
}

// #2797 — CONTROL, the OVER-CORRECTION axis.  A real string still has to be
// recognized as one, the `o is Pt` axis must stay on, and a string must
// never match the imported record (the hundreds digit).  1|2 = 3.  Before
// the fix: 2 (the `is string` axis already worked; `is Pt` did not).
BEZEL_TEST(xmod_imported_record_box_vtable_2797, string_and_dependency_box_stay_distinct) {
    BEZEL_EXPECT_EQ(runXmod2797(kLib2797, app2797(
        "    var score: int := 0; "
        "    var o: Object := \"hello\"; "
        "    if o is string then score := score + 1; end if; "
        "    var o2: Object := Maker.BoxPt(3, 4); "
        "    if o2 is Pt then score := score + 2; end if; "
        "    if o is Pt then score := score + 100; end if; "
        "    if o2 is string then score := score + 100; end if; "
        "    return score; ")), 3);
}

// #2797 / §2.8.3 rule 3 — CROSS-MODULE TYPE IDENTITY.  A second module
// declaring its own `Pt` (a different type under §2.8.3 rule 3, the rule
// the consumer-side fix's module-qualified name is built to defend) must
// NOT match the dependency's `Pt`.  Digit 1 (matches the imported Lib.Pt)
// must fire, and the hundreds digit (would match the local App.Pt) must
// stay 0.  Before the fix: the dependency's boxed value was a "string",
// so neither `is` matched the typed arms and the local App.Pt's
// descriptor was not the chain entry to find it — `is Pt` (unqualified)
// was FALSE, the score was 0.
BEZEL_TEST(xmod_imported_record_box_vtable_2797, local_and_imported_same_name_stay_distinct) {
    BEZEL_EXPECT_EQ(runXmod2797(kLib2797,
        "module App; import Lib; "
        "public record PtLocal(A: int); "
        "class C "
        "  public static method Main(): int "
        "    var score: int := 0; "
        "    var o: Object := Maker.BoxPt(3, 4); "
        "    if o is Pt then score := score + 1; end if; "
        "    if o is PtLocal then score := score + 100; end if; "
        "    return score; "
        "  end Main; "
        "end C; "), 1);
}

// #2797 — LINK SAFETY.  The consumer now walks every imported
// public, non-generic record/enum and emits an external reference to its
// `bvt$box$<module>.<Type>` (the exporter defines those symbols; a
// non-public or generic record's box vtable stays InternalLinkage there,
// and referencing it here would be an undefined symbol at link time).  The
// link must succeed — a -1 is the failure sentinel.
BEZEL_TEST(xmod_imported_record_box_vtable_2797, nonpublic_and_generic_records_still_link) {
    const char* lib =
        "module Lib; "
        "record Hidden(A: int); "
        "public record Holder<T>(V: T); "
        "public record Plain(X: int, Y: int); "
        "public class Maker "
        "  public static method MakePlain(x: int, y: int): Object => new Plain(x, y); "
        "end Maker; ";
    BEZEL_EXPECT_EQ(runXmod2797(lib,
        "module App; import Lib; class C "
        "  public static method Main(): int "
        "    var o: Object := Maker.MakePlain(3, 4); "
        "    if o is Plain then return 10; end if; "
        "    return 20; "
        "  end Main; "
        "end C; "), 10);
}

// #2797 — LINK SAFETY, the imported-but-unused dependency.  Every public
// record/enum of a presented dependency is now referenced by the consumer's
// vtable table even when the program touches none of them; the dependency's
// object is on the link line, so this must still link and run.
BEZEL_TEST(xmod_imported_record_box_vtable_2797, unused_dependency_still_links) {
    BEZEL_EXPECT_EQ(runXmod2797(kLib2797,
        "module App; import Lib; "
        "class Loc public constructor() end constructor; end Loc; "
        "class C "
        "  public static method Main(): int "
        "    using var l := new Loc(); "
        "    var o: Object := l; "
        "    if o is Loc then return 33; end if; "
        "    return 44; "
        "  end Main; "
        "end C; "), 33);
}

// #2797 — CONTROL, single module.  The identical declarations and body in
// ONE file already worked and must be unperturbed: this is the baseline
// §2.5 / §2.8.3 rule 4 require the cross-module spelling to match.
BEZEL_TEST(xmod_imported_record_box_vtable_2797, local_single_module_control) {
    BEZEL_EXPECT_EQ(runLocal2797(
        "module App; "
        "public record Pt(X: int, Y: int); "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := new Pt(3, 4); "
        "    if o is Pt then return 10; end if; "
        "    return 20; "
        "  end Main; "
        "end C; "), 10);
}