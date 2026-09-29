// Codegen / silent misread — fix #3663 (§29.2 / §28.9 / §4.6 / §4.7, v0.286):
// `ensureLinkedListEnumMachinery` and `wrapOwnedLinkedListAsIEnumerable`
// (the brief/issue's "wrapOwnedLinkedListAsIEnumerator" is a naming
// mismatch — verified against HEAD; no `...AsIEnumerator` overload of this
// name exists, only `...AsIEnumerable`, added by #2987) keyed their cached
// struct type, `MoveNext`, `Current` and vtable on the same
// element-name-collapsing spelling as the array family #3570 and the stack
// family #3661:
//
//     std::string elemName = elemTy->isIntegerTy()
//         ? ("i" + std::to_string(elemTy->getIntegerBitWidth()))
//         : (elemTy->isPointerTy() ? "ptr" : "f");
//
// so two different struct element types in one module — e.g.
// `LinkedList<Point>` (8 B) and `LinkedList<BigPoint>` (16 B) — shared one
// `bz$llenum$f$Current`.
//
// THIS ISSUE IS SHARPER THAN ITS #3570/#3661 SIBLINGS, AND MEASURABLY SO
// (the issue's own suspicion, confirmed here rather than assumed). Every
// enumerable-machinery family's `Current` returns `elemTy` BY VALUE, but
// LinkedList's has no independent `esz`/stride signal the way the array
// families do — so the ONLY question is whether an allocSize-only key (the
// #3570/#3661/#3662 `enumMachineryElementName` pattern) is sufficient.
// MEASURED: it is NOT. Two struct element types of the SAME alloc size but
// a DIFFERENT LAYOUT — `record IntPair(X: int, Y: int)` (two ints, x86-64
// SysV ABI **INTEGER** class) and `record OneDouble(V: double)` (one
// double, SysV ABI **SSE** class) — both 8 bytes, sharing one
// `f8`-keyed `Current` under an allocSize-only fix: the SECOND type's
// for-in silently read the WRONG return-register class through the shared,
// first-creator-typed `Current`. Measured pre-strengthening: `sum=0`
// instead of `sum=300` — a **wrong value**, not a crash (both registers are
// architecturally valid, so nothing traps). Per LANE_RULES §6c ("loud ->
// quiet is a rejected trade"), narrowing this away was not an option; the
// fix instead uses a STRONGER key for this call site only — see
// `linkedListEnumElementName`'s declaration (LlvmBackend_impl.hpp) and
// definition (LlvmBackend_Assign.cpp) for why the struct's own LLVM type
// name (`recordStruct`/`classStruct` always mint a NAMED, per-declaration-
// unique `llvm::StructType`) closes this at zero extra bookkeeping, and why
// the shared `enumMachineryElementName` helper's contract for its OTHER six
// call sites is left unchanged (LANE_RULES §R12: do not widen a shared
// helper's contract without stating why) rather than folding this
// strengthening into it.
//
// THE SAME LATENT GAP LIKELY EXISTS AT THE OTHER SIX ENUMERABLE-MACHINERY
// SITES TOO (arrenum/stackenum/dictentry-arrenum/viewfac-leaf — their
// `Current` also returns `elemTy` by value) BUT IS UNMEASURED HERE AND OUT
// OF SCOPE for this lane (this lane owns `LlvmBackend_Assign.cpp`'s
// LinkedList sites only, per its brief) — filed separately as a follow-up
// issue; see this fix's commit body for the issue number.
//
// THE FAILURE MODE, allocSize-only key (NOT what ships in this fix, but
// what a naive #3570-pattern port would have produced — kept here as the
// measured justification, not a shipped intermediate state): two
// `LinkedList<IntPair>` / `LinkedList<OneDouble>` `IEnumerable<T>`
// coercions in one module, both iterated. `IntPair` (first creator) reads
// correctly; `OneDouble`'s reads as 0.
//
// ORACLE. `runAot` — the pre-fix (bare-`"f"`-collapse) symptom for the
// DIFFERENT-size pairing is the #3570/#3661 shape (wrong stride, typically
// a crash or a garbage value); the SAME-size/different-LAYOUT pairing's
// pre-fix symptom is a WRONG VALUE on a clean exit (not a crash), so those
// rows assert the printed value directly rather than a crash signature —
// no WIFSIGNALED-shaped row is needed here (LANE_RULES §R10-G / §R12-G
// noted for completeness: not applicable to this file's rows).
//
// COVERAGE.
//   (A) Plain (borrowing) wrap — `LinkedList<Point>` / `LinkedList<BigPoint>`
//       widened to `IEnumerable<T>` locals, both iterated, DIFFERENT sizes
//       (8 B / 16 B). t=222 (21 + 201... wait: (10+11) + (100+101) = 21 +
//       201 = 222).
//   (B) Same as (A), reverse order (BigPoint first) — order-independence.
//   (C) Plain wrap — SAME alloc size (8 B), DIFFERENT layout: `IntPair`
//       (all-int) vs `OneDouble` (one double). The row that actually
//       distinguishes this fix from a naive allocSize-only port.
//   (D) OWNED wrap (`wrapOwnedLinkedListAsIEnumerable`) — the #3354
//       return-coercion shape (`new LinkedList<T>()` + `AddLast` + `return`
//       from a method declared `IEnumerable<T>`), DIFFERENT sizes.
//   (E) OWNED wrap — SAME alloc size, DIFFERENT layout (the (C) pairing,
//       through the owned/return-coercion path).
//   (F) Single `LinkedList<Point>` plain-wrap control — no collision
//       surface.
//   (G) Single `LinkedList<int>` plain-wrap control — the integer arm is
//       unaffected by the helper swap.
//
// Spec: §29.2 (for-in desugaring), §28.9 (runtime-managed types), stdlib
// §4.6 (`LinkedList<T>`) / §4.7 (`OwningLinkedList<T>` — cited for context;
// the OWNING WRAP under test here, `wrapOwnedLinkedListAsIEnumerable`,
// applies to a callee-owned `LinkedList<T>` return, per #3354/#2987, not
// only to `OwningLinkedList<T>` sources). Revision v0.286.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// (A) Plain wrap, DIFFERENT sizes, Point (8 B) first.
const char* const kPlainDiffSize =
    "module M; "
    "public record Point(X: int, Y: int); "
    "public record BigPoint(X: int, Y: int, Z: int, W: int); "
    "public class Main "
    "  public static method Main(): int "
    "    var t := 0; "
    "    var a: LinkedList<Point> := [new Point(X := 10, Y := 20), new Point(X := 11, Y := 21)]; "
    "    var va: IEnumerable<Point> := a; "
    "    for var x in va do t := t + x.X; end for; "
    "    var b: LinkedList<BigPoint> := [new BigPoint(X := 100, Y := 200, Z := 300, W := 400), new BigPoint(X := 101, Y := 201, Z := 301, W := 401)]; "
    "    var vb: IEnumerable<BigPoint> := b; "
    "    for var y in vb do t := t + y.X; end for; "
    "    Console.WriteLine($\"t={t}\"); "
    "    return 0; "
    "  end Main; "
    "end Main;";

