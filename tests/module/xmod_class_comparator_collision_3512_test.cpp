// Regression tests for fix #3512 — the CLASS `IComparable` comparator thunk was
// cached under the type's UNQUALIFIED name, so two modules each declaring a
// same-named public comparable class shared ONE comparator.
//
// AUTHORITY. spec §2.8.3 rule 3 (v0.283), "Program-wide type identity",
// verbatim:
//
//     "A type is identified program-wide by its **declaring module name, its
//      qualified type name (including any enclosing-type path, §11.6), and its
//      generic arity** ... Two modules each declaring a public type of the same
//      name declare two unrelated types (distinguished in diagnostics by module
//      qualification)."
//
// That is the rule #1488 implemented for every emitted symbol by qualifying it
// through `Emitter::mangledTypeName` — and the rule the class comparator thunk
// did not follow. `buildObjectCompareToThunk` (LlvmBackend_Calls3.cpp) named its
// thunk `bz$classcmp$` + `ty->declaration->name`, the BARE name, while its own
// interface arm (`ensureIfaceCmpThunk`) had used `mangledTypeName` since #2768.
//
// THE DEFECT, MEASURED on 06551dd. Two modules each declaring
// `public class Ord: IComparable<Ord>` with OPPOSITE `CompareTo`, both used from
// one consumer, emit exactly one `bz$classcmp$Ord` (confirmed with `nm`), built
// from whichever type the consumer's lowering reached first. Everything keyed on
// it then answered with the WRONG module's ordering:
//
//   * `Array<T>.Sort()` and `Array<T>.Sorted()` printed the ascending answer for
//     the DESCENDING type (stdlib §4.4);
//   * `SortedDictionary<K, V>` iterated ascending and reported the ascending
//     `FirstKey`/`LastKey` for it (stdlib §4.17, "Ordering is defined by `K`'s
//     `IComparable<K>.CompareTo`");
//   * and — because #3488's `buildSortedObjectKeyIdentity` names its stored
//     identity function after this thunk (`bz$sortedobjkey$eq$` + the name) —
//     MAP MEMBERSHIP too: §4.17's "Two keys are the same key exactly when
//     `CompareTo` returns `0`" was decided by the other module's `CompareTo`.
//
// AND IT WAS NOT MERELY A WRONG ANSWER. Give the two same-named classes
// DIFFERENT FIELD LAYOUTS — a leading `string` against a leading `int` — and the
// shared thunk reads one type's field at the other type's offset. The consumer
// SEGV'd (rc=139) on `Array.Sort()` alone, on both the pre-#3488 tree and
// 06551dd; the `SortedDictionary` shape raised `ArgumentException` from an `Add`
// of a key that was not present, §4.17's duplicate-key tier firing on two
// distinct keys.
//
// THE FIX is one word: key the cache on `mangledTypeName(ty->declaration)`, as
// the interface arm already did. `bz$classcmp$LibP.Ord` and
// `bz$classcmp$LibQ.Ord` are then distinct, and the sorted map's identity
// function inherits the qualified name for free.
//
// WHY THIS SURVIVED. There was NO cross-module class-comparator row anywhere in
// the suite. `cross_module_comparable_iface_2776_test.cpp` covers the INTERFACE
// arm, which was already correct — and is kept here as a control, because a row
// that passes on both trees is what proves the defect was specific to the class
// arm rather than to cross-module comparison in general.
//
// EVERY OBSERVABLE HERE IS A VALUE. An ordering defect is a permutation of the
// same elements, which an exit status cannot distinguish, so the rows compare
// the program's stdout. The crash row asserts the status separately, because a
// signalled process loses its buffered stdout and would otherwise be
// indistinguishable from a merely wrong answer.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

// Per-process scratch dir under the shared base, cleaned up at exit — never
// fs::temp_directory_path(), which is the tmpfs CLAUDE.md forbids for scratch.
const fs::path& wtDir3512() { return wtScratchDir("wt-3512"); }

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// status: the program's exit code, or -1 when a compile/link failed, or -2 when
// the program died on a signal. The three are kept distinct on purpose: the
// pre-fix layout row dies on SIGSEGV, which must not read as "wrong answer".
struct RunOut {
    int status = -1;
    std::string out;
};

