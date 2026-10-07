// fix #4693 (§5, v0.303): ToDictionary raises ArgumentException on duplicate key.
// stdlib §5 specifies: "ToDictionary raises ArgumentException if keySelector
// produces a duplicate key." The previous implementation silently overwrote the
// earlier entry, producing a dict with fewer entries than source elements.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Plain program wrapper — no resource class.
std::string prog(const std::string& mainBody) {
    return "module M; class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

int run(const std::string& p) {
    auto r = runAot(p);
    return r.ran ? r.exitCode : -999;
}

// A checked run is SKIPPED (not failed) when the ASan link toolchain is absent.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        // A toolchain skip and a real finding are DIFFERENT outcomes, and only
        // the former may be returned past. "runAotChecked: SKIP — C toolchain
        // (clang) ..." means the sanitizer never ran; "sanitizer fired" means it
        // ran and found something. Returning true for the second would discard
        // a genuine leak as if the box merely lacked clang — the #3733 silent
        // -skip defect, and the reason this predicate is not a bare find("SKIP").
        if (d.message.find("SKIP") == std::string::npos) continue;
        if (d.message.find("sanitizer fired") != std::string::npos) return false;
        return true;
    }
    return false;
}

// runAotChecked wrapper: returns exit code if ran, -999 if not ran.
// runAotChecked is the LSan-clean oracle; a leak → exit 1 → assertion fails.
// Handles the skip case (ASan runtime unavailable) the same way as wasSkipped.
int checkedExit(const std::string& p) {
    auto r = runAotChecked(p);
    if (wasSkipped(r)) return -999;
    return r.ran ? r.exitCode : -999;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// stdlib §5 — single-selector ToDictionary<K>(keySelector)
// Duplicate int key → ArgumentException
// ---------------------------------------------------------------------------

// fix #4693: duplicate int key raises ArgumentException.
// PRE-FIX: caught=0, d.Count=2, return = 0 (FAIL).
// POST-FIX: caught=1, d.Count=0, return = 8 + 4 = 12 (PASS).
BEZEL_TEST(todictionary_duplicate_key_raises_4693,
           single_selector_duplicate_int_key_raises_4_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "var arr: Array<int> := [1, 2, 1]; "
        "var caught := 0; "
        "var d: Dictionary<int, int> := new Dictionary<int, int>(); "
        "try "
        "  d := arr.ToDictionary(lambda(x: int) => x); "
        "catch "
        "  var e: ArgumentException => do caught := 1; end "
        "end try; "
        "return int(caught = 1) * 8 + int(d.Count = 0) * 4; ")), 12);
}

// fix #4693: distinct int keys build correct dict (no throw).
// Return: d.Count (= 3) → 3. Both pre-fix and post-fix give 3.
BEZEL_TEST(todictionary_duplicate_key_raises_4693,
           single_selector_distinct_int_keys_builds_dict_4_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "var arr: Array<int> := [10, 20, 30]; "
        "var d := arr.ToDictionary(lambda(x: int) => x); "
        "return d.Count; ")), 3);
}

// fix #4693: empty array produces empty dict (no throw).
BEZEL_TEST(todictionary_duplicate_key_raises_4693,
           single_selector_empty_array_4_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "var arr: Array<int> := []; "
        "var d := arr.ToDictionary(lambda(x: int) => x); "
        "return d.Count; ")), 0);
}

// fix #4693: single element produces dict of size 1 (no throw).
BEZEL_TEST(todictionary_duplicate_key_raises_4693,
           single_selector_single_element_4_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "var arr: Array<int> := [42]; "
        "var d := arr.ToDictionary(lambda(x: int) => x); "
        "return d.Count; ")), 1);
}

// ---------------------------------------------------------------------------
// stdlib §5 — single-selector ToDictionary<K>(keySelector)
// Duplicate string key → ArgumentException (string-key fast path)
// ---------------------------------------------------------------------------

// fix #4693: duplicate string key in single-selector overload raises ArgumentException.
// PRE-FIX: caught=0, return = 0 (FAIL).
// POST-FIX: caught=1, d.Count=0, return = 12 (PASS).
// NOTE: ToDictionary<K>(keySelector) returns Dictionary<K, TElement>, where TElement
// is the source array's element type. For Array<string> with keySelector returning
// string, the result is Dictionary<string, string>. Using var d := lets Bezel infer
// the correct type rather than a mismatched explicit annotation.
BEZEL_TEST(todictionary_duplicate_key_raises_4693,
           single_selector_duplicate_string_key_raises_4_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "var arr: Array<string> := [\"a\", \"b\", \"a\"]; "
        "var caught := 0; "
        "var d: Dictionary<string, string> := new Dictionary<string, string>(); "
        "try "
        "  d := arr.ToDictionary(lambda(s: string) => s); "
        "catch "
        "  var e: ArgumentException => do caught := 1; end "
        "end try; "
        "return int(caught = 1) * 8 + int(d.Count = 0) * 4; ")), 12);
}

