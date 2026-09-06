// Regression tests for #2791 — a BOXED value-type element in an interface-typed
// (or `Object`-typed) collection literal was allocated and never disposed. The
// residual of #2771's SIGSEGV fix (which restored §8.6 boxing); every literal
// element of an Object/interface-typed array stored a heap-allocated box that
// nothing currently disposed. 24 bytes / element, measured by LSan.
//
// WHAT THE DOCUMENTS REQUIRE.  spec §8.6: a value type stored where the declared
// location is `Object` or an interface is BOXED.  §15.4 governs the collection
// literal itself; the literal's array is responsible for the boxes it mints.
// A borrowing array co-owns its elements exactly like it co-owns `Array<string>`
// strings (the disposer releases each on scope-end); an owning array fully
// owns them. The element disposer MUST be `bz_box_release` per element for
// both families — the runtime refcount path so a retained box (lambda capture,
// field store, outer OwningArray holding a borrowing inner) is not freed
// underneath its other owner.
//
// THE DEFECT.  Before this fix:
//   - BORROWING `Array<IRank> := [new Pt(3)]` — the array's header disposer was
//     null (ClassBorrow policy), so the box leaked. Measured: 24 B leaked in 1
//     allocation under LSan (`detect_leaks=1:exitcode=99:abort_on_error=0`).
//   - OWNING `OwningArray<IRank> := [new Pt(3)]` — the existing
//     elementDisposerThunk was installed (it routes through the element's
//     vtable Dispose slot, a no-op `builtinDisposeFn_` for boxes, then `free`s
//     the wrapper). That works for rc=1 boxes but bypasses the refcount
//     (UAF if retained) and skips the bz_obj_identity_hash_forget side-table
//     erase. Replaced with the explicit `bz_box_release` thunk.
//
// Direction (per the issue): the literal materializer computes `allBoxesAreBoxable`
// — the element type is Object/interface AND every element source is a boxable
// value type — and installs the `boxElementDisposerThunk` (a `void(void*)` thunk
// that calls `bz_box_release`) in two places:
//   - the bz_array_t header (so any downstream free site can recover it via
//     bz_array_free_dynamic), overriding the class-vtable disposer for owning
//     arrays and setting one for borrowing arrays that had none;
//   - the throw-mid-literal unwind handler (so a throwing element expression
//     doesn't leak the boxes already added).
// The local-decl scope-end disposal is also wired to the same disposer
// explicitly (the literal-path early return in lowerLocalDecl bypasses the
// shared #1003 reclaim block, so the disposer must be set at this site too).
// Mixed literals (some boxable-value sources, some class-instance sources)
// are deliberately NOT covered here — a single `void(void*)` disposer can't
// distinguish a class instance from a box at runtime (they share the same
// pointer shape; calling bz_box_release on a class instance would treat the
// vtable word as a refcount and corrupt it). For mixed literals the existing
// elementDisposerThunk stays correct for the class slots and incidentally
// frees the rc=1 boxes via its tail `free(obj)` — same standing trade.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// runAotChecked is the LSan-clean oracle: ASan-instrumented generated code +
// `ASAN_OPTIONS=detect_leaks=1:exitcode=99:abort_on_error=0` at run time. A
// real sanitizer failure (leak / UAF / OOB) surfaces as `r.ran == false` with
// the report text in `r.diagnostics`; a clean program returns normally with
// `r.ran == true` and the program's exit code in `r.exitCode`. See
// `tests/support/aot_support.hpp` "TWO SEMANTIC DIFFERENCES" and `runAotCheckedImpl`.
//
// ⚠ The harness's exact options (`detect_leaks=1:exitcode=99:abort_on_error=0`)
// are load-bearing — the issue notes that a standalone
// `BEZEL_AOT_FORCE_SANITIZE=address` build reported no leak while the
// harness's build reported it reliably. Same harness options used here.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

