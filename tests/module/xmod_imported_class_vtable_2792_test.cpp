// Regression tests for #2792 — every instance of an IMPORTED class was
// misreported as a `string` by the runtime discriminator, so a whole family of
// cross-module operations produced a wrong value (and one of them crashed).
//
// WHAT THE DOCUMENTS REQUIRE.  spec §2.8.3 rule 4 (v0.240) gives a type "one
// runtime type identity", under which `is` tests, `catch` clauses and §19
// patterns over its values "match identically regardless of which artifact
// produced the value or performs the test".  §2.5 makes an imported type's
// resolution artifact-based but otherwise ordinary, and §12.7 gives `is T` its
// meaning.  So an `Object` holding an instance of an imported class must answer
// `is` and §8.5 `ToString` exactly as the same declaration does in one module,
// and a `CompareTo`/`Equals` reached through an imported interface must call
// the implementor's method.
//
// THE DEFECT.  `bz_obj_is_string` (bezel_runtime_refcounting.cpp) is a whitelist
// NEGATION: it answers "this IS a string" for any object whose word 0 is not one
// of the vtables listed in `bz$vtable$table`.  That table is built by
// finalizeVtableTable() from `allVtables_`, which only ever received the vtable
// globals the CURRENT module emitted — and codegen deliberately emits no vtable
// global for an imported type (LlvmBackend_Setup.cpp's declareVtables comment:
// "No vtable GLOBAL is emitted for an imported type").  So an imported class's
// instance fell through the whitelist, was taken for a string, and each
// discriminator site then read it through the `bz.string` layout
// {ptr data, i64 len, i8 width, i64 rc}.
//
// MEASURED (two-module builds) — five silent wrong values and one crash:
//   * `x.CompareTo(y)` through an imported `IRank: IComparable<IRank>` returned
//     0 instead of 4 — the issue's own repro.  emitMethodCall's `strRecvPossible`
//     diamond (LlvmBackend_Calls2.cpp) took its BOTH-STRINGS arm and produced the
//     "ordinal length difference" of two RNode objects.
//   * `x.Equals(y)` through an imported `IIdent: IEquatable<IIdent>` returned
//     TRUE where the user's `Equals` answers FALSE — so this is NOT specific to
//     IComparable; it is general to the discriminator.
//   * `o is Plain` on an `Object` holding an imported instance was FALSE, which
//     §2.8.3 rule 4 forbids in as many words.
//   * a `case` type pattern (§19.3) over the same value took the `_` arm.
//   * `o.ToString()` on the same value produced "" instead of the §8.5 default
//     type name.
//   * `o.Equals(b)` / `o.GetHashCode()` on the same value SIGSEGV'd.
//
// THE FIX declares the exporter's `vt$<Type>` in the importer (same #1488
// module-qualified mangling, same public/non-generic export gate Pass D applies
// when it DEFINES that global, so the symbol is guaranteed external in the
// dependency object) and registers it in `allVtables_`.  Declaration only — the
// definition stays in the dependency artifact, which the build links in.
//
// ⚠ NOT FIXED HERE, and deliberately left to their own issues, measured on both
// sides of this change:
//   * an imported RECORD boxed into `Object` by the CONSUMER SIGSEGVs — no
//     boxing is emitted at all (#2796).
//   * a value the DEPENDENCY boxed is still misreported: its `bvt$box$<R>` has
//     INTERNAL linkage, so the consumer cannot name it in the table at all.
//     Closing that needs a linkage/ABI decision (#2797).
//   * an imported GENERIC class (`o is Bag<int>`) still answers FALSE.  Its
//     `vt$Bag$spec0` IS in the table (the importer monomorphizes it locally), so
//     that is a descriptor-identity defect, a different mechanism (#2798).

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

// Per-process unique working dir under the shared scratch base. The previous
// fs::temp_directory_path() (== /tmp tmpfs) + non-atomic counter pattern was
// race-prone: parallel shards overwrote each other's _lib.bzl / _lib.o /
// _main.bzl mid-compileObject and garbled the in-flight child read. The atomic
// unique_stem + /var/tmp/bezel-scratch parent fixes that class of flake
// (#3002-class COVERAGE SHORT).
const std::filesystem::path& wtDir2792() {
    return wtScratchDir("wt-2792");
}

