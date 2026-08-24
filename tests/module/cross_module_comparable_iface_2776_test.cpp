// Regression tests for #2776 — a CROSS-MODULE self-comparable interface was
// silently declined by every gate that keys on §14.2 rule 3.
//
// WHAT THE DOCUMENTS REQUIRE.  spec §14.2 rule 3 admits an interface
// substitution for `where T: IComparable<T>`, and §2.5 makes an imported type
// indistinguishable from a local one for typing purposes.  So a `public
// interface IRank: IComparable<IRank>` declared in a dependency and used as
// `Array<IRank>` in the consumer must sort/compare exactly as the same
// declaration would locally.
//
// THE DEFECT was an IDENTITY MISMATCH, not a missing base.  The dependency's
// metadata does carry the base (`ModuleMetadataBuilder.cpp` emits
// `baseTypeRefs`), and the loader does reconstruct it — but `lookupNamed`
// searches the dependency's EXPORTED USER types, and `IComparable` is not one:
// it is compiler-synthesized.  So the reconstructed base came back with
// `declaration == nullptr`, and #1867's IDENTITY test (which exists because
// "blanket-accepting by decl/name was too lenient — it admitted a WRONG generic
// argument") could never match it.  MEASURED with an instrumented build:
//
//     [2776] base head=IComparable decl=(nil) icmp=0x28ac5870
//
// — the head is right, the identity is null.  Consequence: `Array<IRank>.Sort()`,
// `.Min()`, `.Max()` and `Math.Min` all declined a document-admissible element,
// the limit recorded in `selfComparableIfaceCompareTo`'s header ("⚠ MEASURED
// LIMIT: this does NOT reach a CROSS-MODULE interface").
//
// THE FIX resolves an imported occurrence of a synthesized builtin interface to
// the SAME `builtinInterfaceSym` a local occurrence gets, so the identity test
// matches.  Because #2787 had already collapsed the predicate to one definition,
// that single loader change reaches Sort, Min/Max and Math.Min at once — which
// is exactly what #2787 was filed to make possible.
//
// ⚠ IT ALSO REQUIRED A LINKAGE FIX, found by this very test failing to LINK.
// Giving the consumer the same identity makes it emit its own `td$IComparable`
// descriptor alongside the dependency's, and both were strong definitions:
// "multiple definition of `td$IComparable'".  A synthesized builtin's descriptor
// is compiler-provided rather than owned by any one artifact, so it is now
// `linkonce_odr` — which is what §2.8.3 rule 5 actually asks for ("exactly one
// static state and one runtime identity, regardless of link mode").

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace {
namespace fs = std::filesystem;

// Per-process unique working dir under the shared scratch base. The previous
// fs::temp_directory_path() (== /tmp tmpfs) + non-atomic counter pattern was
// race-prone: parallel shards overwrote each other's source/object files
// mid-compile, garbling the in-flight child read. The atomic unique_stem
// + /var/tmp/bezel-scratch parent fixes that class of flake (#3002-class).
const std::filesystem::path& wtDir2776() {
    return wtScratchDir("wt-2776");
}

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile lib → object, compile main importing lib → executable, run.
// Returns the exit code, or -1 when either compile or the link fails — which is
// itself meaningful here: the linkage half of this fix shows up as a -1.
int compileAndRunCrossModule(const std::string& libSrc,
                             const std::string& mainSrc) {
    auto dir = wtDir2776();
    std::string stem = unique_stem("bezel_xmod2776");
    TempFile lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile lObj((dir / (stem + "_lib.o")).string());
    TempFile mSrc((dir / (stem + "_main.bzl")).string());
    TempFile mExe((dir / (stem + "_main")).string());

    writeSource(lSrc.path, libSrc);
    writeSource(mSrc.path, mainSrc);

    bezel::Driver driver;
    bezel::CompileResult rl = driver.compileObject(lSrc.path, lObj.path);
    if (!rl.success) return -1;

    bezel::Dependency dep;
    dep.metadataPath = lObj.path;
    dep.objectPath   = lObj.path;
    std::vector<bezel::Dependency> deps{dep};

    bezel::CompileResult rm = bezel::test::compileFileForTest(driver, mSrc.path, mExe.path, deps);
    if (!rm.success) return -1;

    int status = std::system(mExe.path.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

// The dependency: a self-comparable interface and one implementor, ascending.
const char* kLib =
    "module Lib; "
    "public interface IRank: IComparable<IRank> "
    "  method Rank(): int; "
    "end IRank; "
    "public class RNode: IRank "
    "  private _r: int; "
    "  public constructor(r: int) _r := r; end constructor; "
    "  public method Rank(): int => _r; "
    "  public method CompareTo(other: IRank): int => _r - other.Rank(); "
    "end RNode; ";

std::string app(const std::string& body) {
    return "module App; import Lib; class C "
           "  public static method Main(): int " + body +
           "  end Main; end C; ";
}
}  // namespace

// #2776 / §14.2 rule 3 + §2.5 — `Min()` over an imported self-comparable
// interface element.  Ranks 7 and 3: Min is 3.
BEZEL_TEST(cross_module_comparable_iface_2776, min_over_imported_interface_element) {
    BEZEL_EXPECT_EQ(compileAndRunCrossModule(kLib, app(
        "    using var a := new RNode(7); "
        "    using var b := new RNode(3); "
        "    var arr: Array<IRank> := [a, b]; "
        "    return arr.Min().Rank(); ")), 3);
}

// `Max()` — the sibling arm, so a fix that reached only one is caught.
BEZEL_TEST(cross_module_comparable_iface_2776, max_over_imported_interface_element) {
    BEZEL_EXPECT_EQ(compileAndRunCrossModule(kLib, app(
        "    using var a := new RNode(7); "
        "    using var b := new RNode(3); "
        "    var arr: Array<IRank> := [a, b]; "
        "    return arr.Max().Rank(); ")), 7);
}

// `Sort()` — a THIRD gate, and the one that reaches the itable comparator thunk.
// 3,1,2 sorted ascending is 1,2,3, encoded base-4 (1*16 + 2*4 + 3 = 27) so the
// 8-bit WEXITSTATUS mask is not involved.
BEZEL_TEST(cross_module_comparable_iface_2776, sort_over_imported_interface_element) {
    BEZEL_EXPECT_EQ(compileAndRunCrossModule(kLib, app(
        "    using var a := new RNode(3); "
        "    using var b := new RNode(1); "
        "    using var c := new RNode(2); "
        "    var arr: Array<IRank> := [a, b, c]; "
        "    arr.Sort(); "
        "    return arr[0].Rank() * 16 + arr[1].Rank() * 4 + arr[2].Rank(); ")),
        27);
}

// `Math.Min` — the §2.2 gate, reached through the same shared predicate.  This
// is what makes the case for #2787's factoring concrete: one loader change fixed
// all four call sites at once.
BEZEL_TEST(cross_module_comparable_iface_2776, math_min_over_imported_interface) {
    BEZEL_EXPECT_EQ(compileAndRunCrossModule(kLib, app(
        "    using var a := new RNode(7); "
        "    using var b := new RNode(3); "
        "    var x: IRank := a; "
        "    var y: IRank := b; "
        "    return Math.Min(x, y).Rank() * 4 + Math.Max(x, y).Rank(); ")),
        19);
}

// LINKAGE — two modules both naming the synthesized `IComparable` must LINK.
// This is the case that caught the second half of the fix: before it, the
// consumer's own `td$IComparable` collided with the dependency's ("multiple
// definition of `td$IComparable'"), and a link failure surfaces here as -1.
//
// The body deliberately does NOT call `CompareTo` explicitly.  That path was
// separately broken when this test was written — a written-out `CompareTo`
// through an imported interface silently returned 0 while `Rank()` on the same
// value returned correctly — which is **#2792**, measured to be pre-existing on
// both sides of this fix.  #2792 is now FIXED (the consumer's `bz$vtable$table`
// omitted every imported class's vtable, so `bz_obj_is_string` took the receiver
// for a string); it is pinned by `xmod_imported_class_vtable_2792`, and this
// case is left as-is so the linkage half stays isolated from it.
BEZEL_TEST(cross_module_comparable_iface_2776, both_modules_naming_icomparable_link) {
    BEZEL_EXPECT_EQ(compileAndRunCrossModule(kLib, app(
        "    using var a := new RNode(7); "
        "    var x: IRank := a; "
        "    return x.Rank() - 2; ")), 5);
}

// CONTROL — a LOCAL self-comparable interface is unchanged.  The loader change
// must not perturb the same-module path that already worked.
BEZEL_TEST(cross_module_comparable_iface_2776, control_local_interface_unchanged) {
    BEZEL_EXPECT_EQ(compileAndRunCrossModule(kLib, app(
        "    var arr: Array<int> := [3, 1, 2]; "
        "    arr.Sort(); "
        "    return arr[0] * 16 + arr[1] * 4 + arr[2]; ")), 27);
}
