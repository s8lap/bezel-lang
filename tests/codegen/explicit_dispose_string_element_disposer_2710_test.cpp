// Codegen / disposal — fix #2710 (spec §28.2 / §28.9 / §28.11, v0.240):
// the EXPLICIT `.Dispose()` lowering on a borrowing collection of `string`
// (and the `#2299` return-coercion free path that hands the source backing
// to `bz_array_free` / `bz_llist_free` / `bz_dict_free` after a wrap) both
// passed a NULL element disposer. A borrowing `string` element is a
// "runtime-managed reference" (§28.11 v0.135 table — RuntimeManaged row, no
// ownable backing), and the construction side ALREADY retains its +1 on every
// insert path (`litIsStrArr` in `materializeHeapCollectionLiteral`,
// `llitIsStrElem` in the LinkedList literal arm, the `.Add*` arms in
// `LlvmBackend_Calls.cpp`/`LlvmBackend_Calls3.cpp`). The disposal half was
// the one missing the matched release.
//
// THE BUG SURFACE (issue #2710 body, copied verbatim from the family framing).
// Three sites remained NULL-disposer after #2692 swept the other ten:
//
//   Site 1 — explicit `.Dispose()` on a borrowing collection of string:
//     * LinkedList arm of the disposal chain (`LlvmBackend_Calls2.cpp`,
//       the `isLinkedListTypeName` arm of the explicit-Dispose lowering).
//     * Array / Stack / Queue arm of the disposal chain (`LlvmBackend_Calls2.cpp`,
//       the `isCollectionTypeName` arm). The `IList<T>` interface arm is the
//       same array-backed lowering and was fixed in the same shape.
//     Both derived the disposer as `elementPolicyFor(elem, isOwning).disposerThunk`
//     with NO string fallback. `elementPolicyFor` returns a NULL disposer for a
//     borrowing-family `string` (the RuntimeManaged row carries retainRelease=true
//     and no thunk), so an explicit `d.Dispose()` on `Array<string>` or
//     `LinkedList<string>` freed the backing and leaked every element's +1.
//
//   Site 2 — the `#2299` IEnumerable return-coercion free. The three helpers
//     `freeCollectionSourceNow`, `freeDictSourceNow`, `freeLinkedListSourceNow`
//     in `LlvmBackend_Assign.cpp` hardcoded `ConstantPointerNull::get(ptrTy())`
//     as the element disposer. So returning an owned `Array<string>` /
//     `LinkedList<string>` coerced to `IEnumerable<string>` freed the backing
//     at the return-coercion free and leaked every element's +1.
//
// THE PAIRING PROOF that licenses the fix (mirrors the pairing proof in
// `materializeHeapCollectionLiteral` and the #2692 / #2686 release-and-retain
// sweep). The construction paths above are the ONLY ways an element enters a
// borrowing `Array<string>` / `LinkedList<string>`; the runtime exposes no
// other insert for the borrowing family, and codegen has no other construction
// path. So every element of a borrowing `Array<string>` / `LinkedList<string>`
// carries EXACTLY ONE list-owned reference — retained at construction — and a
// release that runs once per element consumes exactly that one.
//
// THE FIX SHAPE.
//   * Site 1 — derive `arrIsStringElem` / `llIsStringElem` (RuntimeManaged +
//     non-nullable string primitive — the verbatim predicate used by every
//     other retain/release site in `LlvmBackend_Calls.cpp` and
//     `LlvmBackend_Calls3.cpp`). If `elementPolicyFor` returns null and the
//     predicate is true, fall back to `stringElementReleaseThunk()`. This is
//     the same one-liner the `.RemoveAt`/`.Remove`/`.Clear` arms already carry.
//   * Site 2 — change the three `freeXxxSourceNow` helpers to take a
//     `const sema::TypeRef*` and derive the disposer from the element type
//     using the same predicate. The four call sites in
//     `wrapAsIEnumerableForTarget` already have `actualTy` in scope and thread
//     it through. (`freeDictSourceNow` is unchanged — Set's string keys are
//     released by `bz_set_string_keys_release` (Disposal.cpp:4819), a separate
//     mechanism out of #2710's stated scope; see the in-source note at
//     `LlvmBackend_Assign.cpp`.)
//
// OBSERVABILITY. `runAotChecked` links and runs the generated code under
// ASan/LSan, so a missing release flips `r.ran` to false. To stay under the
// 8-bit WEXITSTATUS truncation that bites an observable outside 0..255
// (aot_support.hpp "8-bit exit truncation" note, runAot vs runAot doc, also
// the run-cursor `runet-vs-exit` recall), every observable below is moved to
// stdout: each program prints `HITS <sum>` and the test parses
// `r.stdoutText`. As in #2692: every string is built at runtime with
// interpolation (`$"v-{i}"`) because `"ab" + "cd"` constant-folds to an
// rc-sentinel and `bz_string_release` is a no-op on a sentinel, and every
// string is passed as a borrowed PARAMETER so the pre-existing #422
// local-escape leak cannot contaminate the measurement.
//
// CONFIDENCE. The issue body marks this as "inferred from code shape, not
// executed" — the leak is bounded (one +1 per element, NOT unsoundness) and
// the fix is the symmetric counterpart to the retain paths already in place.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// Parse a trailing "HITS <number>" line from `r.stdoutText`. Returns -1 if
// not found. The program prints the sum as the LAST line, after any noise
// from the runtime / ASan net; we anchor on the trailing substring.
long parseHits(const std::string& text) {
    auto pos = text.rfind("HITS ");
    if (pos == std::string::npos) return -1;
    return std::strtol(text.c_str() + pos + 5, nullptr, 10);
}