// (B) Plain wrap, DIFFERENT sizes, reverse order (BigPoint first).
const char* const kPlainDiffSizeReversed =
    "module M; "
    "public record Point(X: int, Y: int); "
    "public record BigPoint(X: int, Y: int, Z: int, W: int); "
    "public class Main "
    "  public static method Main(): int "
    "    var t := 0; "
    "    var b: LinkedList<BigPoint> := [new BigPoint(X := 100, Y := 200, Z := 300, W := 400), new BigPoint(X := 101, Y := 201, Z := 301, W := 401)]; "
    "    var vb: IEnumerable<BigPoint> := b; "
    "    for var y in vb do t := t + y.X; end for; "
    "    var a: LinkedList<Point> := [new Point(X := 10, Y := 20), new Point(X := 11, Y := 21)]; "
    "    var va: IEnumerable<Point> := a; "
    "    for var x in va do t := t + x.X; end for; "
    "    Console.WriteLine($\"t={t}\"); "
    "    return 0; "
    "  end Main; "
    "end Main;";

// (C) Plain wrap, SAME alloc size (8 B), DIFFERENT layout: `IntPair`
// (all-integer fields, SysV INTEGER class) vs `OneDouble` (one `double`
// field, SysV SSE class). This is the row an allocSize-only key (the naive
// #3570-pattern port) gets WRONG: `sum` reads 0 instead of 300.0 because
// the shared `f8`-keyed `Current` returns whichever class the FIRST
// creator's struct classified as.
const char* const kPlainSameSizeDifferentLayout =
    "module M; "
    "public record IntPair(X: int, Y: int); "
    "public record OneDouble(V: double); "
    "public class Main "
    "  public static method Main(): int "
    "    var t := 0; "
    "    var a: LinkedList<IntPair> := [new IntPair(X := 10, Y := 20), new IntPair(X := 11, Y := 21)]; "
    "    var va: IEnumerable<IntPair> := a; "
    "    for var x in va do t := t + x.X; end for; "
    "    var b: LinkedList<OneDouble> := [new OneDouble(V := 100.0), new OneDouble(V := 200.0)]; "
    "    var vb: IEnumerable<OneDouble> := b; "
    "    var sum := 0.0; "
    "    for var y in vb do sum := sum + y.V; end for; "
    "    Console.WriteLine($\"t={t} sum={sum}\"); "
    "    return 0; "
    "  end Main; "
    "end Main;";

