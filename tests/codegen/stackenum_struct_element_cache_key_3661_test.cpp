// Codegen / silent misread — fix #3661 (§29.2 / §28.9 / §4.8, v0.286):
// `ensureStackEnumMachinery` and `wrapOwnedStackAsIEnumerator` keyed their
// cached struct type, `MoveNext`, `Current` and vtable on an **element-name
// string that collapsed every non-integer, non-pointer element type to the
// single letter `"f"`**:
//
//     std::string elemName = elemTy->isIntegerTy()
//         ? ("i" + std::to_string(elemTy->getIntegerBitWidth()))
//         : (elemTy->isPointerTy() ? "ptr" : "f");
//
// Every **struct** element type therefore shared one `bz$stackenum$f$…`
// family. `Current` additionally bakes the element stride `esz` in **as a
// constant** (computed per creation, then frozen into the emitted function
// — see `ensureStackEnumMachinery`'s `b.CreateCall(...,{arrPtr, idx,
// ConstantInt::get(i64(), esz)})`). So two different struct element types in
// one module get **one** `bz$stackenum$f$Current`, carrying the **first
// creator's** element type and stride. `Stack<Point>` is 8 bytes
// (`{X: int, Y: int}`); `Stack<BigPoint>` is 16 bytes (`{X, Y, Z, W}: int`).
// Whichever is minted first wins, and the other reads its entries at the
// wrong stride.
//
// This is the **stack-family twin** of #3570 (arrenum family). The shared
// `enumMachineryElementName` helper introduced by this lane covers both:
// #3570's three arrenum sites plus the two stack sites here. Sibling issues
// #3662 (ienumview) and #3663 (llenum) carry the same inline spelling; they
// stay unfixed this cycle and are slated for their own lanes — the helper
// already exists for them to switch to.
//
// THE FAILURE MODE. Two `Stack<Point>` / `Stack<BigPoint>` foreach coercions
// in one module, both iterated and summed. Pre-fix: SIGSEGV (`bz_array_ptr_at`
// dereferences a freed/wrong slot — `arrp=0x0`, `idx=<garbage>`, gdb trace
// `at bezel_runtime_collections.cpp:421`). Post-fix: `t=222` (101 + 100 +
// 11 + 10 in pop order; the reverse-order case also yields 222). There is
// **no IR-verifier backstop**: the call is made through the vtable slot with
// a locally constructed `FunctionType`, so LLVM sees a well-typed call site
// — silent misread rather than a verification failure.
//
// THE FIX. Extract a shared `enumMachineryElementName(llvm::Type*)` helper
// (declared in `LlvmBackend_impl.hpp`, defined in `LlvmBackend_Assign.cpp`
// next to the array twin's machinery) and replace the inline cache-key
// expression at all five sites that spelled it: `ensureArrayEnumMachinery`,
// `wrapOwnedArrayAsIEnumerable`, `wrapDictionaryEntryArrayAsIEnumerable`
// (the array family, #3570 — refactored to the helper for consistency), and
// `ensureStackEnumMachinery`, `wrapOwnedStackAsIEnumerator` (the stack
// family, #3661 — the actual fix). The helper broadens the `"f"` fallback
// to `"f<allocSize>"` so distinct struct strides get distinct machinery;
// integer / pointer kinds keep their discriminating `i<bitwidth>` / `"ptr"`
// segments.
//
// ORACLE. `runAot` — the pre-fix symptom is a SEGFAULT on both oracles, so
// #2633's "neither dominates" rule does not apply: the plain build's
// `(non-)exit` is the load-bearing indicator. Per LANE_RULES §R10-G, the
// crash-pinning rows assert the disjunction of `WIFSIGNALED(status)` and an
// abnormal `WIFEXITED(status)`, never `WIFSIGNALED` alone — ASan replaces
// the allocator AND installs its own fault handler, terminating via
// `_exit()` instead of by signal, so a SEGV-pinning row that ignores the
// ASan shape is silently wrong under `--full`.
//
// COVERAGE.
//   (A) The repro, narrow-mints-first order: two `Stack<Point>` /
//       `Stack<BigPoint>` coercions in one module, summed. Pre-fix:
//       SEGFAULT (status not 0). Post-fix: `t=222`.
//   (B) The reverse order — wider struct mints first, narrower second.
//       Pre-fix: also SEGFAULT. Post-fix: `t=222`. The order-matters-ness
//       is what distinguishes this from a benign "single-stack works".
//   (C) The standalone `Stack<Point>` coercion, control — a single struct
//       element type in the module, no cache-collision surface. Post-fix
//       identical to pre-fix (`t=21`), proving the helper did not regress
//       the simple shape and that the SEGFAULT in (A)/(B) is the cache-
//       collision defect rather than a Stack<Point> defect.
//   (D) The standalone `Stack<int>` control — integer element type. The
//       cache key for this is `i32`; pre-fix identical to post-fix.
//       Proves the helper's `i<bitwidth>` arm behaves the same as the
//       inline expression's integer branch.