// Each program runs the scenario many times so LSan sees the leak with
// confidence (per-iteration leak is small and LSan has been observed to drop
// a single +1 in cold-run cases on this toolchain). Counts are picked so the
// final `HITS` value comfortably fits in 0..255.

// ── Site 1 — explicit `.Dispose()` on `Array<string>` ──────────────────────

// A borrowing `Array<string>` is populated, then `.Dispose()` is called
// explicitly (NOT through a `using var`, which would route through the
// already-fixed `registerUsingDisposal`). The collection is NOT touched
// afterwards — only its element-counting legibility is asserted before the
// Dispose. Per iteration: 3 inserts + 1 dispose = 3 +1 leaks without the fix.
// Value: 50 iterations x 3 inserts = 150 (fits 8-bit).
const char* kArrayExplicitDisposeLeaksElements =
    "module M; import System; class C "
    "  public static method Three(s: string): int "
    "    var d := new Array<string>(); "
    "    d.Add(s); "
    "    d.Add(s); "
    "    d.Add(s); "
    "    var n := d.Count; "                          // 3
    "    d.Dispose(); "                               // ← the bug site
    "    return n; "                                  // pinned BEFORE dispose
    "  end Three; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..50 do "
    "      total := total + C.Three($\"v-{i}\"); "
    "    end; "
    "    Console.WriteLine($\"HITS {total}\"); "
    "    return 0; "
    "  end Main; "
    "end C;";

// ── Site 1 — explicit `.Dispose()` on `LinkedList<string>` ─────────────────

// Same shape, LinkedList. The release-half complement to #2686's retain. Per
// iteration: 4 inserts + 1 dispose = 4 +1 leaks without the fix.
// Value: 50 iterations x 4 inserts = 200 (fits 8-bit).
const char* kLinkedListExplicitDisposeLeaksElements =
    "module M; import System; class C "
    "  public static method Four(s: string): int "
    "    var d := new LinkedList<string>(); "
    "    d.AddLast(s); "
    "    d.AddLast(s); "
    "    d.AddFirst(s); "
    "    d.AddFirst(s); "
    "    var n := d.Count; "                          // 4
    "    d.Dispose(); "                               // ← the bug site
    "    return n; "                                  // pinned BEFORE dispose
    "  end Four; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..50 do "
    "      total := total + C.Four($\"v-{i}\"); "
    "    end; "
    "    Console.WriteLine($\"HITS {total}\"); "
    "    return 0; "
    "  end Main; "
    "end C;";

// ── Site 2 — return coercion of an owned `Array<string>` as `IEnumerable<string>` ─

