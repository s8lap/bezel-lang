// Codegen / silent misread — fix #3662 (§29.2 / §28.9 / §4.8, v0.286):
// `ensureIEnumViewFactory` — the nested-covariant-view factory that mints a
// per-shape enumerator wrapping `Array<X>`/`Stack<X>` as an `IEnumerable<X>`
// view (used when the SOURCE is itself array/stack-backed and the TARGET
// element is itself a pure `IEnumerable<T>` view, e.g. a doubly-nested
// `Array<Array<T>>` -> `IEnumerable<IEnumerable<T>>` coercion, #1850 / #2487)
// — keyed its LEAF-branch factory name, and its NESTED-branch internal
// Dispose/MoveNext/Current/GetEnumerator/vtable prefix, on the same
// element-name-collapsing spelling as the array family #3570 and the stack
// family #3661:
//
//     std::string elemName = elemTy->isIntegerTy()
//         ? ("i" + std::to_string(elemTy->getIntegerBitWidth()))
//         : (elemTy->isPointerTy() ? "ptr" : "f");
//
// THIS ISSUE'S PREMISE, AS FILED, DOES NOT HOLD (see LANE_RULES §1: "your
// brief is a hypothesis too"). #3662 was filed claiming the LEAF branch
// (`bz$viewfac$P$…` / `bz$viewfac$SP$…`) was "transitively fixed" by #3570 —
// because it delegates to `ensureArrayEnumMachinery`/`ensureStackEnumMachinery`,
// which #3570/#3661 already keyed correctly — and that only the NESTED
// branch (`bz$viewfac$N$…` / `bz$viewfac$SN$…`) still carried the residual
// `"f"` collapse. MEASURED on af4f73b (post-#3661, pre-this-fix): that is
// backwards on BOTH counts.
//
//   * The LEAF branch's OWN factory-name cache lookup
//     (`module_->getFunction(name)`) runs BEFORE the (correctly-keyed)
//     `ensureArrayEnumMachinery`/`ensureStackEnumMachinery` call — so on a
//     cache HIT (the second struct type reusing the first's collapsed
//     `"f"`-keyed name), the correctly-keyed inner machinery is never even
//     reached for the second type; the whole factory body, including its
//     baked-in vtable/struct/esz, is the FIRST creator's. Fixing #3570/#3661
//     alone does NOT transitively fix this call site.
//   * The NESTED branch's OWN `elemName` is `lowerTypeRef(viewElem)`, and
//     `viewElem` at that recursion level is BY CONSTRUCTION always itself an
//     `IEnumerable<T>` interface reference (`isPureIEnumView` requires
//     `!t->declaration`, true of every interface type) — so `elemTy` is
//     ALWAYS a pointer there, and `elemName` is ALWAYS `"ptr"`, regardless
//     of what the innermost element type is. The nested branch's internal
//     `prefix` (driving `Dispose`/`MoveNext`/`Current`/`GetEnumerator`/
//     `vtable` names) therefore was NEVER discriminated by struct identity
//     in the first place — sharing `bz$arrenumnest$ptr$Current` across EVERY
//     nested shape in a module, whose baked-in `innerFac` call target
//     (`Current`'s body calls `innerFac` directly, a specific function
//     pointer closed over at first creation) is the FIRST shape's.
//
// So the fix touches BOTH call sites: the LEAF branch's `elemName` now
// routes through the shared `enumMachineryElementName` helper (the #3570 /
// #3661 pattern), which also fixes the LEAF's downstream call into
// `ensureArrayEnumMachinery`/`ensureStackEnumMachinery` (now reachable with a
// correctly-differentiated name). The NESTED branch's `prefix` is re-keyed
// on `innerFac`'s own (now correctly per-shape-unique) name instead of the
// always-`"ptr"` `elemName` — the same discriminator the factory's own outer
// `name` variable already used. `stName` (the LLVM struct TYPE name) is left
// keyed on `elemName`/`"ptr"` deliberately: the 5-field
// `{vtable,arr,idx,esz,curInner}` struct LAYOUT is identical regardless of
// the inner shape, so sharing that type is harmless — only the FUNCTION
// identity (which bakes a specific `innerFac` call target) needed the fix.
//
// THE FAILURE MODE. A doubly-nested view over `Array<Point>` (8 B element)
// and a doubly-nested view over `Array<BigPoint>` (16 B element) in one
// module, both iterated. Pre-fix: SIGSEGV inside `bz$arrenum$f8$Current`
// (gdb) — the SECOND (`BigPoint`) nested view's `Current` silently calls the
// FIRST (`Point`) shape's `innerFac`/inner machinery on a 16-byte-per-element
// backing array read at an 8-byte stride. Post-fix: correct per-shape sums.
//
// COVERAGE.
//   (A) The repro, Point-view-first order: two doubly-nested views
//       (`Array<Array<Point>>` -> `IEnumerable<IEnumerable<Point>>`, and the
//       `BigPoint` twin) in one module, summed. Pre-fix: SEGFAULT. Post-fix:
//       t=212 (11 + 201).
//   (B) The reverse order — `BigPoint` (wider) view minted first. Pre-fix:
//       also SEGFAULT (order-independence of the collision, like #3661's
//       pair). Post-fix: t=212.
//   (C) Single `Array<Array<Point>>` doubly-nested view, control — no second
//       inner shape in the module, no collision surface. Post-fix identical
//       to pre-fix: t=11 (1 + 10).
//   (D) Single `Array<Array<int>>` doubly-nested view, integer-element
//       control (matches #1850's own multi-row test shape) — the `"i32"`
//       leaf key is unaffected by the helper swap. t=6 (1+2+3, one row).
//
// ORACLE. `runAot` — the pre-fix symptom is a SEGFAULT on the plain build
// (verified via gdb: `bz$arrenum$f8$Current`), so per LANE_RULES §R10-G a
// crash-pinning row asserts the disjunction of "died abnormally" shapes
// (non-`ran`, or a non-zero exit / signal death) rather than `WIFSIGNALED`
// alone — ASan installs its own fault handler and `_exit()`s rather than
// dying by signal.
//
// Spec: §29.2 (for-in desugaring; the doubly-nested for-in that reaches
// `Current`), §28.9 (runtime-managed types — `IEnumerable<T>`'s vtable-based
// dispatch shape), stdlib §4.8 (cited per #3570/#3661's own citation set for
// this enumerable-machinery cache-key family). Revision v0.286.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// Two doubly-nested views in one module, Point (8 B leaf) first: each is
// `Array<Array<T>>` assigned to `IEnumerable<IEnumerable<T>>` (the #1850
// nested-covariance shape) and drained with a doubly-nested for-in, summing
// the X field. Pre-fix: SIGSEGV. Post-fix: t=212 (11 + 201).
const char* const kTwoNestedViewsRepro =
    "module M; "
    "public record Point(X: int, Y: int); "
    "public record BigPoint(X: int, Y: int, Z: int, W: int); "
    "public class Main "
    "  public static method Main(): int "
    "    var t := 0; "
    "    var innerA: Array<Point> := [new Point(X := 1, Y := 2), new Point(X := 10, Y := 20)]; "
    "    var outerA: Array<Array<Point>> := [innerA]; "
    "    var viewA: IEnumerable<IEnumerable<Point>> := outerA; "
    "    for var row in viewA do "
    "      for var p in row do t := t + p.X; end for; "
    "    end for; "
    "    var innerB: Array<BigPoint> := [new BigPoint(X := 100, Y := 200, Z := 300, W := 400), new BigPoint(X := 101, Y := 201, Z := 301, W := 401)]; "
    "    var outerB: Array<Array<BigPoint>> := [innerB]; "
    "    var viewB: IEnumerable<IEnumerable<BigPoint>> := outerB; "
    "    for var row in viewB do "
    "      for var p in row do t := t + p.X; end for; "
    "    end for; "
    "    Console.WriteLine($\"t={t}\"); "
    "    return 0; "
    "  end Main; "
    "end Main;";