// (D) OWNED wrap (`wrapOwnedLinkedListAsIEnumerable`) — the #3354
// return-coercion shape, DIFFERENT sizes.
const char* const kOwnedDiffSize =
    "module M; "
    "public record Point(X: int, Y: int); "
    "public record BigPoint(X: int, Y: int, Z: int, W: int); "
    "public class Main "
    "  public static method AaaSmall(): IEnumerable<Point> "
    "    var s := new LinkedList<Point>(); "
    "    s.AddLast(new Point(X := 10, Y := 20)); "
    "    s.AddLast(new Point(X := 11, Y := 21)); "
    "    return s; "
    "  end AaaSmall; "
    "  public static method ZzzWide(): IEnumerable<BigPoint> "
    "    var s := new LinkedList<BigPoint>(); "
    "    s.AddLast(new BigPoint(X := 100, Y := 200, Z := 300, W := 400)); "
    "    s.AddLast(new BigPoint(X := 101, Y := 201, Z := 301, W := 401)); "
    "    return s; "
    "  end ZzzWide; "
    "  public static method Main(): int "
    "    var t := 0; "
    "    using var a := AaaSmall(); "
    "    for var x in a do t := t + x.X; end for; "
    "    using var b := ZzzWide(); "
    "    for var y in b do t := t + y.X; end for; "
    "    Console.WriteLine($\"t={t}\"); "
    "    return 0; "
    "  end Main; "
    "end Main;";

// (E) OWNED wrap, SAME alloc size, DIFFERENT layout — the (C) pairing
// through the return-coercion path.
const char* const kOwnedSameSizeDifferentLayout =
    "module M; "
    "public record IntPair(X: int, Y: int); "
    "public record OneDouble(V: double); "
    "public class Main "
    "  public static method MkInt(): IEnumerable<IntPair> "
    "    var s := new LinkedList<IntPair>(); "
    "    s.AddLast(new IntPair(X := 10, Y := 20)); "
    "    s.AddLast(new IntPair(X := 11, Y := 21)); "
    "    return s; "
    "  end MkInt; "
    "  public static method MkDouble(): IEnumerable<OneDouble> "
    "    var s := new LinkedList<OneDouble>(); "
    "    s.AddLast(new OneDouble(V := 100.0)); "
    "    s.AddLast(new OneDouble(V := 200.0)); "
    "    return s; "
    "  end MkDouble; "
    "  public static method Main(): int "
    "    var t := 0; "
    "    using var a := MkInt(); "
    "    for var x in a do t := t + x.X; end for; "
    "    var sum := 0.0; "
    "    using var b := MkDouble(); "
    "    for var y in b do sum := sum + y.V; end for; "
    "    Console.WriteLine($\"t={t} sum={sum}\"); "
    "    return 0; "
    "  end Main; "
    "end Main;";