// #2771's record-implements-interface shape.
const char* kPt =
    "module M; "
    "public interface IRank: IComparable<IRank> "
    "  method Rank(): int; "
    "end IRank; "
    "public record Pt(R: int): IRank "
    "  public method Rank(): int => R; "
    "  public method CompareTo(other: IRank): int => R - other.Rank(); "
    "end Pt; ";

std::string main_(const char* preamble, const std::string& body) {
    return std::string(preamble) +
           "class C  public static method Main(): int " + body +
           "  end Main; end C; ";
}
}  // namespace

// §8.6 / §15.4: the issue's repro. BORROWING literal with a single boxed
// record element. Pre-fix: 24 B leaked. Post-fix: clean LSan + value 3.
BEZEL_TEST(array_record_boxed_collection_disposal_2791, borrowing_single_boxed_element_reclaimed) {
    BEZEL_EXPECT_EQ(runChecked(main_(kPt,
        "    var arr: Array<IRank> := [new Pt(3)]; "
        "    return arr[0].Rank(); ")), 3);
}

// §8.6 / §15.4: BORROWING literal with MULTIPLE boxed elements. Pre-fix:
// 24 B × N leaked. Post-fix: clean LSan + value 54 (base-4 accumulator of
// 3,1,2; every wrong order stays under 256 so the 8-bit WEXITSTATUS mask
// is not involved).
BEZEL_TEST(array_record_boxed_collection_disposal_2791, borrowing_multi_boxed_elements_reclaimed) {
    BEZEL_EXPECT_EQ(runChecked(main_(kPt,
        "    var arr: Array<IRank> := [new Pt(3), new Pt(1), new Pt(2)]; "
        "    return arr[0].Rank() * 16 + arr[1].Rank() * 4 + arr[2].Rank(); ")),
        54);
}

// §8.6 / §15.4: OWNING literal — `OwningArray<IRank>` — owns its boxes
// fully. Wrapped in `using var` so the array backing is disposed at scope
// end (without the `using`, the backing would leak — a separate, still-open
// #2774 defect, orthogonal to #2791). Pre-fix: the box itself leaked via the
// existing class-vtable disposer (the bypass path through `free(obj)` worked
// for rc=1 boxes but skipped the refcount + side-table erase — see the
// issue's "DELIBERATE, RECORDED TRADE" paragraph). Post-fix: clean LSan +
// value 54.
BEZEL_TEST(array_record_boxed_collection_disposal_2791, owning_multi_boxed_elements_reclaimed) {
    BEZEL_EXPECT_EQ(runChecked(main_(kPt,
        "    using var arr: OwningArray<IRank> := [new Pt(3), new Pt(1), new Pt(2)]; "
        "    return arr[0].Rank() * 16 + arr[1].Rank() * 4 + arr[2].Rank(); ")),
        54);
}

// §8.6 / §15.4: OWNING literal, single element. Pre-fix: same bypass path
// as the multi-element case. Post-fix: clean LSan + value 7.
BEZEL_TEST(array_record_boxed_collection_disposal_2791, owning_single_boxed_element_reclaimed) {
    BEZEL_EXPECT_EQ(runChecked(main_(kPt,
        "    using var arr: OwningArray<IRank> := [new Pt(7)]; "
        "    return arr[0].Rank(); ")), 7);
}

// §8.6 / §15.4: Object-typed borrowing literal — the same shape as the
// `Array<IRank>` cases but on `Object` directly. The Object target admits
// every boxable value type per §8.6 (records, primitives, enums, inline
// arrays, spans, unit, Range, nullable values). Value 3 — single-element.
BEZEL_TEST(array_record_boxed_collection_disposal_2791, borrowing_object_typed_boxed_element_reclaimed) {
    BEZEL_EXPECT_EQ(runChecked(main_(kPt,
        "    var arr: Array<Object> := [new Pt(3)]; "
        "    return (arr[0] as Pt)!.R; ")), 3);
}