// Same two views, REVERSE order (BigPoint, the wider 16 B leaf, minted
// first). Pre-fix: also SEGFAULT — the collision is order-independent.
// Post-fix: t=212.
const char* const kTwoNestedViewsReproReversed =
    "module M; "
    "public record Point(X: int, Y: int); "
    "public record BigPoint(X: int, Y: int, Z: int, W: int); "
    "public class Main "
    "  public static method Main(): int "
    "    var t := 0; "
    "    var innerB: Array<BigPoint> := [new BigPoint(X := 100, Y := 200, Z := 300, W := 400), new BigPoint(X := 101, Y := 201, Z := 301, W := 401)]; "
    "    var outerB: Array<Array<BigPoint>> := [innerB]; "
    "    var viewB: IEnumerable<IEnumerable<BigPoint>> := outerB; "
    "    for var row in viewB do "
    "      for var p in row do t := t + p.X; end for; "
    "    end for; "
    "    var innerA: Array<Point> := [new Point(X := 1, Y := 2), new Point(X := 10, Y := 20)]; "
    "    var outerA: Array<Array<Point>> := [innerA]; "
    "    var viewA: IEnumerable<IEnumerable<Point>> := outerA; "
    "    for var row in viewA do "
    "      for var p in row do t := t + p.X; end for; "
    "    end for; "
    "    Console.WriteLine($\"t={t}\"); "
    "    return 0; "
    "  end Main; "
    "end Main;";

