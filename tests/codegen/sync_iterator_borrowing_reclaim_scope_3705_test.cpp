// Codegen / leak — issue #3705 (spec §28.9 "Runtime-managed types", §28.11
// "Collection element ownership", §29.7 "Disposal of partially-consumed
// iterators", §30.2 "`await`", v0.286): the SAVE/RESTORE half of the
// borrowing-collection reclaim re-establishment.
//
// `emitIteratorMethod` (fix #2148) and `emitAsyncMethod` (#2132/#2142/#2143)
// have both CLEARED and re-pinned the five collection-reclaim eligibility sets
// (+ their five fn tokens + the #2660 aggregate set) for the function their body
// lowers into for a long time. Neither ever grew the matching save/restore, and
// LANE_RULES §8 states the contract: a `clear()` on per-lowering state and a
// save/restore around nested emission are two halves of ONE. fix #3697 supplied
// the missing half for `emitAsyncIteratorMethod` only.
//
// Both emitters are reached MID-EMISSION of an outer body by
// `buildSpecializedMethodFn` (LlvmBackend_Calls2.cpp) whenever a GENERIC sync
// iterator / generic async method is monomorphized inside one. That site saves
// the unwind, string and closure-release sets; it saves neither this family nor
// the §27.5 capture family. So the outer body resumed lowering with
// `borrowingArrayFreeEligibleFn_` (and four analogues) still naming the
// SPECIALISATION's MoveNext/Resume, every one of its own gates in
// `lowerLocalDecl` went false, and the OUTER body's own reclaims were silently
// skipped.
//
//     $ bezelc --sanitize=address -o p p.bzl && ./p     # 200 rounds
//     total=2400
//     Direct leak of 11200 byte(s) in 200 object(s) ... bz_array_alloc
//     SUMMARY: AddressSanitizer: 24000 byte(s) leaked in 400 allocation(s).
//
// ── DIRECTION is a LEAK, and the printed value is UNAFFECTED on both sides, so
// LSan is the only oracle that sees this defect: every row here is
// `runAotChecked` (generated-code ASan with LeakSanitizer on) and no row may be
// weakened to a plain value check. Every row is loop-amplified to 200 rounds —
// a one-shot row reads clean on BOTH sides, because the single allocation is
// still reachable at exit.
//
// ── WHAT EACH ROW LOCALISES. The leaked bytes are always the OUTER body's own
// literal, never the specialisation's: rows 1-5 give the specialised callee a
// collection local of its own too, and it reclaims correctly on both sides.
// Rows 6-7 are the §27.5 capture family — the SECOND scope `emitAsyncMethod`
// was missing, which the #3500 comment on `CaptureReclaimSetScope` already
// named ("which `emitAsyncMethod` already trips over today") without an issue
// or a test. Row 8 is the non-generic control that must be clean on both sides.
// per §28.9, §28.11, §30.2 (issue #3705)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
// runAotChecked runs the program under generated-code ASan with LeakSanitizer
// on, so a leak, a double free or a use-after-free fails the row before the
// printed value is compared.
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

const char* kHdr =
    "module M; import System; import System.Collections.Generic; "
    "import System.Threading.Tasks; ";
}  // namespace

// ── Row 1, the issue's repro. A GENERIC SYNC iterator specialised from inside
// `Outer`, which then declares an `Array<int>` literal of its own. Pre-fix
// 24000 B / 400 allocations, ALL of it `Outer`'s `ys` backing (the
// specialisation's own `zs` reclaims on both sides). Value identical either way.
// 200 * ((2 + 7) + 3) = 2400. // per §28.11 (issue #3705)
BEZEL_TEST(sync_iterator_borrowing_reclaim_scope_3705,
           generic_sync_iterator_spec_restores_the_outer_bodys_tokens_spec_28_11) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Gen<T>(a: T, b: T): IEnumerable<int> "
        "    var zs: Array<T> := [a, b]; "
        "    yield return zs.Count; "
        "    yield return 7; "
        "  end Gen; "
        "  public static method Outer(): int "
        "    var t := 0; "
        "    for var v in C.Gen<int>(1, 2) do t := t + v; end; "
        "    var ys: Array<int> := [5, 6, 7]; "
        "    return t + ys.Count; "
        "  end Outer; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + C.Outer(); end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=2400\n");
}