// (F) Single LinkedList<Point> plain-wrap control — no collision surface.
const char* const kSinglePlainPoint =
    "module M; "
    "public record Point(X: int, Y: int); "
    "public class Main "
    "  public static method Main(): int "
    "    var t := 0; "
    "    var a: LinkedList<Point> := [new Point(X := 10, Y := 20), new Point(X := 11, Y := 21)]; "
    "    var va: IEnumerable<Point> := a; "
    "    for var x in va do t := t + x.X; end for; "
    "    Console.WriteLine($\"t={t}\"); "
    "    return 0; "
    "  end Main; "
    "end Main;";

// (G) Single LinkedList<int> plain-wrap control — integer arm unaffected.
const char* const kSinglePlainInt =
    "module M; "
    "public class Main "
    "  public static method Main(): int "
    "    var t := 0; "
    "    var a: LinkedList<int> := [1, 2, 3]; "
    "    var va: IEnumerable<int> := a; "
    "    for var x in va do t := t + x; end for; "
    "    Console.WriteLine($\"t={t}\"); "
    "    return 0; "
    "  end Main; "
    "end Main;";

}  // namespace

// ---------------------------------------------------------------------------
// (A) Plain wrap — different sizes, Point first.
//   per §29.2, §28.9, stdlib §4.6
// ---------------------------------------------------------------------------
BEZEL_TEST(llenum_struct_element_cache_key_3663,
           plain_wrap_two_sizes_in_one_module_sum_29_2) {
    JitResult r = runAot(kPlainDiffSize);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "t=222"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (B) Plain wrap — different sizes, reverse order.
//   per §29.2, §28.9, stdlib §4.6
// ---------------------------------------------------------------------------
BEZEL_TEST(llenum_struct_element_cache_key_3663,
           plain_wrap_two_sizes_reverse_order_29_2) {
    JitResult r = runAot(kPlainDiffSizeReversed);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "t=222"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (C) Plain wrap — SAME alloc size, DIFFERENT layout. The row an
//     allocSize-only key gets wrong (measured `sum=0` pre-strengthening).
//   per §29.2, §28.9, stdlib §4.6
// ---------------------------------------------------------------------------
BEZEL_TEST(llenum_struct_element_cache_key_3663,
           plain_wrap_same_size_different_layout_29_2) {
    JitResult r = runAot(kPlainSameSizeDifferentLayout);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "t=21"));
    // "sum=300", not "sum=3e+02": fix #5079 (stdlib §2.18, v0.307) corrected
    // double no-format ToString()'s shortest-round-trip rendering to prefer
    // fixed-point notation over scientific when it round-trips and is not
    // longer.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "sum=300"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "sum=0"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (D) OWNED wrap — different sizes, the #3354 return-coercion shape.
//   per §29.2, §28.9, stdlib §4.6 / §4.7
// ---------------------------------------------------------------------------
BEZEL_TEST(llenum_struct_element_cache_key_3663,
           owned_wrap_two_sizes_in_one_module_sum_29_2) {
    JitResult r = runAot(kOwnedDiffSize);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "t=222"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (E) OWNED wrap — SAME alloc size, DIFFERENT layout.
//   per §29.2, §28.9, stdlib §4.6 / §4.7
// ---------------------------------------------------------------------------
BEZEL_TEST(llenum_struct_element_cache_key_3663,
           owned_wrap_same_size_different_layout_29_2) {
    JitResult r = runAot(kOwnedSameSizeDifferentLayout);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "t=21"));
    // "sum=300", not "sum=3e+02": fix #5079 (stdlib §2.18, v0.307), see the
    // sibling row above.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "sum=300"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "sum=0"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (F) Single LinkedList<Point> plain wrap — control.
//   per §29.2, stdlib §4.6
// ---------------------------------------------------------------------------
BEZEL_TEST(llenum_struct_element_cache_key_3663,
           single_plain_point_is_unchanged_29_2) {
    JitResult r = runAot(kSinglePlainPoint);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "t=21"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (G) Single LinkedList<int> plain wrap — integer-element control.
//   per §29.2, stdlib §4.6
// ---------------------------------------------------------------------------
BEZEL_TEST(llenum_struct_element_cache_key_3663,
           single_plain_int_is_unchanged_29_2) {
    JitResult r = runAot(kSinglePlainInt);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "t=6"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