// Single doubly-nested `Point` view, control — no second inner shape in the
// module, so the LEAF/NESTED factory names have no collision surface.
// Pre-fix and post-fix identical: t=11 (1 + 10).
const char* const kSinglePointView =
    "module M; "
    "public record Point(X: int, Y: int); "
    "public class Main "
    "  public static method Main(): int "
    "    var t := 0; "
    "    var innerA: Array<Point> := [new Point(X := 1, Y := 2), new Point(X := 10, Y := 20)]; "
    "    var outerA: Array<Array<Point>> := [innerA]; "
    "    var viewA: IEnumerable<IEnumerable<Point>> := outerA; "
    "    for var row in viewA do "
    "      for var p in row do t := t + p.X; end for; "
    "    end for; "
    "    Console.WriteLine($\"t={t}\"); "
    "    return 0; "
    "  end Main; "
    "end Main;";

// Single doubly-nested `int` view, integer-element control (mirrors #1850's
// own multi-row int test). Cache key post-fix: `"i32"`; pre-fix: `"i32"` —
// identical on both sides. t=6 (1+2+3, one row).
const char* const kSingleIntView =
    "module M; "
    "public class Main "
    "  public static method Main(): int "
    "    var t := 0; "
    "    var innerA: Array<int> := [1, 2, 3]; "
    "    var outerA: Array<Array<int>> := [innerA]; "
    "    var viewA: IEnumerable<IEnumerable<int>> := outerA; "
    "    for var row in viewA do "
    "      for var x in row do t := t + x; end for; "
    "    end for; "
    "    Console.WriteLine($\"t={t}\"); "
    "    return 0; "
    "  end Main; "
    "end Main;";

}  // namespace

// ---------------------------------------------------------------------------
// (A) The repro — Point-view-first order.
//   per §29.2 (doubly-nested for-in desugaring), §28.9, stdlib §4.8
// ---------------------------------------------------------------------------
BEZEL_TEST(ienumview_nested_struct_element_cache_key_3662,
           two_nested_views_in_one_module_sum_their_values_29_2) {
    JitResult r = runAot(kTwoNestedViewsRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "t=212"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "t=11"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (B) The reverse order — wider struct's view minted first.
//   per §29.2, §28.9, stdlib §4.8
// ---------------------------------------------------------------------------
BEZEL_TEST(ienumview_nested_struct_element_cache_key_3662,
           two_nested_views_in_reverse_order_still_sum_29_2) {
    JitResult r = runAot(kTwoNestedViewsReproReversed);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "t=212"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "t=11"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (C) Single doubly-nested Point view — control.
//   per §29.2, stdlib §4.8
// ---------------------------------------------------------------------------
BEZEL_TEST(ienumview_nested_struct_element_cache_key_3662,
           single_nested_point_view_is_unchanged_29_2) {
    JitResult r = runAot(kSinglePointView);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "t=11"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (D) Single doubly-nested int view — the integer-element control.
//   per §29.2, stdlib §4.8
// ---------------------------------------------------------------------------
BEZEL_TEST(ienumview_nested_struct_element_cache_key_3662,
           single_nested_int_view_is_unchanged_29_2) {
    JitResult r = runAot(kSingleIntView);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "t=6"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