// Compile two independent library modules to objects, compile a consumer that
// imports BOTH, link, run, and return its status and stdout. Mirrors
// buildAndRunTwoLibs in cross_module_type_name_collision_1488_test.cpp (which is
// in that file's anonymous namespace and so not reusable), with stdout capture
// added and the scratch location corrected to wtScratchDir.
RunOut buildAndRunTwoLibs(const std::string& libASrc, const std::string& libBSrc,
                          const std::string& mainSrc, const std::string& tag) {
    RunOut r;
    auto dir = wtDir3512();
    std::string stem = "bz3512_" + tag + "_" + unique_stem("");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bObj((dir / (stem + "_b.o")).string());
    TempFile mSrc((dir / (stem + "_main.bzl")).string());
    TempFile mExe((dir / stem).string());
    TempFile mOut((dir / (stem + ".out")).string());

    writeSource(aSrc.path, libASrc);
    writeSource(bSrc.path, libBSrc);
    writeSource(mSrc.path, mainSrc);

    bezel::Driver driver;
    if (!driver.compileObject(aSrc.path, aObj.path).success) return r;
    if (!driver.compileObject(bSrc.path, bObj.path).success) return r;

    std::vector<bezel::Dependency> deps;
    for (const std::string& obj : {aObj.path, bObj.path}) {
        bezel::Dependency d;
        d.metadataPath = obj;
        d.objectPath = obj;
        deps.push_back(d);
    }
    if (!bezel::test::compileFileForTest(driver, mSrc.path, mExe.path, deps).success)
        return r;

    std::string cmd = mExe.path + " > " + mOut.path + " 2>&1";
    int status = std::system(cmd.c_str());
    {
        std::ifstream in(mOut.path);
        std::ostringstream ss;
        ss << in.rdbuf();
        r.out = ss.str();
    }
    if (status == -1) return r;
    r.status = WIFEXITED(status) ? WEXITSTATUS(status) : -2;
    return r;
}

// ── The two same-named comparable classes ──────────────────────────────────
//
// Identical public shape, identical field layout, OPPOSITE `CompareTo`. Under
// spec §2.8.3 rule 3 these are "two unrelated types", so each must be ordered by
// its own comparator. `LibP` is written first in every program below, so it is
// LibP's thunk that the pre-fix cache handed to LibQ — which makes each expected
// string differ from the pre-fix answer in the LibQ half only.
const char* kLibP_asc =
    "module LibP; "
    "public class Ord: IComparable<Ord> "
    "  private _v: int; "
    "  public constructor(v: int) _v := v; end constructor; "
    "  public method Value(): int => _v; "
    "  public method CompareTo(other: Ord): int => _v - other.Value(); "
    "end Ord; ";

const char* kLibQ_desc =
    "module LibQ; "
    "public class Ord: IComparable<Ord> "
    "  private _v: int; "
    "  public constructor(v: int) _v := v; end constructor; "
    "  public method Value(): int => _v; "
    "  public method CompareTo(other: Ord): int => other.Value() - _v; "
    "end Ord; ";

// The membership shape: LibQ's ordering buckets by tens, so `Ord(11)` and
// `Ord(15)` are ONE key under §4.17's "same key exactly when `CompareTo` returns
// `0`" while LibP's `Ord(11)` and `Ord(15)` are two.
const char* kLibQ_tens =
    "module LibQ; "
    "public class Ord: IComparable<Ord> "
    "  private _v: int; "
    "  public constructor(v: int) _v := v; end constructor; "
    "  public method Value(): int => _v; "
    "  public method CompareTo(other: Ord): int "
    "    return (_v / 10) - (other.Value() / 10); "
    "  end CompareTo; "
    "end Ord; ";

// The memory-unsafe shape: a LEADING `string` where LibP has a leading `int`.
// Reading one at the other's offset dereferences an integer as a `bz.string*`.
const char* kLibQ_stringfield =
    "module LibQ; "
    "public class Ord: IComparable<Ord> "
    "  private _name: string; "
    "  public constructor(n: string) _name := n; end constructor; "
    "  public method Name(): string => _name; "
    "  public method CompareTo(other: Ord): int "
    "    => _name.Length - other.Name().Length; "
    "end Ord; ";

std::string app(const std::string& body) {
    return "module App; import LibP; import LibQ as Q; "
           "class Program public static method Main(): int " + body +
           " end Main; end Program; ";
}

}  // namespace

// ── stdlib §4.4 `Array<T>.Sort()` ──────────────────────────────────────────

