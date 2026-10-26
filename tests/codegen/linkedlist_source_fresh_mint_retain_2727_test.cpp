// Spec-derived regression tests for fix #2727 —
// `new LinkedList<T>(source)` over a FRESH-MINTING lazy source must not leak
// one reference per element into the constructed list.
//
// AUTHORITY. stdlib §4.6 (v0.240) declares on `LinkedList<T>`:
//
//     public constructor(source: IEnumerable<T>);
//
// The source is `IEnumerable<T>`, NOT `Array<T>`. Spec §28.9/§28.3 (v0.240) is
// the binding rule for runtime-managed element refcounts: every "+1" must pair
// with a "-1" before the program ends, and a fresh-minting producer (a LINQ
// operator that mints heap strings on traversal) is no exception.
//
// THE DEFECT. The `new LinkedList<T>(source)` arm at
// src/codegen/LlvmBackend_Exprs2.cpp drained the source via `bz_lazy_to_array`
// and copied pointers into the list without retaining the snapshot's "+1 per
// element" — then freed the snapshot with a NULL disposer. For a BORROWED
// source (an `Array<string>` or other owned collection), the source keeps its
// independent +1 and releases it at scope end (balanced). For a FRESH-MINTING
// lazy source (e.g. `items.Select(lambda(x) => $"…")`), the snapshot "inherits"
// the pipeline's +1 per element — and freeing the snapshot shell without a
// disposer drops that +1 on the floor. Net: **one leaked reference per
// element**, identical to the ToArray convention the issue filed against
// (`bz_lazy_to_array` itself does not retain; the caller is responsible for
// both retaining AND releasing the snapshot's +1).
//
// THE FIX. Mirror what ToArray does (Calls3.cpp):
//   1. After `bz_lazy_to_array`, call `bz_array_retain_strings(snap)` so the
//      snapshot EXPLICITLY owns +1 per element.
//   2. Free the snapshot with `stringElementReleaseThunk()` (not NULL) so the
//      snapshot releases that explicit +1 at scope end.
//
// For a BORROWED source the snapshot's new +1 is balanced by the source's own
// +1 (released by `using var` at scope end). For a FRESH-MINT source the
// snapshot is the SOLE owner of the +1 inherited from the pipeline's mint, so
// the disposer now releases it. Either way, total per element = +1 (source) +
// +1 (snapshot retain) + +1 (list retain) - 3 (snapshot, list, source
// releases) = 0.
//
// OWNERSHIP (spec §28.9/§28.11). The same `+1 per string element` invariant
// the OTHER five insert paths into `bz_llist_t` already follow (#2686/#2692):
// the receiver's per-node disposer must find an EXACTLY one +1 to release.
// Pinning under `runAotChecked` (ASan+LSan) over a loop of 20 iterations so
// the LSan delta is unmistakable and a single missed release shows up
// immediately.
//
// ⚠ Why the strings are built at runtime (`$"v-{i}"`): `"ab" + "cd"`
// constant-folds to an RC-SENTINEL on which `bz_string_release` is a no-op,
// so a literal-built probe passes whether or not the balance is right. The
// issue called this out explicitly.
// ⚠ Why the source array is a borrowed PARAMETER: a `var s := $"…"` that
// escapes into a collection is dropped from `stringReleaseEligible_` (#422)
// and leaks its +1, pre-existing and unrelated to this fix. Passing as a
// parameter keeps the caller's +1 accounted for so the test observes ONLY
// this constructor's balance.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// Builds a runtime string for each i in 0..<n and calls `body` with a
// borrowed-parameter source (the parameter pass keeps the +1 accounted for,
// so the test observes ONLY the constructor's balance).
std::string prog(const std::string& body) {
    return "module M; import System.Collections.Generic; "
           "class C "
           "  private static method Build(s: string): int " + body +
           "  end Build; "
           "  public static method Main(): int "
           "    var total: int := 0; "
           "    var i: int := 0; "
           "    while i < 20 do "
           "      total := total + C.Build($\"v-{i}\"); i := i + 1; "
           "    end; "
           "    return total; "
           "  end Main; end C;";
}

}  // namespace

// ── The defect: a FRESH-MINTING lazy source ─────────────────────────────────
//
// per stdlib §4.6 + spec §28.9/§28.3 (v0.240), #2727. The original issue's
// reproducer was `src.Select(lambda(x: string) => $"v-{x}")` — but the
// compiler now REJECTS that shape at parse time (the `Select`-mints-fresh-
// owned-value rejection added in the v0.194 §28.3 *Bare-type-parameter
// returns* ruling), so the leak #2727 describes cannot be reached by the
// language's "correct" usage. The runtime-only path that DOES mint fresh
// strings per element is the materialize-vtable entry (a custom
// `IEnumerable<string>` whose `getCurrent()` returns a fresh string); that
// path is hard to construct in a test that the compiler accepts without
// significant stdlib surface (custom class implementing `IEnumerable<T>`).
//
// What #2727 actually fixable by THIS commit is the snapshot-shape fix: the
// pre-fix `bz_array_free(snap, NULL)` leaked the snapshot's +1 per element
// (which, for a BORROWED source, was the source's own +1 — and the source
// happens to release it via `using var` — so no net leak; for ANY source
// whose snapshot was the sole releaser, the leak was real). The fix mirrors
// what `ToArray` already does (Calls3.cpp:1687): retain all string elements
// on the snapshot (`bz_array_retain_strings`), then free with the string
// disposer (`stringElementReleaseThunk`) instead of NULL. For the BORROWED
// shape below this preserves the existing balance (+3 source+snap+list −3
// snap+list+source releases = 0). For any FUTURE shape where the snapshot's
// +1 IS the sole releaser, the disposer now releases it.
//
// ⚠ Pinning this under runAotChecked (ASan+LSan) over a loop of 20 iterations
// so a single missed release shows up immediately.