#include <sys/wait.h>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// A `//` comment inside a C++ string literal holding Bezel source lexes as
// Bezel source and comments out the rest of the program; the test passes for
// the wrong reason. Bezel's own comment forms are `//`, `/* ... */` and `///`
// (spec §1.5, exhaustive) — none of those are used here.
bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// Two coercions in one module. Point mints first (the 8-byte struct wins the
// shared `bz$stackenum$f$Current`); BigPoint inherits the 8-byte stride —
// `Current` reads at 8-byte steps over a 16-byte-per-element array and
// dereferences past the end via `bz_array_ptr_at(arrp=0x0)` → SEGFAULT.
// Pre-fix: SIGSEGV / ASan `_exit(1)`. Post-fix: `t=222` (101 + 100 + 11 + 10
// in pop order).
const char* const kTwoStackRepro =
    "module M; "
    "public record Point(X: int, Y: int); "
    "public record BigPoint(X: int, Y: int, Z: int, W: int); "
    "public class Main "
    "  public static method AaaSmall(): IEnumerable<Point> "
    "    var s: Stack<Point> := [new Point(X := 10, Y := 20), new Point(X := 11, Y := 21)]; "
    "    return s; "
    "  end AaaSmall; "
    "  public static method ZzzWide(): IEnumerable<BigPoint> "
    "    var s: Stack<BigPoint> := [new BigPoint(X := 100, Y := 200, Z := 300, W := 400), new BigPoint(X := 101, Y := 201, Z := 301, W := 401)]; "
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

// Same two coercions, REVERSE order so the wider struct mints first. The
// wider stride (16) wins; the narrower (`Stack<Point>`, 8-byte) array is
// read at 16-byte stride — the iteration walks past the int entries and the
// wrapper's `arrp` slot ends up null from a freed / corrupted allocation,
// again `bz_array_ptr_at` dereferences it. Pre-fix: SEGFAULT. Post-fix:
// `t=222`.
const char* const kTwoStackReproReversed =
    "module M; "
    "public record Point(X: int, Y: int); "
    "public record BigPoint(X: int, Y: int, Z: int, W: int); "
    "public class Main "
    "  public static method ZzzWide(): IEnumerable<BigPoint> "
    "    var s: Stack<BigPoint> := [new BigPoint(X := 100, Y := 200, Z := 300, W := 400), new BigPoint(X := 101, Y := 201, Z := 301, W := 401)]; "
    "    return s; "
    "  end ZzzWide; "
    "  public static method AaaSmall(): IEnumerable<Point> "
    "    var s: Stack<Point> := [new Point(X := 10, Y := 20), new Point(X := 11, Y := 21)]; "
    "    return s; "
    "  end AaaSmall; "
    "  public static method Main(): int "
    "    var t := 0; "
    "    using var b := ZzzWide(); "
    "    for var y in b do t := t + y.X; end for; "
    "    using var a := AaaSmall(); "
    "    for var x in a do t := t + x.X; end for; "
    "    Console.WriteLine($\"t={t}\"); "
    "    return 0; "
    "  end Main; "
    "end Main;";

// Single-coercion control: one `Stack<Point>` coercion, no second struct
// element type in the module, so the cache has no collision surface.
// Pre-fix and post-fix identical: `t=21` (pop order: 11 + 10). The row that
// says the helper did not regress the simple shape and that the SEGFAULT
// in (A)/(B) is the cache-collision defect rather than a generic
// `Stack<Point>` defect.
const char* const kSmallOnly =
    "module M; "
    "public record Point(X: int, Y: int); "
    "public class Main "
    "  public static method Mk(): IEnumerable<Point> "
    "    var s: Stack<Point> := [new Point(X := 10, Y := 20), new Point(X := 11, Y := 21)]; "
    "    return s; "
    "  end Mk; "
    "  public static method Main(): int "
    "    var t := 0; "
    "    using var e := Mk(); "
    "    for var x in e do t := t + x.X; end for; "
    "    Console.WriteLine($\"t={t}\"); "
    "    return 0; "
    "  end Main; "
    "end Main;";

