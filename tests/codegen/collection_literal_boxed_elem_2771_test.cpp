// Regression tests for #2771 — a RECORD implementing an interface, written
// directly into an interface-typed collection literal, was stored UNBOXED and
// SIGSEGV'd on the first element access.
//
// WHAT THE DOCUMENTS REQUIRE.  spec §8.6: a value type stored where the declared
// location is `Object` or an interface is BOXED.  §15.4 governs the collection
// literal itself.  So `var arr: Array<IRank> := [new Pt(3)]` for a record `Pt`
// implementing `IRank` must box each element into the interface-typed slot.
//
// THE DEFECT WAS A SIGSEGV WITH NO COLLECTION OPERATION INVOLVED — plain element
// access.  The emitted IR was
//     %2 = alloca %record.Pt
//     store %record.Pt %5, ptr %2
//     call void @bz_array_add(ptr %clit.arr, ptr %2, i64 8)
// `esz` is 8 because the declared element type is an INTERFACE (a pointer slot),
// so `bz_array_add` copied the record's first 8 bytes into a slot every reader
// treats as an object pointer.  `arr[0].Rank()` dereferenced the field bits as a
// vtable: exit 139.
//
// WHY IT LOOKED LIKE A COLLECTION DEFECT AND IS NOT.  Measured before the fix:
//   var x: IRank := new Pt(3); x.Rank()                     → 3    (worked)
//   var p := new Pt(3); var x: IRank := p; [x] then arr[0]  → 3    (worked)
//   var arr: Array<IRank> := [new Pt(3)]; arr[0].Rank()     → SIGSEGV
// Boxing into an interface-typed LOCAL was fine; only the literal-direct-into-
// array spelling was broken.  The gap was in the collection-literal element
// store, which did numeric coercion but never boxed — the assignment path
// (LlvmBackend_Assign.cpp ~5082) and the property/return paths already called
// `boxForTarget`.
//
// The fix keys on the DECLARED element type, and `boxForTarget` is a no-op
// unless that target is Object/interface AND the source is a boxable value type,
// so a class element stays a bare pointer (#399) and numeric elements keep their
// coercion.  Controls below pin each of those.
//
// This also UNBLOCKS the boxed-record row that #2768 and #2770 had to record as
// NOT MEASURED — `Min()`/`Max()`/`CompareTo` over an interface-typed array of
// boxed records now work, pinned by min_max_over_boxed_record_elements.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
// Generated-code ASan: reading record field bits as a vtable is exactly the
// class of error this catches, so the boxed cases run instrumented.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}
// ⚠ Boxed cases use the LENIENT oracle — generated-code ASan ON, LeakSanitizer
// OFF — and that choice is deliberate, measured, and filed.
//
// Restoring §8.6 boxing makes these programs CORRECT, and the box is a heap
// allocation that nothing currently disposes: 24 bytes per boxed element,
// reported by LeakSanitizer.  Filed as #2791.  The trade is explicit — before
// this fix the same programs SIGSEGV'd (exit 139), and a bounded leak is
// strictly better than a memory-safety fault — but a `runAotChecked` here would
// assert a dispose-exactly-once claim the fix does not yet make, so it would be
// a false claim rather than a stricter test.
//
// Nothing that matters for THIS defect is given up: the failure being pinned is
// a bad READ (record field bits dereferenced as a vtable), and generated-code
// ASan still trips on exactly that.  When #2791 lands these should be promoted
// to runAotChecked, which will then pin the disposal too.
//
// ⚠ The leak is BINARY-LAYOUT dependent: a standalone
// `BEZEL_AOT_FORCE_SANITIZE=address` build of the same program reported no leak
// while the harness's build reported it reliably.  Measure with the harness's
// own options (`detect_leaks=1:exitcode=99:abort_on_error=0`).
//
// fix #2791 / §8.6 / §15.4 (v0.240): the boxed cases are now clean under LSan
// — the literal materializer installs the bz_box_release disposer for any
// all-boxes literal, and the borrowing/owning distinction in
// `materializeHeapCollectionLiteral`'s `allBoxesAreBoxable` branch routes both
// Array<IRank> (ClassBorrow → NULL disposer pre-fix → 24 B/elem leak) and
// OwningArray<IRank> (ClassOwned → vtable dispose + tail-free pre-fix, which
// incidentally freed rc=1 boxes but bypassed the refcount + side-table erase)
// to a per-element bz_box_release thunk. The full LSan oracle (`runAotChecked`
// with `detect_leaks=1:exitcode=99:abort_on_error=0`) now asserts the
// dispose-exactly-once claim the original lenient oracle declined to make;
// promote the helpers here.
int runBoxed(const std::string& program) {
    auto r = bezel::test::runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

// A record implementing a self-comparable interface — the #2768/#2770 shape.
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

// #2771 / §8.6 + §15.4 — the reported repro, which exited 139 (SIGSEGV).
// Elements 3,1,2 read back in order, encoded base-4 (3*16 + 1*4 + 2 = 54) so the
// 8-bit WEXITSTATUS mask is not involved.
BEZEL_TEST(collection_literal_boxed_elem_2771, record_elements_boxed_into_interface_array) {
    BEZEL_EXPECT_EQ(runBoxed(main_(kPt,
        "    var arr: Array<IRank> := [new Pt(3), new Pt(1), new Pt(2)]; "
        "    return arr[0].Rank() * 16 + arr[1].Rank() * 4 + arr[2].Rank(); ")),
        54);
}

// A single element — the minimal shape, to pin that this is not an
// arity/alignment artefact of the three-element case.
BEZEL_TEST(collection_literal_boxed_elem_2771, single_record_element_boxed) {
    BEZEL_EXPECT_EQ(runBoxed(main_(kPt,
        "    var arr: Array<IRank> := [new Pt(3)]; "
        "    return arr[0].Rank(); ")), 3);
}

// The interface method dispatched through the boxed element — a vtable read on
// the box, which is precisely what faulted before.
BEZEL_TEST(collection_literal_boxed_elem_2771, interface_dispatch_through_boxed_element) {
    BEZEL_EXPECT_EQ(runBoxed(main_(kPt,
        "    var arr: Array<IRank> := [new Pt(7), new Pt(3)]; "
        "    return arr[0].CompareTo(arr[1]) + 10; ")), 14);
}

// UNBLOCKS #2768 / #2770's boxed-record row, which both had to record as NOT
// MEASURED because this shape SIGSEGV'd before any comparison ran.  Min is 1,
// Max is 3, CompareTo(3,1) is 2 → 1*16 + 3*4 + 2 + 2 = 32.
BEZEL_TEST(collection_literal_boxed_elem_2771, min_max_over_boxed_record_elements) {
    BEZEL_EXPECT_EQ(runBoxed(main_(kPt,
        "    var arr: Array<IRank> := [new Pt(3), new Pt(1), new Pt(2)]; "
        "    return arr.Min().Rank() * 16 + arr.Max().Rank() * 4 "
        "         + arr[0].CompareTo(arr[1]) + 2; ")), 32);
}

// CONTROL — boxing into an interface-typed LOCAL already worked and must stay
// working.  This is the case that proves the defect was in the literal's element
// store rather than in boxing generally.
BEZEL_TEST(collection_literal_boxed_elem_2771, control_interface_typed_local_unchanged) {
    BEZEL_EXPECT_EQ(runBoxed(main_(kPt,
        "    var x: IRank := new Pt(3); "
        "    return x.Rank(); ")), 3);
}

// CONTROL — a CLASS element must stay a BARE POINTER (#399): `boxForTarget` is
// keyed on the source being a boxable value type, so a class must not acquire a
// box here.  A spurious box would still dispatch correctly, so this asserts the
// value AND runs under ASan, where double-boxing a pointer would misread.
BEZEL_TEST(collection_literal_boxed_elem_2771, control_class_element_not_boxed) {
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

// CONTROL — a NUMERIC element keeps its `coerceNumericToType` result: the box
// step must not disturb the widening an `Array<long> := [1, 2]` literal relies
// on.  Plain runAot; nothing is allocated.
BEZEL_TEST(collection_literal_boxed_elem_2771, control_numeric_widening_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C  public static method Main(): int "
        "    var arr: Array<long> := [3, 1, 2]; "
        "    var s: long := arr[0] * 16 + arr[1] * 4 + arr[2]; "
        "    if s = 54 then return 54; end if; "
        "    return 1; "
        "  end Main; end C; "), 54);
}

// CONTROL — a record element in an array of that record's OWN type is not an
// Object/interface target, so it must remain unboxed and read back by value.
BEZEL_TEST(collection_literal_boxed_elem_2771, control_record_typed_array_unboxed) {
    BEZEL_EXPECT_EQ(runBoxed(main_(kPt,
        "    var arr: Array<Pt> := [new Pt(3), new Pt(1)]; "
        "    return arr[0].R * 4 + arr[1].R; ")), 13);
}