// ── Row 2, the SET-LITERAL token (#2142). `setLiteralFreeEligibleFn_` is a
// separate one of the five keys the scope restores, so a partial restore that
// put back only the array key would still leak here.
// 200 * ((2 + 7) + 3) = 2400. // per §28.11 (issue #3705)
BEZEL_TEST(sync_iterator_borrowing_reclaim_scope_3705,
           generic_sync_iterator_spec_restores_the_set_literal_token_spec_28_11) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Gen<T>(a: T, b: T): IEnumerable<int> "
        "    var zs: Array<T> := [a, b]; "
        "    yield return zs.Count; "
        "    yield return 7; "
        "  end Gen; "
        "  public static method Outer(): int "
        "    var t := 0; "
        "    for var v in C.Gen<int>(1, 2) do t := t + v; end; "
        "    var ys: Set<int> := [5, 6, 7]; "
        "    return t + ys.Count; "
        "  end Outer; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + C.Outer(); end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=2400\n");
}

// ── Row 3, the DICT-LITERAL token (#2143) plus the #2660 aggregate set, whose
// consumer (LlvmBackend_Disposal.cpp) gates on `dictLiteralFreeEligibleFn_` —
// so the set and its guard must be restored together or the outer body's
// nested-literal roots consult the specialisation's set.
// 200 * ((2 + 7) + 2) = 2200. // per §28.11 (issue #3705)
BEZEL_TEST(sync_iterator_borrowing_reclaim_scope_3705,
           generic_sync_iterator_spec_restores_the_dict_literal_token_spec_28_11) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Gen<T>(a: T, b: T): IEnumerable<int> "
        "    var zs: Array<T> := [a, b]; "
        "    yield return zs.Count; "
        "    yield return 7; "
        "  end Gen; "
        "  public static method Outer(): int "
        "    var t := 0; "
        "    for var v in C.Gen<int>(1, 2) do t := t + v; end; "
        "    var ys: Dictionary<string, int> := [\"a\": 5, \"b\": 6]; "
        "    return t + ys.Count; "
        "  end Outer; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + C.Outer(); end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=2200\n");
}

// ── Row 4, the MATERIALIZER token, and the axis with a NON-null per-element
// disposer: `"a,b,c,d".Split(",")` is a fresh borrowing `Array<string>` whose
// reclaim must also release all four substrings, so a missed reclaim leaks five
// allocations per round rather than two.
// 200 * ((2 + 7) + 4) = 2600. // per §28.9 (issue #3705)
BEZEL_TEST(sync_iterator_borrowing_reclaim_scope_3705,
           generic_sync_iterator_spec_restores_the_materializer_token_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Gen<T>(a: T, b: T): IEnumerable<int> "
        "    var zs: Array<T> := [a, b]; "
        "    yield return zs.Count; "
        "    yield return 7; "
        "  end Gen; "
        "  public static method Outer(): int "
        "    var t := 0; "
        "    for var v in C.Gen<int>(1, 2) do t := t + v; end; "
        "    var parts := \"a,b,c,d\".Split(\",\"); "
        "    return t + parts.Count; "
        "  end Outer; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + C.Outer(); end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=2600\n");
}