// A method whose declared return is `IEnumerable<string>` returns a fresh
// `Array<string>` (the `#2299` shape). The wrap site frees the source via
// `freeCollectionSourceNow(v)`, which previously passed a NULL disposer — so
// every element's +1 is leaked. With the fix the helper derives the
// disposer from the element type (RuntimeManaged row → stringElementReleaseThunk).
// Per iteration: 2 inserts at construction = 2 +1 leaks without the fix.
// Value: 40 iterations x (3 + 3) = 240 (fits 8-bit). With Seq.Id ranging 0..39,
// "a-N" / "b-N" are length 3 for N<10 (10 iterations) and length 4 for N>=10
// (30 iterations): total = 10*6 + 30*8 = 60 + 240 = 300 (also fits 8-bit).
const char* kArrayReturnCoercionLeaksElements =
    "module M; import System; "
    "public class Seq public static Id: int := 0; end Seq; "
    "class C "
    "  public static method SeqMethod(): IEnumerable<string> "
    "    var s := new Array<string>(); "
    "    s.Add($\"a-{Seq.Id}\"); "
    "    s.Add($\"b-{Seq.Id}\"); "
    "    return s; "
    "  end SeqMethod; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    var i: int := 0; "
    "    while i < 40 do "
    "      Seq.Id := i; "
    "      for var it in C.SeqMethod() do total := total + it.Length; end; "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine($\"HITS {total}\"); "
    "    return 0; "
    "  end Main; "
    "end C;";

// ── Site 2 — return coercion of an owned `LinkedList<string>` as `IEnumerable<string>` ─
//
// DELIBERATELY NOT PINNED HERE. The LinkedList return-coercion path
// (wrapAsIEnumerableForTarget's LinkedList arm, `wrapLinkedListAsIEnumerable`
// + `freeLinkedListSourceNow` at LlvmBackend_Assign.cpp) is a pre-existing
// UAF: the BORROWING wrapper stores the source `list` pointer in slot 1, and
// the caller's iteration walks `bz_llist_first_node` / `bz_llist_node_next`
// on the SAME backing that `freeLinkedListSourceNow` freed immediately after
// the wrap was returned. The minimal reproducer SEGV's under ASan on the
// OWNING `string` release path this fix would install — but the bug is
// independent of the disposer argument (the original null-disposer code also
// freed the backing pre-iteration), so adding the string release makes the
// SEGV deterministic rather than introducing it. The structural remedy is to
// route the LinkedList return coercion through `wrapOwnedLinkedListAsIEnumerable`
// (the OWNING sibling, already used for the local-decl-init path — see
// LlvmBackend_Setup.cpp:12960) so the wrapper's slot-1 Dispose frees the
// list at the caller's scope-end, AFTER iteration completes. That is a
// separate change beyond #2710's disposer-argument scope — filed as a
// follow-up; the explicit-Dispose site (Site 1) and the Array return-
// coercion site (Site 2) are both fully covered by this fix.

}  // namespace

// §28.2 / §28.9 / §28.11 v0.240 — an explicit `Array<string>.Dispose()` releases
// every retained element's +1 (the explicit-Dispose arm of
// `LlvmBackend_Calls2.cpp`'s `isCollectionTypeName` chain).
BEZEL_TEST(codegen_explicit_dispose_string_disposer_2710,
           array_explicit_dispose_releases_elements_2710) {
    JitResult r = runAotChecked(kArrayExplicitDisposeLeaksElements);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(parseHits(r.stdoutText), 150);
}

// §28.2 / §28.9 / §28.11 v0.240 — an explicit `LinkedList<string>.Dispose()`
// releases every retained element's +1 (the explicit-Dispose arm of
// `LlvmBackend_Calls2.cpp`'s `isLinkedListTypeName` chain).
BEZEL_TEST(codegen_explicit_dispose_string_disposer_2710,
           linkedlist_explicit_dispose_releases_elements_2710) {
    JitResult r = runAotChecked(kLinkedListExplicitDisposeLeaksElements);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(parseHits(r.stdoutText), 200);
}

// §28.2 / §28.9 / §28.11 v0.240 — the `#2299` Array→IEnumerable return-coercion
// free (`freeCollectionSourceNow` in `LlvmBackend_Assign.cpp`, paired with the
// OWNING wrapper's slot-1 Dispose now carrying `stringElementReleaseThunk`)
// releases the source's string elements, not just the backing. The LinkedList
// sibling is filed separately — see the comment above the
// `kLinkedListReturnCoercionLeaksElements` slot.
BEZEL_TEST(codegen_explicit_dispose_string_disposer_2710,
           array_return_coercion_releases_elements_2710) {
    JitResult r = runAotChecked(kArrayReturnCoercionLeaksElements);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(parseHits(r.stdoutText), 300);
}

// (LinkedList return-coercion test deliberately omitted — see the slot's
// docstring.)