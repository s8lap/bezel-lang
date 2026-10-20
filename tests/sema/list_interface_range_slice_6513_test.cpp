// Spec-derived tests for issue #6513 — a §15.6 range slice over a receiver that
// is NOT `Array<T>`.
//
// AUTHORITY. Spec §15.6 (v0.338, docs/Bezel_Language_Spec.md, heading
// "15.6 Slicing"), first sentence:
//
//   "Range slicing `arr[a..b]`, `arr[..b]`, `arr[a..]`, `arr[..]` is supported
//    on `Array<T>`. Slicing produces a new `Array<T>` containing copies of the
//    references (or values) in the range; mutating the slice does not affect
//    the original."
//
// and its two exclusion paragraphs, which name every OTHER receiver kind and
// say why it is out: inline arrays, and `OwningArray<T>` ("Slicing is not
// admitted on `OwningArray<T>`").
//
// Nothing in §15.6 admits a list-interface receiver, and no other type declares
// the form: stdlib §4.4's `public method Slice(r: Range<int>): owned
// Array<T>;` is the only `Slice(r: Range<int>)` in the standard library;
// stdlib §4.1/§4.2 spell `IReadOnlyList<T>` / `IList<T>` / `IOwningList<T>` out
// in full and none declares a Slice; stdlib §4.14 says "`Slice` is a member of
// the span *views*, not of the buffer — a buffer is range-restricted by taking a
// span of it"; stdlib §4.19's `FrozenArray<T>` listing declares `From` /
// `Length` / indexer / `ToArray` / destructor and no Slice.
//
// ⚠ DIRECTION. This is a REJECTION, so the oracle is "the program is refused,
// naming §15.6 and the receiver" — not a value. The issue's own report says the
// `IList<string>` program "prints NOTHING and exits 0"; that is MEASURED on the
// base as `BEFORE-SLICE` / `AFTER-SLICE count=0` at exit 0 — a silent WRONG
// ANSWER (0 where 2 is required), not an absent one, and an exit-code /
// didNotCrash oracle reads it as a clean pass. Post-fix the same program does
// not run at all, which is the whole point: a value assertion would have to
// change shape, and "no output" would pass on any program that never ran.
//
// EVERY reject row below pins the SPECIFIC §15.6 sentence naming the receiver
// ("is not admitted on `IList<T>`"), never a bare "§15.6" and never a bare
// `hasErrors()`. A bare section number is short enough to appear in a
// neighbouring diagnostic's text (#4716), and a bare `hasErrors()` passes
// vacuously on the four rows whose pre-fix form was ALREADY rejected — for a
// primitive or class element type the pre-fix mis-typing surfaced as a
// misleading downstream "no member `Count` on primitive type `int`", so only
// the named-diagnostic assertion can tell red from green on those.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// A four-element `Array<string>` producer. `using var` (not `var`) so the
// §28.3 must-use / auto-drop lint does not add a warning that the surrounding
// rows would have to read around — the point of each row is the slice site.
const char* kMakeArr =
    "module M; "
    "class P "
    "  public static method MakeArr(): owned Array<string> "
    "    var a := new Array<string>(); "
    "    for var i in 0..<4 do a.Add(\"e\"); end for; "
    "    return a; "
    "  end MakeArr; "
    "end P; ";

// `body` runs with `il` (a `IList<string>` view of the array) and `s` (the
// slice result) in scope.
JitResult compileListSlice(const std::string& decl, const std::string& body) {
    return runAot(std::string(kMakeArr) +
                  "class Program "
                  "  public static method Main(): int "
                  "    using var src := P.MakeArr(); "
                  "    " + decl + " " + body +
                  "  end Main; "
                  "end Program; ");
}

}  // namespace

// ── §15.6: the list INTERFACES are not §15.6 receivers ──────────────────