// per spec §2.8.3 rule 3 + stdlib §4.4 (`public method Sort();  // in place`),
// v0.283, #3512: each module's `Ord` sorts by its OWN `CompareTo`. Pre-fix both
// halves printed the ascending answer, because one `bz$classcmp$Ord` served both
// types.
BEZEL_TEST(xmod_class_comparator_collision_3512,
           array_sort_uses_each_modules_own_comparator) {
    RunOut r = buildAndRunTwoLibs(kLibP_asc, kLibQ_desc, app(
        "  using var p1 := new LibP.Ord(2); "
        "  using var p2 := new LibP.Ord(3); "
        "  using var p3 := new LibP.Ord(1); "
        "  var pa: Array<LibP.Ord> := [p1, p2, p3]; "
        "  pa.Sort(); "
        "  using var q1 := new Q.Ord(2); "
        "  using var q2 := new Q.Ord(3); "
        "  using var q3 := new Q.Ord(1); "
        "  var qa: Array<Q.Ord> := [q1, q2, q3]; "
        "  qa.Sort(); "
        "  Console.WriteLine($\"asc={pa[0].Value()}{pa[1].Value()}{pa[2].Value()}"
        " desc={qa[0].Value()}{qa[1].Value()}{qa[2].Value()}\"); "
        "  return 0; "), "sort");
    BEZEL_EXPECT_EQ(r.out, std::string("asc=123 desc=321\n"));
}

// per stdlib §4.4 (`public method Sorted(): owned Array<T>;  // fresh sorted
// copy`), v0.283,
// #3512 / narrow #3488: the `Sorted()` arm carried its OWN verbatim inline copy
// of this comparator builder — a THIRD spelling, which #3488's commit body said
// did not exist. It is now routed through `buildObjectCompareToThunk` and the
// copy deleted, so this row is the one that would catch a copy left behind: an
// unrouted `Sorted()` would keep colliding here while `Sort()` above went green.
BEZEL_TEST(xmod_class_comparator_collision_3512,
           array_sorted_uses_each_modules_own_comparator) {
    RunOut r = buildAndRunTwoLibs(kLibP_asc, kLibQ_desc, app(
        "  using var p1 := new LibP.Ord(2); "
        "  using var p2 := new LibP.Ord(3); "
        "  using var p3 := new LibP.Ord(1); "
        "  var pa: Array<LibP.Ord> := [p1, p2, p3]; "
        "  var ps := pa.Sorted(); "
        "  using var q1 := new Q.Ord(2); "
        "  using var q2 := new Q.Ord(3); "
        "  using var q3 := new Q.Ord(1); "
        "  var qa: Array<Q.Ord> := [q1, q2, q3]; "
        "  var qs := qa.Sorted(); "
        "  Console.WriteLine($\"asc={ps[0].Value()}{ps[1].Value()}{ps[2].Value()}"
        " desc={qs[0].Value()}{qs[1].Value()}{qs[2].Value()}\"); "
        "  return 0; "), "sorted");
    BEZEL_EXPECT_EQ(r.out, std::string("asc=123 desc=321\n"));
}

// ── stdlib §4.17 `SortedDictionary<K, V>` ──────────────────────────────────

// per stdlib §4.17 (v0.283) "Ordering is defined by `K`'s
// `IComparable<K>.CompareTo`" and "iteration is **O(n)** in ascending key
// order", with `FirstKey` "least key" / `LastKey` "greatest key", #3512: #3488
// routed the sorted map through this very thunk, so the collision reached the
// map's ORDER as well as `Array`'s. Pre-fix the LibQ half printed
// "desc=1,2,3, first=1 last=3".
BEZEL_TEST(xmod_class_comparator_collision_3512,
           sorted_dictionary_orders_by_each_modules_own_comparator) {
    RunOut r = buildAndRunTwoLibs(kLibP_asc, kLibQ_desc, app(
        "  using var p1 := new LibP.Ord(2); "
        "  using var p2 := new LibP.Ord(3); "
        "  using var p3 := new LibP.Ord(1); "
        "  var pd := new SortedDictionary<LibP.Ord, int>(); "
        "  pd.Add(p1, 20); pd.Add(p2, 30); pd.Add(p3, 10); "
        "  var pb := new StringBuilder(); "
        "  for var e in pd do pb.Append($\"{e.Key.Value()},\"); end; "
        "  Console.WriteLine($\"asc={pb.ToString()} first={pd.FirstKey.Value()}"
        " last={pd.LastKey.Value()}\"); "
        "  using var q1 := new Q.Ord(2); "
        "  using var q2 := new Q.Ord(3); "
        "  using var q3 := new Q.Ord(1); "
        "  var qd := new SortedDictionary<Q.Ord, int>(); "
        "  qd.Add(q1, 20); qd.Add(q2, 30); qd.Add(q3, 10); "
        "  var qb := new StringBuilder(); "
        "  for var e2 in qd do qb.Append($\"{e2.Key.Value()},\"); end; "
        "  Console.WriteLine($\"desc={qb.ToString()} first={qd.FirstKey.Value()}"
        " last={qd.LastKey.Value()}\"); "
        "  return 0; "), "sorteddict");
    BEZEL_EXPECT_EQ(r.out, std::string("asc=1,2,3, first=1 last=3\n"
                                       "desc=3,2,1, first=3 last=1\n"));
}