struct TempFile2792 {
    std::string path;
    explicit TempFile2792(std::string p) : path(std::move(p)) {}
    ~TempFile2792() { std::remove(path.c_str()); }
};

void writeSource2792(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile lib → object, compile main importing lib → executable, run.
// Returns the exit code, or -1 when either compile or the link fails (a link
// failure is a meaningful outcome here: the fix adds an external reference to
// the dependency's `vt$<Type>`, so a case that fails to link must be visible).
int runXmod2792(const std::string& libSrc, const std::string& mainSrc) {
    auto dir = wtDir2792();
    std::string stem = unique_stem("bezel_xmod2792");
    TempFile2792 lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile2792 lObj((dir / (stem + "_lib.o")).string());
    TempFile2792 mSrc((dir / (stem + "_main.bzl")).string());
    TempFile2792 mExe((dir / (stem + "_main")).string());

    writeSource2792(lSrc.path, libSrc);
    writeSource2792(mSrc.path, mainSrc);

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
int runLocal2792(const std::string& src) {
    auto dir = wtDir2792();
    std::string stem = unique_stem("bezel_loc2792");
    TempFile2792 mSrc((dir / (stem + ".bzl")).string());
    TempFile2792 mExe((dir / stem).string());
    writeSource2792(mSrc.path, src);

    bezel::Driver driver;
    if (!driver.compileFile(mSrc.path, mExe.path, {}).success) return -1;
    int status = std::system(mExe.path.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

// The dependency: a self-comparable interface, a self-equatable interface, one
// implementor of both, and a plain class with no interfaces at all.
//
// `Equals` is deliberately NOT identity on `_r` — it answers TRUE when the ranks
// DIFFER.  A test that used the obvious `_r = other.Ident()` could not observe
// the defect: the misreported "string length" read at offset 8 of an RNode
// happens to carry `_r` in its high word, so the string path returned the RIGHT
// answer for an identity-shaped Equals by accident.  Inverting the predicate
// makes the string path's answer differ from the user's.
const char* kLib2792 =
    "module Lib; "
    "public interface IRank: IComparable<IRank> "
    "  method Rank(): int; "
    "end IRank; "
    "public interface IIdent: IEquatable<IIdent> "
    "  method Ident(): int; "
    "end IIdent; "
    "public class RNode: IRank, IIdent "
    "  private _r: int; "
    "  public constructor(r: int) _r := r; end constructor; "
    "  public method Rank(): int => _r; "
    "  public method Ident(): int => _r; "
    "  public method CompareTo(other: IRank): int => _r - other.Rank(); "
    "  public method Equals(other: IIdent): bool => _r <> other.Ident(); "
    "end RNode; "
    "public class Plain "
    "  private _p: int; "
    "  public constructor(p: int) _p := p; end constructor; "
    "  public method P(): int => _p; "
    "end Plain; ";

std::string app2792(const std::string& body) {
    return "module App; import Lib; class C "
           "  public static method Main(): int " + body +
           "  end Main; end C; ";
}
}  // namespace

// #2792 / §2.8.3 rule 4 + §2.5 — THE ISSUE'S OWN REPRO.  `CompareTo` is
// inherited from the compiler-synthesized `IComparable` base of the imported
// `IRank`; 7 - 3 = 4, so `+ 100` is 104.  Before the fix: 100 (CompareTo
// returned 0 for every pair of operands).
BEZEL_TEST(xmod_imported_class_vtable_2792, compareto_through_imported_interface) {
    BEZEL_EXPECT_EQ(runXmod2792(kLib2792, app2792(
        "    using var a := new RNode(7); "
        "    using var b := new RNode(3); "
        "    var x: IRank := a; "
        "    var y: IRank := b; "
        "    return x.CompareTo(y) + 100; ")), 104);
}

// #2792 / §2.8.3 rule 4 — the SECOND builtin-interface family, proving the
// defect is not `IComparable`-specific.  `RNode.Equals` answers "the ranks
// DIFFER", so 7 vs 7 is FALSE → 2.  Before the fix: 1 (the string path compared
// the two objects' misread "lengths", found them equal, and answered TRUE).
BEZEL_TEST(xmod_imported_class_vtable_2792, equals_through_imported_interface) {
    BEZEL_EXPECT_EQ(runXmod2792(kLib2792, app2792(
        "    using var a := new RNode(7); "
        "    using var b := new RNode(7); "
        "    var x: IIdent := a; "
        "    var y: IIdent := b; "
        "    if x.Equals(y) then return 1; end if; "
        "    return 2; ")), 2);
}

// #2792 / §12.7 + §2.8.3 rule 4 — `is` on an `Object` holding an imported
// instance.  Rule 4 is explicit that the test must "match identically regardless
// of which artifact produced the value or performs the test".  Before the fix:
// 22 (the instance was taken for a string, so `is Plain` was FALSE).
BEZEL_TEST(xmod_imported_class_vtable_2792, is_test_on_imported_instance) {
    BEZEL_EXPECT_EQ(runXmod2792(kLib2792, app2792(
        "    using var a := new Plain(7); "
        "    var o: Object := a; "
        "    if o is Plain then return 11; end if; "
        "    return 22; ")), 11);
}

// #2792 / §8.5 — the §8.5 default `ToString` on an imported instance reached
// through `Object` is the type name, "Plain" (5 chars).  Before the fix: 0 (the
// object was read as a string, whose "data"/"length" are its vtable word and
// field bytes).
BEZEL_TEST(xmod_imported_class_vtable_2792, object_tostring_on_imported_instance) {
    BEZEL_EXPECT_EQ(runXmod2792(kLib2792, app2792(
        "    using var a := new Plain(7); "
        "    var o: Object := a; "
        "    var s: string := o.ToString(); "
        // #2934 (§8.5, v0.252): the §8.5 class default is module-path-
        // qualified, and an IMPORTED type carries its EXPORTER's module
        // (ModuleLoader populates TypeSymbol::moduleName from the
        // dependency's metadata, #1488) — "Lib.Plain" (9), was "Plain" (5).
        "    return s.Length; ")), 9);
}

// #2792 / §19.3 + §2.8.3 rule 4 — a `case` TYPE PATTERN over an `Object`
// holding an imported instance.  Rule 4 names §19 patterns alongside `is`.
// Before the fix: 99 (the `_` arm; the typed arm never matched).
BEZEL_TEST(xmod_imported_class_vtable_2792, case_type_pattern_on_imported_instance) {
    BEZEL_EXPECT_EQ(runXmod2792(kLib2792, app2792(
        "    using var a := new Plain(7); "
        "    var o: Object := a; "
        "    return (case o "
        "      of var p: Plain => p.P() + 1, "
        "         _ => 99); ")), 8);
}

// #2792 / §8.3 — `Object.Equals` / `GetHashCode` on an imported instance.
// This one did NOT merely answer wrongly before the fix: it SIGSEGV'd, because
// the object-equality and identity-hash lowerings dereference a receiver the
// discriminator claimed was a string (`bz.string`'s data pointer is the object's
// vtable word).  A -1 here is the compile/link/abnormal-exit sentinel, so the
// pre-fix crash is observed as a failed expectation rather than 11.
BEZEL_TEST(xmod_imported_class_vtable_2792, object_equals_and_hash_on_imported_instance) {
    BEZEL_EXPECT_EQ(runXmod2792(kLib2792, app2792(
        "    using var a := new Plain(7); "
        "    var o: Object := a; "
        "    var b: Object := a; "
        "    var score: int := 0; "
        "    if o.Equals(b) then score := score + 1; end if; "
        "    if o.GetHashCode() = b.GetHashCode() then score := score + 10; end if; "
        "    return score; ")), 11);
}

// #2792 / §4.6 — CONTROL, the over-correction axis.  The fix only ADDS entries
// to the whitelist, so a genuine string must still be recognized as one in the
// very same program.  Digit 1 = `o is string`, digit 2 = the imported instance
// still matches its own class, and the hundreds digit must stay 0 (a string must
// never match the imported class).  Before the fix this was 1: the string half
// already worked, the imported half did not.
BEZEL_TEST(xmod_imported_class_vtable_2792, string_through_object_still_a_string) {
    BEZEL_EXPECT_EQ(runXmod2792(kLib2792, app2792(
        "    var o: Object := \"hello\"; "
        "    using var p := new Plain(7); "
        "    var q: Object := p; "
        "    var score: int := 0; "
        "    if o is string then score := score + 1; end if; "
        "    if q is Plain then score := score + 10; end if; "
        "    if o is Plain then score := score + 100; end if; "
        "    return score; ")), 11);
}

// #2792 — CONTROL, the imported interface's OWN method.  `Rank()` is declared on
// `IRank` itself rather than inherited from a synthesized builtin, and it was
// already correct before the fix; it must stay correct.
BEZEL_TEST(xmod_imported_class_vtable_2792, own_interface_method_unchanged) {
    BEZEL_EXPECT_EQ(runXmod2792(kLib2792, app2792(
        "    using var a := new RNode(7); "
        "    var x: IRank := a; "
        "    return x.Rank() - 2; ")), 5);
}

// #2792 — LINK SAFETY.  The fix emits an EXTERNAL reference to `vt$<Type>`, so
// it must reference only the globals the exporter actually defines externally:
// a NON-PUBLIC class and a GENERIC class both keep INTERNAL linkage there
// (LlvmBackend_Setup.cpp's `exported` gate), and referencing either would be an
// undefined symbol at link time.  A link failure surfaces here as -1.
BEZEL_TEST(xmod_imported_class_vtable_2792, nonpublic_and_generic_types_still_link) {
    const char* lib =
        "module Lib; "
        "class Hidden "
        "  private _h: int; "
        "  public constructor(h: int) _h := h; end constructor; "
        "  public method H(): int => _h; "
        "end Hidden; "
        "public class Box<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public method Get(): T => _v; "
        "end Box; "
        "public class Plain "
        "  private _p: int; "
        "  public constructor(p: int) _p := p; end constructor; "
        "  public method P(): int => _p; "
        "end Plain; ";
    BEZEL_EXPECT_EQ(runXmod2792(lib, app2792(
        "    using var a := new Plain(7); "
        "    var o: Object := a; "
        "    if o is Plain then return 11; end if; "
        "    return 22; ")), 11);
}

// #2792 — LINK SAFETY, the imported-but-unused dependency.  Every public class
// of a presented dependency is now referenced by the consumer's vtable table
// even when the program touches none of them; the dependency's object is on the
// link line (§2.8.3 rule 1), so this must still link and run.
BEZEL_TEST(xmod_imported_class_vtable_2792, unused_dependency_still_links) {
    BEZEL_EXPECT_EQ(runXmod2792(kLib2792,
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

// #2792 — CONTROL, single module.  The identical declarations and bodies in ONE
// file already answered correctly and must be unperturbed: this is the baseline
// the cross-module cases are required to match (§2.5 / §2.8.3 rule 4).
BEZEL_TEST(xmod_imported_class_vtable_2792, local_single_module_control) {
    BEZEL_EXPECT_EQ(runLocal2792(
        "module App; "
        "public interface IRank: IComparable<IRank> "
        "  method Rank(): int; "
        "end IRank; "
        "public class RNode: IRank "
        "  private _r: int; "
        "  public constructor(r: int) _r := r; end constructor; "
        "  public method Rank(): int => _r; "
        "  public method CompareTo(other: IRank): int => _r - other.Rank(); "
        "end RNode; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new RNode(7); "
        "    using var b := new RNode(3); "
        "    var x: IRank := a; "
        "    var y: IRank := b; "
        "    return x.CompareTo(y) + 100; "
        "  end Main; "
        "end C; "), 104);
}