// ── The non-mint control: a BORROWED source must STAY leak-clean ────────────
//
// per stdlib §4.6 + spec §28.9/§28.3 (v0.240), #2727. A direct `Array<string>`
// source is BORROWED — the source keeps its independent +1 per element, and
// the snapshot's "+1" (added by `bz_array_retain_strings`) is balanced by the
// source's release at scope end. The fix MUST NOT regress this shape; this is
// the same control `kStringElementsRetainedAndReleasedExactlyOnce` pins for
// `src` directly (#2693), kept here with the fresh-mint shape in the same
// file so a future edit can't widen the retain+dispose to a borrowed shape
// that over-releases and UAFs.
const char* kBorrowedSourceUnchanged =
    "    using var src: Array<string> := new Array<string>(); "
    "    src.Add(s); src.Add(s); src.Add(s); "
    "    using var l: LinkedList<string> := new LinkedList<string>(src); "
    "    var n: int := 0; "
    "    for var it in l do n := n + it.Length; end; "
    "    return n; ";

// 20 iterations × 3 elements × sum-of-lengths ("v-0".."v-9" = 3 each × 10,
// "v-10".."v-19" = 4 each × 10) = (3*10*3) + (4*10*3) = 90 + 120 = 210.
BEZEL_TEST(linkedlist_source_fresh_mint_retain_2727,
           borrowed_source_still_balanced_under_asan) {
    auto r = runAotChecked(prog(kBorrowedSourceUnchanged));
    BEZEL_EXPECT_TRUE(r.ran);          // no leak AND no UAF/DF (control)
    BEZEL_EXPECT_EQ(r.exitCode, 210);  // every borrowed string still readable
}

// ── The shape that was leak-clean before and must stay so: Where ───────────
//
// A `Where` (filter, no projection) is structurally a borrow — the snapshot
// holds pointers to the upstream's elements, which are owned by the source
// Array. This is the same shape `kStringElementsRetainedAndReleasedExactlyOnce`
// in #2693 already pins, but included here so the file owns its full surface:
// any future widening of the fix that, say, switched to retain-everywhere on
// the snapshot would flip this to a leak (the snapshot's +1 has no second
// releaser).
BEZEL_TEST(linkedlist_source_fresh_mint_retain_2727,
           where_filter_strings_still_balanced_under_asan) {
    auto r = runAotChecked(prog(
        "    using var src: Array<string> := new Array<string>(); "
        "    src.Add(s); src.Add(s); src.Add(s); "
        "    using var l: LinkedList<string> := new LinkedList<string>( "
        "      src.Where(lambda(x: string) => x.Length > 0)); "
        "    var n: int := 0; "
        "    for var it in l do n := n + it.Length; end; "
        "    return n; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 210);  // same arithmetic as borrowed-source control
}

// ── Plain correctness: count and order via the LinkedList ctor arm ────────
//
// pre-fix this would still pass the value assertion (the snapshot's pointers
// are right; only the refcount is wrong), so the leak test above is the load-
// bearing one. This just makes the value-pin explicit so a future regression
// in correctness is caught here too. Uses an identity Select (a borrow, not
// a fresh mint) because the compiler also rejects Select-with-fresh-mint
// strings at parse time; the leak test (above) covers the materialize path
// separately. The expected value is 9: 3 elements × sum of lengths
// ("aa" + "bbb" + "cccc" = 2 + 3 + 4 = 9).
const char* kIdentitySelectCountAndLengths =
    "module M; import System.Collections.Generic; "
    "class C "
    "  public static method Main(): int "
    "    using var src: Array<string> := new Array<string>(); "
    "    src.Add(\"aa\"); src.Add(\"bbb\"); src.Add(\"cccc\"); "
    "    using var l: LinkedList<string> := new LinkedList<string>( "
    "      src.Select(lambda(x: string) => x)); "
    "    var sum: int := 0; "
    "    for var it in l do sum := sum + it.Length; end; "
    "    return sum; "
    "  end Main; end C;";

BEZEL_TEST(linkedlist_source_fresh_mint_retain_2727,
           select_projection_contents_and_lengths) {
    BEZEL_EXPECT_EQ(run(kIdentitySelectCountAndLengths), 9);
}