// per stdlib §4.17 (v0.283) "Two keys are the same key exactly when `CompareTo`
// returns `0`", with `TryAdd` "v0.282 (§1 shape (d)): false and no mutation if
// present", #3512: #3488's identity pair is named after this thunk
// (`bz$sortedobjkey$eq$` + the thunk name), so the collision decided MEMBERSHIP,
// not only order. LibQ orders by tens: `Ord(11)` and `Ord(15)` are ONE key under
// its own `CompareTo` and two under LibP's. Pre-fix this printed
// "p=1/False q=2/True" — LibQ's map answering with LibP's identity.
BEZEL_TEST(xmod_class_comparator_collision_3512,
           sorted_dictionary_membership_uses_each_modules_own_comparator) {
    RunOut r = buildAndRunTwoLibs(kLibP_asc, kLibQ_tens, app(
        "  using var p1 := new LibP.Ord(11); "
        "  using var p2 := new LibP.Ord(15); "
        "  var pd := new SortedDictionary<LibP.Ord, int>(); "
        "  pd.Add(p1, 1); "
        "  var pDup := pd.TryAdd(p2, 2); "
        "  Console.Write($\"p={pd.Count}/{pDup}\"); "
        "  using var q1 := new Q.Ord(11); "
        "  using var q2 := new Q.Ord(15); "
        "  var qd := new SortedDictionary<Q.Ord, int>(); "
        "  qd.Add(q1, 1); "
        "  var qDup := qd.TryAdd(q2, 2); "
        "  Console.WriteLine($\" q={qd.Count}/{qDup}\"); "
        "  return 0; "), "membership");
    BEZEL_EXPECT_EQ(r.out, std::string("p=2/True q=1/False\n"));
}

// ── The memory-unsafe shape ────────────────────────────────────────────────

// per spec §2.8.3 rule 3 + stdlib §4.4, v0.283, #3512: with DIFFERENT field
// layouts the shared thunk is not merely wrong, it is unsound — LibP's `Ord`
// stores an `int` where LibQ's stores a `string`, and the pre-fix single thunk
// dereferences one as the other. LibQ's array is sorted FIRST here so LibQ owns
// the shared thunk and LibP's `int` is the value read as a `bz.string*`.
// MEASURED pre-fix: rc=139 (SIGSEGV), on 06551dd and on its parent alike.
//
// The status and the value are asserted separately: a signalled process loses
// its buffered stdout, so an output-only assertion could not tell a crash from a
// wrong answer — which is the whole distinction this row exists to make.
BEZEL_TEST(xmod_class_comparator_collision_3512,
           differing_field_layouts_do_not_reinterpret_one_type_as_the_other) {
    RunOut r = buildAndRunTwoLibs(kLibP_asc, kLibQ_stringfield, app(
        "  using var q1 := new Q.Ord(\"bb\"); "
        "  using var q2 := new Q.Ord(\"c\"); "
        "  using var q3 := new Q.Ord(\"aaa\"); "
        "  var qa: Array<Q.Ord> := [q1, q2, q3]; "
        "  qa.Sort(); "
        "  Console.WriteLine($\"q={qa[0].Name()}{qa[1].Name()}{qa[2].Name()}\"); "
        "  using var p1 := new LibP.Ord(2); "
        "  using var p2 := new LibP.Ord(3); "
        "  using var p3 := new LibP.Ord(1); "
        "  var pa: Array<LibP.Ord> := [p1, p2, p3]; "
        "  pa.Sort(); "
        "  Console.WriteLine($\"p={pa[0].Value()}{pa[1].Value()}{pa[2].Value()}\"); "
        "  return 0; "), "layout");
    // -2 is the signalled-process code this helper reserves; pre-fix this row
    // reported exactly that.
    BEZEL_EXPECT_EQ(r.status, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("q=cbbaaa\np=123\n"));
}

