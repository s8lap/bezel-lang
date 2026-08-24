// Regression tests for #2796 — boxing an IMPORTED record into `Object` (or into
// an interface the record implements) emitted NO boxing at all, so the record's
// own bytes were stored into the reference-typed slot and then released as if
// they were a pointer: SIGSEGV.
//
// WHAT THE DOCUMENTS REQUIRE.  spec §8.6 (v0.240): "When a value type is used
// where `Object` (or an interface implemented by the value type) is expected,
// the value is **boxed**: a heap-allocated wrapper holds the value."  §2.5 makes
// an imported type's resolution artifact-based but its *use* ordinary, and
// §2.8.3 rule 4 gives a type "one runtime type identity" under which `is` tests
// and §19 patterns "match identically regardless of which artifact produced the
// value or performs the test".  Nothing in any of the three distinguishes a
// record this module declared from one it imported.
//
// THE DEFECT.  setupBoxDescriptors() (LlvmBackend_Setup.cpp) minted the
// descriptor (`btd$<R>`) and box vtable (`bvt$box$<R>`) for the record/enum
// TypeSymbols it found in `ctx_.symbols()` — which, by SemaContext's own design,
// holds only the LOCAL module's declarations; imported symbols are owned in a
// separate list precisely so that codegen never tries to emit a BODY for an
// externally-defined symbol.  So `boxVtable_` had no entry for an imported
// record, `boxVtableFor`
// returned null, and `boxValue`'s `if (!vt) return value;` made the box a SILENT
// NO-OP.  Measured on the issue's own two-module repro, the consumer's IR then
// read (`BEZEL_DUMP_IR=1`):
//
//     %0 = alloca %record.Pt          ; the `o: Object` local
//     store %record.Pt %6, ptr %0     ; no box allocated, no box vtable
//     %box.rel = load ptr, ptr %0     ; = 0x0000000400000003  (x=3, y=4)
//     call void @bz_box_release(ptr %box.rel)
//
// i.e. the §28.9 box-release that lowerLocalDecl emits for the slot — gated on
// isBoxableValueType, which DOES answer true for the imported record — loaded
// the record's first eight bytes and freed them as an address.
//
// THE FIX widens the ENUMERATION (not any per-type gate): the record/enum
// TypeSymbols this module must be able to box are the local ones PLUS the
// imported ones, and everything downstream — the descriptor chain, the itable
// and its box thunks, the ToString/Equals/GetHashCode thunks, the vtable — runs
// unchanged on both.
//
// ⚠ WHAT THIS DOES NOT COVER, measured on both sides of the change and left to
// its own issue: a value the DEPENDENCY boxed still carries the dependency's own
// INTERNAL-linkage `bvt$box$<R>`/`btd$<R>`, which no consumer can name, so
// cross-artifact box identity is unchanged (#2797 — it needs a linkage/ABI
// decision).  #2798 (`o is Bag<int>` for an imported generic CLASS) is a
// separate descriptor-identity defect and is also unchanged.

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
const std::filesystem::path& wtDir2796() {
    return wtScratchDir("wt-2796");
}

struct TempFile2796 {
    std::string path;
    explicit TempFile2796(std::string p) : path(std::move(p)) {}
    ~TempFile2796() { std::remove(path.c_str()); }
};