// fix #4693: distinct string keys in single-selector overload build correct dict.
BEZEL_TEST(todictionary_duplicate_key_raises_4693,
           single_selector_distinct_string_keys_builds_dict_4_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "var arr: Array<string> := [\"a\", \"b\", \"c\"]; "
        "var d := arr.ToDictionary(lambda(s: string) => s); "
        "return d.Count; ")), 3);
}

// ---------------------------------------------------------------------------
// stdlib §5 — two-selector ToDictionary<K, V>(keySelector, valueSelector)
// Duplicate int key → ArgumentException
// ---------------------------------------------------------------------------

// fix #4693: duplicate int key in two-selector overload raises ArgumentException.
// Note: two-selector overload only supports int-typed value selectors (#747).
// PRE-FIX: caught=0, d.Count=2, return = 0 (FAIL).
// POST-FIX: caught=1, d.Count=0, return = 8 + 4 = 12 (PASS).
BEZEL_TEST(todictionary_duplicate_key_raises_4693,
           two_selector_duplicate_int_key_raises_4_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "var arr: Array<int> := [1, 2, 1]; "
        "var caught := 0; "
        "var d: Dictionary<int, int> := new Dictionary<int, int>(); "
        "try "
        "  d := arr.ToDictionary(lambda(x: int) => x, lambda(x: int) => x * 2); "
        "catch "
        "  var e: ArgumentException => do caught := 1; end "
        "end try; "
        "return int(caught = 1) * 8 + int(d.Count = 0) * 4; ")), 12);
}

// fix #4693: distinct keys in two-selector overload build correct dict (no throw).
// Return: d.Count (= 3) → 3. Both pre-fix and post-fix give 3.
BEZEL_TEST(todictionary_duplicate_key_raises_4693,
           two_selector_distinct_int_keys_builds_dict_4_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "var arr: Array<int> := [10, 20, 30]; "
        "var d := arr.ToDictionary(lambda(x: int) => x, lambda(x: int) => x * 2); "
        "return d.Count; ")), 3);
}

// ---------------------------------------------------------------------------
// stdlib §5 — two-selector ToDictionary<K, V>(keySelector, valueSelector)
// Duplicate string key → ArgumentException (string-key fast path)
// ---------------------------------------------------------------------------

// fix #4693: duplicate string key in two-selector overload raises ArgumentException.
// Note: two-selector overload only supports int-typed value selectors (#747).
// PRE-FIX: caught=0, d.Count=2, return = 0 (FAIL).
// POST-FIX: caught=1, d.Count=0, return = 8 + 4 = 12 (PASS).
BEZEL_TEST(todictionary_duplicate_key_raises_4693,
           two_selector_duplicate_string_key_raises_4_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "var arr: Array<string> := [\"x\", \"y\", \"x\"]; "
        "var caught := 0; "
        "var d: Dictionary<string, int> := new Dictionary<string, int>(); "
        "try "
        "  d := arr.ToDictionary(lambda(s: string) => s, lambda(s: string) => s.Length); "
        "catch "
        "  var e: ArgumentException => do caught := 1; end "
        "end try; "
        "return int(caught = 1) * 8 + int(d.Count = 0) * 4; ")), 12);
}

// fix #4693: distinct string keys in two-selector overload build correct dict.
// Note: two-selector overload only supports int-typed value selectors (#747).
// Return: d.Count (= 3) → 3. Both pre-fix and post-fix give 3.
BEZEL_TEST(todictionary_duplicate_key_raises_4693,
           two_selector_distinct_string_keys_builds_dict_4_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "var arr: Array<string> := [\"a\", \"b\", \"c\"]; "
        "var d := arr.ToDictionary(lambda(s: string) => s, lambda(s: string) => s.Length); "
        "return d.Count; ")), 3);
}

// ---------------------------------------------------------------------------
// stdlib §5 — LSan control: the throw path must not leak.
// 200 iterations of a throw-inducing call — one-shot does not reach LSan threshold.
// ---------------------------------------------------------------------------