// ── Controls: pass on BOTH trees ───────────────────────────────────────────

// per spec §14.2 rule 3 + §2.8.3 rule 3, v0.283, #2768 / #3512: the INTERFACE
// arm has keyed its thunk on `mangledTypeName` since #2768
// (`bz$ifacecmp$LibP.IOrd` / `bz$ifacecmp$LibQ.IOrd`, confirmed with `nm`), so
// two modules' same-named self-comparable interfaces were ALREADY distinguished.
// This row passes on both trees, which is what makes it a control rather than a
// restatement of the fix: it pins that the defect was the class arm's bare name,
// not cross-module comparison as such, and that requalifying the class arm did
// not disturb the arm that was already right.
BEZEL_TEST(xmod_class_comparator_collision_3512,
           interface_comparator_already_distinguished_modules_control) {
    RunOut r = buildAndRunTwoLibs(
        "module LibP; "
        "public interface IOrd: IComparable<IOrd> "
        "  method Val(): int; "
        "end IOrd; "
        "public class Node: IOrd "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method Val(): int => _v; "
        "  public method CompareTo(other: IOrd): int => _v - other.Val(); "
        "end Node; ",
        "module LibQ; "
        "public interface IOrd: IComparable<IOrd> "
        "  method Val(): int; "
        "end IOrd; "
        "public class Node: IOrd "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method Val(): int => _v; "
        "  public method CompareTo(other: IOrd): int => other.Val() - _v; "
        "end Node; ",
        app("  using var p1 := new LibP.Node(2); "
            "  using var p2 := new LibP.Node(3); "
            "  using var p3 := new LibP.Node(1); "
            "  var pa: Array<LibP.IOrd> := [p1, p2, p3]; "
            "  pa.Sort(); "
            "  using var q1 := new Q.Node(2); "
            "  using var q2 := new Q.Node(3); "
            "  using var q3 := new Q.Node(1); "
            "  var qa: Array<Q.IOrd> := [q1, q2, q3]; "
            "  qa.Sort(); "
            "  Console.WriteLine($\"asc={pa[0].Val()}{pa[1].Val()}{pa[2].Val()}"
            " desc={qa[0].Val()}{qa[1].Val()}{qa[2].Val()}\"); "
            "  return 0; "),
        "ifacecontrol");
    BEZEL_EXPECT_EQ(r.out, std::string("asc=123 desc=321\n"));
}

// Control: two modules whose comparable classes have DIFFERENT names never
// collided in the first place (`bz$classcmp$Asc` / `bz$classcmp$Desc`), so this
// row passes on both trees. It is what separates "the fix works" from "the
// harness observes the right thing": if this row ever failed, the expectation
// strings above would be measuring the runner rather than the compiler.
BEZEL_TEST(xmod_class_comparator_collision_3512,
           differently_named_classes_never_collided_control) {
    RunOut r = buildAndRunTwoLibs(
        "module LibP; "
        "public class Asc: IComparable<Asc> "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method Value(): int => _v; "
        "  public method CompareTo(other: Asc): int => _v - other.Value(); "
        "end Asc; ",
        "module LibQ; "
        "public class Desc: IComparable<Desc> "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method Value(): int => _v; "
        "  public method CompareTo(other: Desc): int => other.Value() - _v; "
        "end Desc; ",
        app("  using var p1 := new LibP.Asc(2); "
            "  using var p2 := new LibP.Asc(3); "
            "  using var p3 := new LibP.Asc(1); "
            "  var pa: Array<LibP.Asc> := [p1, p2, p3]; "
            "  pa.Sort(); "
            "  using var q1 := new Q.Desc(2); "
            "  using var q2 := new Q.Desc(3); "
            "  using var q3 := new Q.Desc(1); "
            "  var qa: Array<Q.Desc> := [q1, q2, q3]; "
            "  qa.Sort(); "
            "  Console.WriteLine($\"asc={pa[0].Value()}{pa[1].Value()}{pa[2].Value()}"
            " desc={qa[0].Value()}{qa[1].Value()}{qa[2].Value()}\"); "
            "  return 0; "),
        "distinctnames");
    BEZEL_EXPECT_EQ(r.out, std::string("asc=123 desc=321\n"));
}

}  // namespace bezel::module
