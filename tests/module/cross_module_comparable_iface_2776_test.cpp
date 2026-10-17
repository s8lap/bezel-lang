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
//
// ── HARNESS (test #6386) ───────────────────────────────────────────────────
//
// Every row here builds a TWO-module program (a `compileObject`'d library plus
// an importer), which used to mean this file carried its own private runner —
// `compileAndRunCrossModule`, four hand-rolled steps with its own
// `TempFile`/`writeSource`/exit-code decoding.  It is now
// `bezel::test::runAotModules` (tests/support/aot_support.hpp, added by
// #6379), which is the SAME run half `runAot` uses (`aot_detail::runProgramFill`)
// and the same forked compile half (`forkedCompileModule` /
// `forkedCompileWithDeps`) `runAot`'s `cachedCompile` is built on.
//
// THE PORTED ORACLE IS STRICTLY STRONGER, and the difference is the point:
//
//   * the old runner returned -1 for BOTH a compile failure and a program that
//     died from a signal, and the rows asserted only the exit code. A crash was
//     therefore a *different wrong number*, never a crash report.
//   * `runAotModules` returns the driver's own captured stderr in
//     `r.diagnostics` and `ran == false` for a reject, a compiler crash, a
//     runtime signal, a timeout and an ASan report — each carrying the literal
//     that says WHICH. Each row asserts `r.ran` FIRST and unconditionally, so a
//     row that cannot pass by accident is the requirement, not the outcome.
//   * the compile half is now FORKED, so a driver crash in the library build is
//     isolated and reported rather than taking the test process with it.
//
// ⚠ The old runner used `compileFileForTest`, which honours the #1822
// whole-suite ASan sweep; `runAotModules` honours the same sweep through the
// same predicate (`aot_detail::forceSanitizeAddress`), so the sweep still
// reaches every row here. Under the sweep the run ENVIRONMENT differs by one
// setting, and deliberately: the old `std::system(exe)` inherited the ambient
// environment, which turns LeakSanitizer ON by default, whereas `runAot`
// pins `detect_leaks=0` under the sweep because whole-suite leak detection is
// too noisy and because §28.2 checked disposal leaks BY DESIGN (#4532 measured
// the cost). MEASURED under `BEZEL_AOT_FORCE_SANITIZE=address`: every row below
// is clean, so no row here was relying on that inherited leak detection.

#include <string>
#include <vector>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::runAotModules;

namespace {

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

// The one entry point every row used to route through. `Lib` is always the
// dependency and `App` always the entry; `runAotModules` derives the build
// order from the two modules' own `import` declarations, so the pair is
// supplied in one brace-init and the harness does the rest.
JitResult compileAndRunCrossModule(const std::string& libSrc,
                                   const std::string& mainSrc) {
    return runAotModules({ModuleSource{"Lib", libSrc},
                          ModuleSource{"App", mainSrc}},
                         "App");
}

}  // namespace

// #2776 / §14.2 rule 3 + §2.5 — `Min()` over an imported self-comparable
// interface element.  Ranks 7 and 3: Min is 3.
//
// ⚠ ORDER MATTERS AND DID NOT CHANGE. Each old row was a single
// `BEZEL_EXPECT_EQ(<exit code>, N)`. Each is now `ran` first, then that same
// exit code: the value assertion is byte-for-byte the one it was and `ran` is
// added on top. A row that passes now cannot have passed for a compile
// failure, a signal, a timeout or an ASan report — each of which the old -1
// could not distinguish from "the program returned some other number".

// `Min()` — the sibling arm, so a fix that reached only one is caught.
BEZEL_TEST(cross_module_comparable_iface_2776, min_over_imported_interface_element) {
    JitResult r = compileAndRunCrossModule(kLib, app(
        "    using var a := new RNode(7); "
        "    using var b := new RNode(3); "
        "    var arr: Array<IRank> := [a, b]; "
        "    return arr.Min().Rank(); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// `Max()` — the sibling arm, so a fix that reached only one is caught.
BEZEL_TEST(cross_module_comparable_iface_2776, max_over_imported_interface_element) {
    JitResult r = compileAndRunCrossModule(kLib, app(
        "    using var a := new RNode(7); "
        "    using var b := new RNode(3); "
        "    var arr: Array<IRank> := [a, b]; "
        "    return arr.Max().Rank(); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// `Sort()` — a THIRD gate, and the one that reaches the itable comparator thunk.
// 3,1,2 sorted ascending is 1,2,3, encoded base-4 (1*16 + 2*4 + 3 = 27) so the
// 8-bit WEXITSTATUS mask is not involved.
BEZEL_TEST(cross_module_comparable_iface_2776, sort_over_imported_interface_element) {
    JitResult r = compileAndRunCrossModule(kLib, app(
        "    using var a := new RNode(3); "
        "    using var b := new RNode(1); "
        "    using var c := new RNode(2); "
        "    var arr: Array<IRank> := [a, b, c]; "
        "    arr.Sort(); "
        "    return arr[0].Rank() * 16 + arr[1].Rank() * 4 + arr[2].Rank(); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 27);
}

// `Math.Min` — the §2.2 gate, reached through the same shared predicate.  This
// is what makes the case for #2787's factoring concrete: one loader change fixed
// all four call sites at once.
BEZEL_TEST(cross_module_comparable_iface_2776, math_min_over_imported_interface) {
    JitResult r = compileAndRunCrossModule(kLib, app(
        "    using var a := new RNode(7); "
        "    using var b := new RNode(3); "
        "    var x: IRank := a; "
        "    var y: IRank := b; "
        "    return Math.Min(x, y).Rank() * 4 + Math.Max(x, y).Rank(); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 19);
}

// LINKAGE — two modules both naming the synthesized `IComparable` must LINK.
// This is the case that caught the second half of the fix: before it, the
// consumer's own `td$IComparable` collided with the dependency's ("multiple
// definition of `td$IComparable'"), and a link failure surfaces here as -1.
//
// ⚠ THIS ROW IS WHERE `ran` BUYS SOMETHING SPECIFIC. Under the old runner a
// link failure and a wrong value were both "some exit code"; the row survived
// only because -1 differs from 5. On `runAotModules` the same link failure is
// `ran == false` with clang's "multiple definition" line in `r.diagnostics`, so
// a future re-break of §2.8.3 rule 5 reports WHAT broke rather than only that
// a number was wrong.
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
    JitResult r = compileAndRunCrossModule(kLib, app(
        "    using var a := new RNode(7); "
        "    var x: IRank := a; "
        "    return x.Rank() - 2; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// CONTROL — a LOCAL self-comparable interface is unchanged.  The loader change
// must not perturb the same-module path that already worked.
BEZEL_TEST(cross_module_comparable_iface_2776, control_local_interface_unchanged) {
    JitResult r = compileAndRunCrossModule(kLib, app(
        "    var arr: Array<int> := [3, 1, 2]; "
        "    arr.Sort(); "
        "    return arr[0] * 16 + arr[1] * 4 + arr[2]; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 27);
}