void writeSource2796(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile lib → object, compile main importing lib → executable, run.
// Returns the exit code, or -1 when either compile, the link, or the run fails.
// A -1 is a meaningful outcome here: the pre-fix behaviour of most cases below
// is a SIGSEGV (WIFEXITED false), and the fix adds external references to the
// dependency's record methods, so a link failure must be visible too.
int runXmod2796(const std::string& libSrc, const std::string& mainSrc) {
    auto dir = wtDir2796();
    std::string stem = unique_stem("bezel_xmod2796");
    TempFile2796 lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile2796 lObj((dir / (stem + "_lib.o")).string());
    TempFile2796 mSrc((dir / (stem + "_main.bzl")).string());
    TempFile2796 mExe((dir / (stem + "_main")).string());

    writeSource2796(lSrc.path, libSrc);
    writeSource2796(mSrc.path, mainSrc);

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
int runLocal2796(const std::string& src) {
    auto dir = wtDir2796();
    std::string stem = unique_stem("bezel_loc2796");
    TempFile2796 mSrc((dir / (stem + ".bzl")).string());
    TempFile2796 mExe((dir / stem).string());
    writeSource2796(mSrc.path, src);

    bezel::Driver driver;
    if (!driver.compileFile(mSrc.path, mExe.path, {}).success) return -1;
    int status = std::system(mExe.path.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

// The dependency: one record per shape the box machinery treats differently —
// positional all-primitive, positional with a `string` field (§9.1), body-only
// (§9.2), a record implementing an interface (§9.6), an enum, a generic record
// (§12.7/§12.9), and a static factory so a value the DEPENDENCY produced can be
// boxed by the CONSUMER.
const char* kLib2796 =
    "module Lib; "
    "public interface IShape "
    "  method Area(): int; "
    "end IShape; "
    "public record Pt(X: int, Y: int); "
    "public record Named(Tag: string, N: int); "
    "public record Rect "
    "  public readonly property W: int; "
    "  public readonly property H: int; "
    "  public constructor(w: int, h: int) "
    "    W := w; "
    "    H := h; "
    "  end constructor; "
    "  public method Twice(): int => (W + H) * 2; "
    "end Rect; "
    "public record Sq(Side: int): IShape "
    "  public method Area(): int => Side * Side; "
    "end Sq; "
    "public record Pair<A, B>(First: A, Second: B); "
    "public enum Color Red, Green, Blue; end Color; "
    "public class Maker "
    "  public static method MakePt(x: int, y: int): Pt => new Pt(x, y); "
    "end Maker; ";

std::string app2796(const std::string& body) {
    return "module App; import Lib; class C "
           "  public static method Main(): int " + body +
           "  end Main; end C; ";
}
}  // namespace

// #2796 / §8.6 — THE ISSUE'S OWN REPRO, verbatim.  Nothing is even read back:
// declaring the `Object` local is enough, because the box-release emitted for
// its slot freed the record's own bytes.  Before the fix: SIGSEGV (-1 here).
BEZEL_TEST(xmod_imported_record_box_2796, boxing_imported_record_into_object) {
    BEZEL_EXPECT_EQ(runXmod2796(kLib2796, app2796(
        "    var p: Pt := new Pt(3, 4); "
        "    var o: Object := p; "
        "    return 5; ")), 5);
}

// #2796 / §8.6 + §12.7 + §8.5 + §8.3 — the whole `Object` surface over a boxed
// imported record: `is`, the `as` round-trip (the payload must survive at the
// box's +16 offset), §8.5 `ToString`, and §9.1's structural `Equals`/
// `GetHashCode` reached through `Object`.  1|2|4|8|16 = 31.  Before the fix:
// SIGSEGV (-1).
BEZEL_TEST(xmod_imported_record_box_2796, is_as_tostring_equals_hash) {
    BEZEL_EXPECT_EQ(runXmod2796(kLib2796, app2796(
        "    var score: int := 0; "
        "    var p: Pt := new Pt(3, 4); "
        "    var o: Object := p; "
        "    if o is Pt then score := score + 1; end if; "
        "    var q: Pt? := (o as Pt); "
        "    if q <> null and q!.X = 3 and q!.Y = 4 then score := score + 2; end if; "
        "    var s: string := o.ToString(); "
        "    if s.Length > 0 then score := score + 4; end if; "
        "    var o2: Object := new Pt(3, 4); "
        "    if o.Equals(o2) then score := score + 8; end if; "
        "    if o.GetHashCode() = o2.GetHashCode() then score := score + 16; end if; "
        "    return score; ")), 31);
}

// #2796 / §9.1 + §9.2 + §10 + §19.3 — the record SHAPES the box machinery treats
// differently, plus an enum (same enumeration, same Pass A) and a §19.3 `case`
// TYPE PATTERN, which §2.8.3 rule 4 names alongside `is`.  1|2|4|8|16 = 31.
// Before the fix: SIGSEGV (-1).
BEZEL_TEST(xmod_imported_record_box_2796, string_field_body_record_enum_case_pattern) {
    BEZEL_EXPECT_EQ(runXmod2796(kLib2796, app2796(
        "    var score: int := 0; "
        "    var n: Named := new Named(\"hi\", 7); "
        "    var o: Object := n; "
        "    if o is Named then score := score + 1; end if; "
        "    var s: string := o.ToString(); "
        "    if s.Length > 0 then score := score + 2; end if; "
        "    var r: Rect := new Rect(2, 5); "
        "    var o2: Object := r; "
        "    if o2 is Rect then score := score + 4; end if; "
        "    var c: Color := Color.Green; "
        "    var o3: Object := c; "
        "    if o3 is Color then score := score + 8; end if; "
        "    score := score + (case o "
        "      of var m: Named => 16, "
        "         _ => 0); "
        "    return score; ")), 31);
}

// #2796 / §8.6 (the INTERFACE half of the boxing sentence) + §9.6 — an imported
// record reaching an interface it implements must box with an itable whose slots
// forward to the dependency's real method, so `sh.Area()` is 36 and not garbage;
// and the box must answer `is` for BOTH the interface and the record.  The last
// digit boxes a value the DEPENDENCY produced (still boxed HERE, which is what
// this fix covers — a value the dependency ITSELF boxed is #2797).
// 1|2|4|8 = 15.  Before the fix: SIGSEGV (-1).
BEZEL_TEST(xmod_imported_record_box_2796, interface_dispatch_on_boxed_imported_record) {
    BEZEL_EXPECT_EQ(runXmod2796(kLib2796, app2796(
        "    var score: int := 0; "
        "    var sq: Sq := new Sq(6); "
        "    var sh: IShape := sq; "
        "    if sh.Area() = 36 then score := score + 1; end if; "
        "    var o: Object := sq; "
        "    if o is IShape then score := score + 2; end if; "
        "    if o is Sq then score := score + 4; end if; "
        "    var p: Pt := Maker.MakePt(8, 9); "
        "    var o2: Object := p; "
        "    if o2 is Pt then score := score + 8; end if; "
        "    return score; ")), 15);
}

// #2796 / §8.6 + §15.4 — the collection-literal element position is the other
// place §8.6's boxing sentence bites (the shape #2771 fixed for LOCAL value
// types).  1|2|4|8 = 15.  Before the fix: 1 (the two
// type tests were DROPPED and the `as` degraded to a zero value).
BEZEL_TEST(xmod_imported_record_box_2796, imported_record_in_object_collection) {
    BEZEL_EXPECT_EQ(runXmod2796(kLib2796, app2796(
        "    var score: int := 0; "
        "    var a: Array<Object> := [new Pt(1, 2), new Pt(3, 4)]; "
        "    if a.Length = 2 then score := score + 1; end if; "
        "    if a[0] is Pt then score := score + 2; end if; "
        "    if a[1] is Pt then score := score + 4; end if; "
        "    var b: Pt? := (a[1] as Pt); "
        "    if b <> null and b!.Y = 4 then score := score + 8; end if; "
        "    return score; ")), 15);
}

// #2796 / §2.8.3 rule 3 — OVER-CORRECTION axis.  "Two modules each declaring a
// public type of the same name declare two unrelated types".  The consumer now
// mints a descriptor for the imported `Rect` alongside the one it mints for its
// OWN `Rect`; the two must stay distinct.  The hundreds digit must stay 0.
// 1|2 = 3.  Before the fix: SIGSEGV (-1).
BEZEL_TEST(xmod_imported_record_box_2796, local_and_imported_same_name_stay_distinct) {
    BEZEL_EXPECT_EQ(runXmod2796(kLib2796,
        "module App; import Lib; "
        "public record Rect "
        "  public readonly property A: int; "
        "  public constructor(a: int) "
        "    A := a; "
        "  end constructor; "
        "end Rect; "
        "class C "
        "  public static method Main(): int "
        "    var score: int := 0; "
        "    var localR: Rect := new Rect(5); "
        "    var o: Object := localR; "
        "    if o is Rect then score := score + 1; end if; "
        "    var lp: Pt := new Pt(3, 4); "
        "    var o2: Object := lp; "
        "    if o2 is Pt then score := score + 2; end if; "
        "    if o2 is Rect then score := score + 100; end if; "
        "    if o is Pt then score := score + 100; end if; "
        "    return score; "
        "  end Main; "
        "end C; "), 3);
}

// #2796 / §12.7 + §12.9 + §8.6 "Nullable value-type boxing" — an imported
// GENERIC record must box with its per-monomorphization identity (matching
// `Pair<int,int>` and NOT the sibling `Pair<string,int>`, which §12.9's
// invariance requires), and `T?` must box per §8.6: null → a null `Object`
// reference, non-null → a boxed `T`.  1|2|4|8 = 15, hundreds digit 0.
// Before the fix: SIGSEGV (-1).
BEZEL_TEST(xmod_imported_record_box_2796, imported_generic_record_and_nullable_boxing) {
    BEZEL_EXPECT_EQ(runXmod2796(kLib2796, app2796(
        "    var score: int := 0; "
        "    var pr: Pair<int, int> := new Pair<int, int>(3, 4); "
        "    var o: Object := pr; "
        "    if o is Pair<int, int> then score := score + 1; end if; "
        "    if o is Pair<string, int> then score := score + 100; end if; "
        "    var p: Pt? := null; "
        "    var o2: Object? := p; "
        "    if o2 = null then score := score + 2; end if; "
        "    var p2: Pt? := new Pt(1, 2); "
        "    var o3: Object? := p2; "
        "    if o3 <> null then score := score + 4; end if; "
        "    if o3 is Pt then score := score + 8; end if; "
        "    return score; ")), 15);
}

// #2796 — LINK SAFETY.  The consumer now walks EVERY imported record and emits
// box thunks that reference the dependency's record methods, so it must only
// reference what the dependency actually exports: a NON-public record's members
// stay internal there, and referencing them would be an undefined symbol.  Also
// covers a record implementing a compiler-SYNTHESIZED builtin interface (the
// #2776 shape) and a record with a user `ToString` override, which the box
// vtable's §8.5 slot must reach.  1|2|4|8 = 15; a link failure surfaces as -1.
BEZEL_TEST(xmod_imported_record_box_2796, nonpublic_and_builtin_iface_records_still_link) {
    const char* lib =
        "module Lib; "
        "public interface IShape "
        "  method Area(): int; "
        "end IShape; "
        "record Hidden(Side: int): IShape "
        "  public method Area(): int => Side * 3; "
        "end Hidden; "
        "record Quiet(A: int, B: int); "
        "public record Rank(V: int): IComparable<Rank> "
        "  public method CompareTo(other: Rank): int => V - other.V; "
        "end Rank; "
        "public record Tagged(T: int) "
        "  public method ToString(): string => \"TAG\"; "
        "end Tagged; "
        "public class Use "
        "  public static method HiddenArea(s: int): int => new Hidden(s).Area(); "
        "end Use; ";
    BEZEL_EXPECT_EQ(runXmod2796(lib, app2796(
        "    var score: int := 0; "
        "    if Use.HiddenArea(2) = 6 then score := score + 1; end if; "
        "    var r: Rank := new Rank(7); "
        "    var o: Object := r; "
        "    if o is Rank then score := score + 2; end if; "
        "    var t: Tagged := new Tagged(1); "
        "    var o2: Object := t; "
        "    if o2 is Tagged then score := score + 4; end if; "
        "    var s: string := o2.ToString(); "
        "    if s = \"TAG\" then score := score + 8; end if; "
        "    return score; ")), 15);
}

// #2796 / §28.9 — repeated boxing.  A one-shot `Main` is a weak oracle for a box
// whose release is mis-emitted, so amplify: 20 iterations × four boxes, plus a
// churn box per pass.  4 × 20 = 80 and a clean exit (the pre-fix single-shot
// spelling already SIGSEGV'd; a per-iteration bad free would too).
BEZEL_TEST(xmod_imported_record_box_2796, repeated_boxing_in_a_loop) {
    BEZEL_EXPECT_EQ(runXmod2796(kLib2796, app2796(
        "    var hits: int := 0; "
        "    var i: int := 0; "
        "    while i < 20 do "
        "      i := i + 1; "
        "      var p: Pt := new Pt(i, i + 1); "
        "      var o: Object := p; "
        "      if o is Pt then hits := hits + 1; end if; "
        "      var r: Rect := new Rect(i, i); "
        "      var o2: Object := r; "
        "      if o2 is Rect then hits := hits + 1; end if; "
        "      var sq: Sq := new Sq(i); "
        "      var sh: IShape := sq; "
        "      if sh.Area() = i * i then hits := hits + 1; end if; "
        "      var c: Color := Color.Blue; "
        "      var o3: Object := c; "
        "      if o3 is Color then hits := hits + 1; end if; "
        "      var churn: Object := i; "
        "    end while; "
        "    return hits; ")), 80);
}

// #2796 — CONTROL, single module.  The identical declarations and body in ONE
// file already worked and must be unperturbed: this is the baseline §2.5 /
// §2.8.3 rule 4 require the cross-module spelling to match.
BEZEL_TEST(xmod_imported_record_box_2796, local_single_module_control) {
    BEZEL_EXPECT_EQ(runLocal2796(
        "module App; "
        "public record Pt(X: int, Y: int); "
        "class C "
        "  public static method Main(): int "
        "    var score: int := 0; "
        "    var p: Pt := new Pt(3, 4); "
        "    var o: Object := p; "
        "    if o is Pt then score := score + 1; end if; "
        "    var q: Pt? := (o as Pt); "
        "    if q <> null and q!.X = 3 then score := score + 2; end if; "
        "    return score; "
        "  end Main; "
        "end C; "), 3);
}