// ── Row 5, the ASYNC-METHOD venue of the same gap. `emitAsyncMethod` never had
// this scope either; a generic async method monomorphized inside an outer async
// body cost the outer body the identical 24000 B / 400 allocations. This row is
// what turns issue 3705's "audit `emitAsyncMethod` for the same omission" into a
// measurement. 200 * ((2 + 7) + 3) = 2400. // per §28.11, §30.2 (issue #3705)
BEZEL_TEST(sync_iterator_borrowing_reclaim_scope_3705,
           generic_async_method_spec_restores_the_outer_bodys_tokens_spec_30_2) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Gen<T>(a: T, b: T): Task<int> "
        "    var zs: Array<T> := [a, b]; "
        "    return zs.Count + 7; "
        "  end Gen; "
        "  public static async method Outer(): Task<int> "
        "    var t := await C.Gen<int>(1, 2); "
        "    var ys: Array<int> := [5, 6, 7]; "
        "    return t + ys.Count; "
        "  end Outer; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + await C.Outer(); end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=2400\n");
}

// ── Row 6, the §27.5 CAPTURE family in the async-method venue — the SECOND
// scope `emitAsyncMethod` was missing (`CaptureReclaimSetScope`, which
// `emitIteratorMethod` has had since #3500 and `emitAsyncIteratorMethod` since
// #3507). The outer body's `var g := lambda() => k + 1` loses its capture-cell
// free: measured 800 B / 200 allocations, i.e. exactly one 4-byte cell per
// round. 200 * (3 + 6) = 1800. // per §27.5, §30.2 (issue #3705)
BEZEL_TEST(sync_iterator_borrowing_reclaim_scope_3705,
           generic_async_method_spec_restores_the_capture_cell_tokens_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Gen<T>(a: T): Task<int> "
        "    var f: () -> int := lambda() => 3; "
        "    return f(); "
        "  end Gen; "
        "  public static async method Outer(): Task<int> "
        "    var t := await C.Gen<int>(1); "
        "    var k := 5; "
        "    var g: () -> int := lambda() => k + 1; "
        "    return t + g(); "
        "  end Outer; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + await C.Outer(); end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=1800\n");
}

// ── Row 7, the SYNC-iterator twin of row 6. `emitIteratorMethod` has carried
// `CaptureReclaimSetScope` since #3500, so this row is CLEAN on BOTH sides — it
// is the control that says the capture half of row 6 is specific to
// `emitAsyncMethod` and not a property of specialisation in general.
// 200 * (3 + 6) = 1800. // per §27.5, §29.7 (issue #3705)
BEZEL_TEST(sync_iterator_borrowing_reclaim_scope_3705,
           sync_iterator_spec_already_restored_the_capture_tokens_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Gen<T>(a: T): IEnumerable<int> "
        "    var f: () -> int := lambda() => 3; "
        "    yield return f(); "
        "  end Gen; "
        "  public static method Outer(): int "
        "    var t := 0; "
        "    for var v in C.Gen<int>(1) do t := t + v; end; "
        "    var k := 5; "
        "    var g: () -> int := lambda() => k + 1; "
        "    return t + g(); "
        "  end Outer; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + C.Outer(); end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=1800\n");
}

// ── Row 8, the NON-GENERIC control. The same outer body and the same iterator
// shape, but no monomorphization, so `emitIteratorMethod` is reached only from
// `emitFunctionBody` — where the next method's own pre-scan re-pins everything
// and the missing scope cannot be observed. Clean on BOTH sides: it fixes the
// leak's cause to the mid-emission path, not to iterators as such.
// 200 * ((2 + 7) + 3) = 2400. // per §28.11 (issue #3705)
BEZEL_TEST(sync_iterator_borrowing_reclaim_scope_3705,
           non_generic_sync_iterator_control_is_clean_on_both_sides_spec_28_11) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Gen(a: int, b: int): IEnumerable<int> "
        "    var zs: Array<int> := [a, b]; "
        "    yield return zs.Count; "
        "    yield return 7; "
        "  end Gen; "
        "  public static method Outer(): int "
        "    var t := 0; "
        "    for var v in C.Gen(1, 2) do t := t + v; end; "
        "    var ys: Array<int> := [5, 6, 7]; "
        "    return t + ys.Count; "
        "  end Outer; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + C.Outer(); end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=2400\n");
}