// #6513 / §15.6: `IList<string>`. On the BASE this compiled, ran and printed
// `count=0` where `2` is required, at exit 0, with no diagnostic — sema typed
// the whole index expression as the ELEMENT `string` while codegen's
// `lowerCollectionIndexerGet` took the slice arm and produced a `bz_array_t*`.
BEZEL_TEST(list_iface_range_slice_6513, ilist_string_range_slice_rejected) {
    JitResult r = compileListSlice("var il: IList<string> := src;",
                                   "var s := il[0..1]; "
                                   "Console.WriteLine(s.Count); "
                                   "return 0;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, "§15.6"));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "range slicing `[a..b]` is not admitted on `IList<T>`"));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// #6513 / §15.6: `IReadOnlyList<T>` — the same defect through the read-only
// view, which is the type a read-only parameter is normally written at.
BEZEL_TEST(list_iface_range_slice_6513, ireadonlylist_range_slice_rejected) {
    JitResult r = compileListSlice("var il: IReadOnlyList<string> := src;",
                                   "var s := il[0..1]; "
                                   "Console.WriteLine(s.Count); "
                                   "return 0;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "range slicing `[a..b]` is not admitted on `IReadOnlyList<T>`"));
}

// #6513 / §15.6: `IOwningList<T>`. Its element type is class-constrained, so
// the pre-fix program was ALREADY rejected — but with "no member `Count` on
// type `Node`", naming a member rather than the rule. Only the named-diagnostic
// assertion distinguishes this row's red from its green.
BEZEL_TEST(list_iface_range_slice_6513, iowninglist_range_slice_rejected) {
    JitResult r = runAot(
        "module M; "
        "class Node public Name: string; "
        "  public constructor(n: string) Name := n; end constructor; "
        "end Node; "
        "class Program "
        "  public static method Main(): int "
        "    var a := new OwningArray<Node>(); "
        "    var n0 := new Node(\"n0\"); "
        "    a.Add(move n0); "
        "    var il: IOwningList<Node> := a; "
        "    var s := il[0..0]; "
        "    Console.WriteLine(s.Count); "
        "    return 0; "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "range slicing `[a..b]` is not admitted on `IOwningList<T>`"));
}

// #6513 / §15.6: the SECOND admitted spelling of a range index — a `Range`-typed
// VALUE rather than a range literal (`var r := 0..1; il[r]`), which sema folds
// into the same `isRangeIdx` flag (#1079). Both spellings must be refused;
// covering only the literal would leave `il[r]` silently mis-typed.
BEZEL_TEST(list_iface_range_slice_6513, range_valued_index_rejected) {
    JitResult r = compileListSlice("var il: IList<string> := src;",
                                   "var r: Range<int> := 0..1; "
                                   "var s := il[r]; "
                                   "Console.WriteLine(s.Count); "
                                   "return 0;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "range slicing `[a..b]` is not admitted on `IList<T>`"));
}

// ── §15.6: the two non-interface `isArrLike` heads outside §15.6 ──────────
// Same site, same clause of §15.6, same mis-typing (sema types the index
// expression as the element; codegen's `isArr` covers both heads via
// `isCollectionTypeName`, so the slice arm runs). Both pre-fix programs were
// rejected with the same misleading "no member `Count` on primitive type
// `int`", so — as above — these rows are red pre-fix only through the named
// diagnostic.

// #6513 / §15.6: `FrozenArray<T>` (stdlib §4.19 declares no Slice; its route
// to a sub-range is `ToArray()`).
BEZEL_TEST(list_iface_range_slice_6513, frozen_array_range_slice_rejected) {
    JitResult r = runAot(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    using var src := new Array<int>(); "
        "    src.Add(1); src.Add(2); src.Add(3); src.Add(4); "
        "    using var fa: FrozenArray<int> := FrozenArray.From(src); "
        "    var s := fa[0..1]; "
        "    Console.WriteLine(s.Count); "
        "    return 0; "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "range slicing `[a..b]` is not admitted on `FrozenArray<T>`"));
}

// #6513 / §15.6: `OwningBuffer<T>` (stdlib §4.14: "`Slice` is a member of the
// span *views*, not of the buffer — a buffer is range-restricted by taking a
// span of it"; the remedy is `buf.AsSpan(0, n)`).
BEZEL_TEST(list_iface_range_slice_6513, owning_buffer_range_slice_rejected) {
    JitResult r = runAot(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<int>(4); "
        "    buf[0] := 1; buf[1] := 2; "
        "    var s := buf[0..1]; "
        "    Console.WriteLine(s.Count); "
        "    return 0; "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "range slicing `[a..b]` is not admitted on `OwningBuffer<T>`"));
}

// ── OVER-FIRE GUARDS. A rejection is only worth its blast radius if the
// admitted shapes keep working. These are the controls that would catch a
// predicate widened past §15.6. ─────────────────────────────────────────────

// §15.6 (the admitted receiver) still produces a fresh owned `Array<T>` of
// the right LENGTH — 0..1 inclusive over four elements is 2. Guards against a
// predicate that over-fires on `Array<T>`.
BEZEL_TEST(list_iface_range_slice_6513, array_range_slice_still_slices) {
    JitResult r = runAot(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "    using var s := a[0..1]; "
        "    return s.Count; "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 2);
}

// The `Range`-valued spelling on the ADMITTED receiver must keep working too —
// the same `isRangeIdx` flag carries both, so a guard on the literal alone
// would not notice the flag being set wrongly.
BEZEL_TEST(list_iface_range_slice_6513, array_range_valued_slice_still_slices) {
    JitResult r = runAot(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "    var r: Range<int> := 0..1; "
        "    using var s := a[r]; "
        "    return s.Count; "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 2);
}

// A PLAIN (non-range) index on a list interface is §4.1's indexer and stays
// admitted (issue #401) — it must not draw the §15.6 slice diagnostic. Reads
// the DATA (`e2`), not a constant, so the row can see a fix that misreads its
// index.
BEZEL_TEST(list_iface_range_slice_6513, plain_index_on_list_iface_still_reads) {
    JitResult r = compileListSlice("var il: IList<string> := src;",
                                   "return il[2].Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// A plain index on `FrozenArray<T>` likewise stays admitted.
BEZEL_TEST(list_iface_range_slice_6513, plain_index_on_frozen_array_still_reads) {
    JitResult r = runAot(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    using var src := new Array<int>(); "
        "    src.Add(7); src.Add(8); src.Add(9); src.Add(10); "
        "    using var fa: FrozenArray<int> := FrozenArray.From(src); "
        "    return fa[2]; "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 9);
}

// §15.6 on `string` is a DIFFERENT feature and stays admitted — the string
// range slice (`emitStringRangeSlice`) has its own arm and its own result type.
// Guards against a predicate reaching the Primitive receiver arm.
BEZEL_TEST(list_iface_range_slice_6513, string_range_slice_still_slices) {
    JitResult r = runAot(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    var s := \"abcdef\"[1..3]; "
        "    return s.Length; "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 3);
}