// §8.6 / §15.4: Object-typed owning literal. NOTE: `OwningArray<Object>` is
// rejected by stdlib §4.2 / §14.4 (#1327) — Object has no Dispose and so has
// no owning-collection element shape. The interface-typed owning cases above
// (`using var arr: OwningArray<IRank> := [new Pt(3)]`) already cover the
// §8.6 box-vs-§15.4 owning disposal claim on a different Object-adjacent
// slot, since IRank IS the interface side of the same Object/interface
// boxing surface. Skipping a redundant `OwningArray<Object>` case here.

// §8.6 / §15.4: borrowing literal of a PRIMITIVE boxed element
// (`Array<Object> := [42]` — the int 42 is boxed into Object per §8.6, the
// same bz_box path a record uses). The §8.6 box policy applies to every
// boxable value type, not just records; this case pins that the literal
// materializer's all-boxes predicate correctly admits primitives. Value 42.
// The unbox `as int?` yields a nullable int (§7) — guard with null check
// before the comparison so the value channel is the bare int 42.
BEZEL_TEST(array_record_boxed_collection_disposal_2791, borrowing_primitive_boxed_element_reclaimed) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C  public static method Main(): int "
        "    var arr: Array<Object> := [42]; "
        "    var x: int? := arr[0] as int?; "
        "    if x = null then return -1; end; "
        "    return x; "
        "  end Main; end C; "), 42);
}

// CONTROL — borrowing literal with a CLASS element stays a bare pointer
// (#399: boxForTarget is a no-op for a reference source). The literal
// materializer's all-boxes predicate declines on a class source, so the
// existing elementDisposerThunk (or null for ClassBorrow) stays in place —
// the class instance is owned by its `using var`, the array borrows it, and
// the slot's content is the class pointer (not a box). Pinned by the same
// shape #2771's control_class_element_not_boxed covers, now under
// runAotChecked (so the absence of a box in the slot is also a dispose-
// exactly-once claim).
BEZEL_TEST(array_record_boxed_collection_disposal_2791, control_borrowing_class_element_unaffected) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "public interface IRank: IComparable<IRank> "
        "  method Rank(): int; "
        "end IRank; "
        "class Node: IRank "
        "  private _r: int; "
        "  public constructor(r: int) _r := r; end constructor; "
        "  public method Rank(): int => _r; "
        "  public method CompareTo(other: IRank): int => _r - other.Rank(); "
        "end Node; "
        "class C  public static method Main(): int "
        "    using var a := new Node(3); "
        "    using var b := new Node(1); "
        "    var arr: Array<IRank> := [a, b]; "
        "    return arr[0].Rank() * 4 + arr[1].Rank(); "
        "  end Main; end C; "), 13);
}

// CONTROL — borrowing literal of NUMERIC elements keeps its
// `coerceNumericToType` result; the box step (a no-op for value-typed
// targets per §8.6) does not disturb the widening. A `Array<long> := [1,2]`
// stays an Array of long, not of boxes. Pre-fix this was already correct;
// the fix must not regress it.
BEZEL_TEST(array_record_boxed_collection_disposal_2791, control_numeric_widening_unchanged) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C  public static method Main(): int "
        "    var arr: Array<long> := [3, 1, 2]; "
        "    var s: long := arr[0] * 16 + arr[1] * 4 + arr[2]; "
        "    if s = 54 then return 54; end if; "
        "    return 1; "
        "  end Main; end C; ").exitCode, 54);
}

// CONTROL — record-typed array literal (`Array<Pt> := [...]`) is NOT an
// Object/interface target, so elements stay unboxed (per value semantics —
// §3.2, §9 — records are value types). The all-boxes predicate declines on
// a record-typed array element, so the disposer stays null (Value category,
// no per-slot bookkeeping). Pins that the fix does not gratuitously install
// a box disposer for a record-typed slot.
BEZEL_TEST(array_record_boxed_collection_disposal_2791, control_record_typed_array_unboxed) {
    BEZEL_EXPECT_EQ(runChecked(main_(kPt,
        "    var arr: Array<Pt> := [new Pt(3), new Pt(1)]; "
        "    return arr[0].R * 4 + arr[1].R; ")), 13);
}