// fix #4693: the duplicate-key throw path is LSan-clean (no leak).
// exitCode==0 + no LSan report = no leak.
BEZEL_TEST(todictionary_duplicate_key_raises_4693,
           throw_path_is_lsan_clean_4_5) {
    JitResult r = runAotChecked(prog(
        "var arr: Array<int> := [1, 2, 1]; "
        "var i := 0; "
        "while i < 200 do "
        "  try "
        "    var d := arr.ToDictionary(lambda(x: int) => x); "
        "  catch "
        "    var e: ArgumentException => do end "
        "  end; "
        "  i := i + 1; "
        "end while; "
        "return 0; "));
    // #3733: a silent `return` here would report a clean PASS on a box
    // whose ASan link is unavailable — green while asserting nothing about
    // memory safety. Invert the branch so the skip is loud, then skip.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// stdlib §5 — control: distinct keys, LSan-clean, correct output
// ---------------------------------------------------------------------------

BEZEL_TEST(todictionary_duplicate_key_raises_4693,
           distinct_keys_lsan_clean_correct_output_4_5) {
    JitResult r = runAotChecked(prog(
        "var arr: Array<int> := [10, 20, 30]; "
        "using var d := arr.ToDictionary(lambda(x: int) => x); "
        "Console.WriteLine($\"{d[10]},{d[20]},{d[30]}\"); "
        "return d.Count; "));
    // #3733: a silent `return` here would report a clean PASS on a box
    // whose ASan link is unavailable — green while asserting nothing about
    // memory safety. Invert the branch so the skip is loud, then skip.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ---------------------------------------------------------------------------
// fix #5421 (§28.7 / §28.9): drop-on-overwrite for collection locals.
// When a Dictionary/Array/LinkedList local is reassigned, the displaced value
// was never disposed.  The pre-fix compiler leaks the old dict on every
// duplicate-key throw (352 bytes: 96-byte header + 256-byte buckets).
// These rows use runAotChecked so LSan is active, and loop 200x so the
// one-shot leak clears Lsan's threshold.  The reassignment SHAPE is required:
// `var d := new Dictionary()` is OUTSIDE the try, so reassigning `d` inside
// displaces the pre-existing owner.  A fresh declaration inside the try
// (`var d := ...`) displaces nothing and cannot exercise drop-on-overwrite.
// ---------------------------------------------------------------------------

// fix #5421: Dictionary reassignment — single-selector ToDictionary.
// var d := new Dictionary() is OUTSIDE the try; d := ... inside the try
// displaces the pre-existing dict and must be dropped on throw.
// Uses type inference (var d := ...) since explicit type annotation
// (var d: Dictionary<int, int> := ...) is rejected by the ownership
// analysis even though semantically identical.
// PRE-FIX: LSan reports 70400-byte leak (200 * 352), exit 1.
// POST-FIX: exitCode==0, no LSan report.
BEZEL_TEST(todictionary_duplicate_key_raises_4693,
           dict_reassign_drop_on_overwrite_single_selector_5421) {
    BEZEL_EXPECT_EQ(checkedExit(prog(
        "var arr: Array<int> := [1, 2, 1]; "
        "var i := 0; "
        "while i < 200 do "
        "  var d := new Dictionary<int, int>(); "
        "  try "
        "    d := arr.ToDictionary(lambda(x: int) => x); "
        "  catch "
        "    var e: ArgumentException => do end "
        "  end try; "
        "  i := i + 1; "
        "end while; "
        "return 0; ")), 0);
}

// fix #5421: OwningArray reassignment — non-dictionary collection type.
// The fix is general over bz_array_free / bz_dict_free / bz_llist_free;
// cover at least one non-Dictionary collection to exercise the array arm.
// Array<T> has no duplicate-key throw, so this uses an array reassigned 200x:
// the displaced value must be dropped on every reassignment.
// Uses type inference (var a := ...) since explicit type annotation
// (var a: Array<int> := ...) is rejected by the ownership analysis.
// Pre-fix: the displaced OwningArray<int> value is leaked on every
// reassignment.  Post-fix: each displaced value is freed via bz_array_free.
BEZEL_TEST(todictionary_duplicate_key_raises_4693,
           array_reassign_drop_on_overwrite_5421) {
    BEZEL_EXPECT_EQ(checkedExit(prog(
        "var i := 0; "
        "while i < 200 do "
        "  var a := new Array<int>(10); "
        "  a := new Array<int>(20); "
        "  i := i + 1; "
        "end while; "
        "return 0; ")), 0);
}

// fix #5421: LinkedList reassignment — the bz_llist_free arm.
// The fix is general over bz_array_free / bz_dict_free / bz_llist_free;
// LinkedList<T> is covered by the linked-list arm of disposeDisplacedCollectionLocal.
// Like the array row, this uses a simple reassignment loop with no throw path:
// the displaced LinkedList value must be dropped on every reassignment.
// LinkedList<T> (borrowing form) is used; the `new` creates an owning
// LinkedList instance, which is what the local's inferred type captures.
// Pre-fix: the displaced LinkedList<int> value is leaked on every
// reassignment.  Post-fix: each displaced value is freed via bz_llist_free.
BEZEL_TEST(todictionary_duplicate_key_raises_4693,
           linkedlist_reassign_drop_on_overwrite_5421) {
    BEZEL_EXPECT_EQ(checkedExit(prog(
        "var i := 0; "
        "while i < 200 do "
        "  var ll := new LinkedList<int>(); "
        "  ll := new LinkedList<int>(); "
        "  i := i + 1; "
        "end while; "
        "return 0; ")), 0);
}