// Same shape, integer element type. Cache key post-fix: `i32`; pre-fix:
// `i32`. Identical on both sides: `t=6` (pop order: 3 + 2 + 1). Proves the
// helper's `i<bitwidth>` arm behaves the same as the inline expression's
// integer branch.
const char* const kIntOnly =
    "module M; "
    "public class Main "
    "  public static method Main(): int "
    "    var t := 0; "
    "    using var s: Stack<int> := [1, 2, 3]; "
    "    for var x in s do t := t + x; end for; "
    "    Console.WriteLine($\"t={t}\"); "
    "    return 0; "
    "  end Main; "
    "end Main;";

// Per LANE_RULES §R10-G: a crash-pinning row asserts the disjunction of
// "died by signal" and "abnormal exit". ASan replaces the allocator and
// installs its own fault handler; under `--full` the child terminates via
// `_exit()` rather than by signal, so `WIFSIGNALED` alone is silently
// wrong under ASan.
bool runAotAndExitedAbnormally(const char* src) {
    JitResult r = runAot(src);
    if (!r.ran) return true;            // compile-fail or run-fail -> abnormal
    if (r.exitCode != 0) return true;   // non-zero exit -> abnormal
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// (A) The issue's repro — narrow-mints-first order.
//   per §29.2 (RETURN coercion to IEnumerable<Stack<T>>), §28.9, stdlib §4.8
//
// Pre-fix on HEAD (1459f30): SIGSEGV (gdb trace `bz_array_ptr_at(arrp=0x0,
// idx=<garbage>) at bezel_runtime_collections.cpp:421`). The 8-byte
// `Stack<Point>` wins the shared `bz$stackenum$f` name, and the 16-byte
// `Stack<BigPoint>` array's iteration walks past the data buffer.
// Post-fix: `t=222`.
// ---------------------------------------------------------------------------
BEZEL_TEST(stackenum_struct_element_cache_key_3661,
           two_stack_coercions_in_one_module_sum_their_values_29_2) {
    JitResult r = runAot(kTwoStackRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "t=222"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "t=21"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (B) The reverse order — wider struct mints first.
//   per §29.2, §28.9, stdlib §4.8
//
// Pre-fix on HEAD: also SIGSEGV — the wider stride (16) wins, and the
// narrower `Stack<Point>` array's iteration reads past the 8-byte-per-
// element data buffer. Either order exposes the same `f`-collapse.
// Post-fix: `t=222`.
// ---------------------------------------------------------------------------
BEZEL_TEST(stackenum_struct_element_cache_key_3661,
           two_stack_coercions_in_reverse_order_still_sum_29_2) {
    JitResult r = runAot(kTwoStackReproReversed);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "t=222"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "t=21"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (C) Single Stack<Point> coercion — control. No second struct element
//     type in the module, so the cache has no collision surface.
//   per §29.2, stdlib §4.8
//
// Pre-fix and post-fix identical: `t=21` (pop order: 11 + 10). The row
// that says the helper did not regress the simple shape and that the
// SEGFAULT in (A)/(B) is the cache-collision defect rather than a generic
// `Stack<Point>` defect.
// ---------------------------------------------------------------------------
BEZEL_TEST(stackenum_struct_element_cache_key_3661,
           single_stack_of_point_is_unchanged_29_2) {
    JitResult r = runAot(kSmallOnly);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "t=21"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (D) Single Stack<int> coercion — the integer-element control.
//   per §29.2, stdlib §4.8
//
// Cache key post-fix: `i32`. Pre-fix: `i32`. Identical on both sides:
// `t=6` (pop order: 3 + 2 + 1). Proves the helper's `i<bitwidth>` arm
// behaves the same as the inline expression's integer branch.
// ---------------------------------------------------------------------------
BEZEL_TEST(stackenum_struct_element_cache_key_3661,
           single_stack_of_int_is_unchanged_29_2) {
    JitResult r = runAot(kIntOnly);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "t=6"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
