// Codegen — an INLINE-ARRAY element type in a heap collection (§15.1.2,
// §15.1, v0.338, #6273).
//
// THE DEFECT. `Emitter::collectionElemSize` (LlvmBackend_Calls.cpp) computes
// the byte STRIDE of a type-erased collection slot. Its size ladder covered
// i32/float, i1/i8, i16 and `llvm::StructType` (the record case, §9), then fell
// through to a hardcoded `return 8` for everything else. An inline-array
// element type lowers to an `llvm::ArrayType` (`[N x T]`, Layout.cpp's
// InlineArray arm of lowerTypeRef) — neither a scalar case nor a StructType —
// so it took that fall-through.
//
// For an `[N]T` whose payload exceeds 8 bytes this is a wrong stride:
// `[3]int` = 12, `[2]string` = 16, `[2](int) -> int` = 16. `bz_array_ptr_at`
// hands the scan `src + i * 8`, i.e. a pointer into the MIDDLE of element i,
// and the `CreateLoad` of the element reads across the following slot. Every
// equality answer was computed over overlapping bytes.
//
// MEASURED on this fix's parent (b1a94683), the issue's own repro:
//     False / True / False / False
// and, isolating storage from comparison, a stored `[10,11,12]` read back
// through `s[0]` gave `10,11,0` — the third slot lost to the 8-byte stride.
//
// §15.1.2 (v0.338): "Two inline-array values of the same type are equal iff
// their elements are pairwise equal under the element type's equality." An
// element that IS in the array must therefore answer True.
//
// The fix is the DataLayout arm: `getTypeAllocSize([N x T])` is exactly
// `N * sizeof(T)` with the type's own trailing padding — the same rule the
// §9 record arm already used. `lazyElemSize` (LlvmBackend_Calls3.cpp), the
// LINQ-side twin ladder, carried the identical defect and the identical fix.
//
// The "at most 8 bytes" boundary is pinned deliberately on both sides:
// `[2]int` (exactly 8) passed before the fix and must still pass, and `[1]int`
// (4) likewise — a fix that widened the STRIDE question into a layout change
// would break them.
//
// Regression: `hs.Contains(h)` answered FALSE for a present `[3]int` element;
// `True / True / False / False` pre-fix for the four issue rows.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

std::string runStdout(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

// An Array<[3]int> holding one element; `.Contains` of the PRESENT element must
// answer True and of a genuinely absent one False. Both directions in one
// program, because the pre-fix stride made the scan read overlapping bytes and
// could answer either way.
constexpr const char* kContains3Int =
    "module M; class C public static method Main(): int "
    "  var present: [3]int := [1, 2, 3]; "
    "  var absent:  [3]int := [4, 5, 6]; "
    "  var hs: Array<[3]int> := [present]; "
    "  Console.WriteLine($\"{hs.Contains(present)} {hs.Contains(absent)}\"); "
    "  return 0; "
    "end Main; end C;";

// The same query over a 2-element STR of a 3-int element: slot 1's read must
// reach its own third element, not slot 0's or the array's tail.
constexpr const char* kContains3IntTwoSlots =
    "module M; class C public static method Main(): int "
    "  var a: [3]int := [10, 11, 12]; "
    "  var b: [3]int := [20, 21, 22]; "
    "  var hs: Array<[3]int> := [a, b]; "
    "  Console.WriteLine($\"{hs.Contains(a)} {hs.Contains(b)}\"); "
    "  return 0; "
    "end Main; end C;";

// Storage read-back (no equality involved): every slot of a stored [3]int must
// survive the stride. Pre-fix `s[0]` read back `10,11,0`. This row is what
// separates a STRIDE fix from a comparison fix — a fix that only corrected the
// equality answer would still fail here.
constexpr const char* kReadBack3Int =
    "module M; class C public static method Main(): int "
    "  var e0: [3]int := [10, 11, 12]; "
    "  var e1: [3]int := [20, 21, 22]; "
    "  var hs: Array<[3]int> := [e0, e1]; "
    "  var t: [3]int := hs[0]; "
    "  var u: [3]int := hs[1]; "
    "  Console.WriteLine($\"{t[0]},{t[1]},{t[2]} {u[0]},{u[1]},{u[2]}\"); "
    "  return 0; "
    "end Main; end C;";

// A 16-byte element: two `string` slots. Pre-fix FALSE. The string rows also
// prove the fix did not disturb §4.6 content equality by content-routing a
// [2]string slot through the pointer-identity arm.
constexpr const char* kContains2String =
    "module M; class C public static method Main(): int "
    "  var p: [2]string := [\"a\", \"b\"]; "
    "  var q: [2]string := [\"x\", \"y\"]; "
    "  var ps: Array<[2]string> := [p, q]; "
    "  Console.WriteLine($\"{ps.Contains(p)} {ps.Contains(q)}\"); "
    "  return 0; "
    "end Main; end C;";

// A 16-byte element of FUNCTION type: two function-pointer slots. Pre-fix
// FALSE. §15.1.2 applies to a function-pointer element exactly as to any
// other; the element's own equality is reference identity (§8.3).
constexpr const char* kContains2Func =
    "module M; class C public static method Main(): int "
    "  var f: (int) -> int := lambda(x) => x; "
    "  var fa: [2](int) -> int := [f, f]; "
    "  var g: (int) -> int := lambda(x) => x + 1; "
    "  var fb: [2](int) -> int := [g, g]; "
    "  var fs: Array<[2](int) -> int> := [fa, fb]; "
    "  Console.WriteLine($\"{fs.Contains(fa)} {fs.Contains(fb)}\"); "
    "  return 0; "
    "end Main; end C;";

// A ONE-slot function element — exactly 8 bytes, so it took the correct stride
// even pre-fix. Pinned so a fix that changed the stride ladder structurally
// (rather than adding the DataLayout arm) would be caught.
constexpr const char* kContains1Func =
    "module M; class C public static method Main(): int "
    "  var f: (int) -> int := lambda(x) => x; "
    "  var g: [1](int) -> int := [f]; "
    "  var gs: Array<[1](int) -> int> := [g]; "
    "  Console.WriteLine($\"{gs.Contains(g)}\"); "
    "  return 0; "
    "end Main; end C;";

// A `[2]int` element — exactly 8 bytes, likewise unaffected pre-fix.
constexpr const char* kContains2Int =
    "module M; class C public static method Main(): int "
    "  var h: [2]int := [1, 2]; "
    "  var k: [2]int := [3, 4]; "
    "  var hs: Array<[2]int> := [h, k]; "
    "  Console.WriteLine($\"{hs.Contains(h)} {hs.Contains(k)}\"); "
    "  return 0; "
    "end Main; end C;";

// A `[1]int` element — 4 bytes, the smallest inline-array element.
// `k` is held only as a query and never stored, so `Contains(k)` must be False.
constexpr const char* kContains1Int =
    "module M; class C public static method Main(): int "
    "  var h: [1]int := [42]; "
    "  var k: [1]int := [7]; "
    "  var hs: Array<[1]int> := [h]; "
    "  Console.WriteLine($\"{hs.Contains(h)} {hs.Contains(k)}\"); "
    "  return 0; "
    "end Main; end C;";

// A `[4]int` element — 16 bytes, the same width as [2]string but a pure
// integer stride, so a fix that only special-cased pointer slots would fail.
constexpr const char* kContains4Int =
    "module M; class C public static method Main(): int "
    "  var h: [4]int := [1, 2, 3, 4]; "
    "  var k: [4]int := [5, 6, 7, 8]; "
    "  var hs: Array<[4]int> := [h, k]; "
    "  Console.WriteLine($\"{hs.Contains(h)} {hs.Contains(k)}\"); "
    "  return 0; "
    "end Main; end C;";

// The issue's other named collections. `Stack`/`Queue`/`LinkedList` and `in`
// all route their per-element comparison through the SAME stride helper
// (`collectionElemSize`) as `Array`, so the fix reaches them without a second
// edit; this row pins that claim rather than leaving it as an assumption.
constexpr const char* kStackQueue =
    "module M; class C public static method Main(): int "
    "  var h: [3]int := [1, 2, 3]; "
    "  var k: [3]int := [4, 5, 6]; "
    "  var st: Stack<[3]int> := [h]; "
    "  var q: Queue<[3]int> := [k]; "
    "  Console.WriteLine($\"{st.Contains(h)} {q.Contains(k)}\"); "
    "  return 0; "
    "end Main; end C;";

constexpr const char* kInOperator =
    "module M; class C public static method Main(): int "
    "  var h: [3]int := [1, 2, 3]; "
    "  var k: [3]int := [4, 5, 6]; "
    "  var hs: Array<[3]int> := [h, k]; "
    "  Console.WriteLine($\"{h in hs} {k in hs}\"); "
    "  return 0; "
    "end Main; end C;";

}  // namespace

// §15.1.2 / §25.2: `.Contains` uses the element type's equality. A PRESENT
// [3]int answers True, an absent one False.
BEZEL_TEST(codegen_inline_array_elem_stride_6273,
           contains_3int_present_true_absent_false_spec_15_1_2) {
    BEZEL_EXPECT_EQ(runStdout(kContains3Int), "True False\n");
}

// §15.1.2: BOTH slots of a two-element array — the second element's read must
// not be served from the first's overlapping bytes.
BEZEL_TEST(codegen_inline_array_elem_stride_6273,
           contains_3int_both_slots_spec_15_1_2) {
    BEZEL_EXPECT_EQ(runStdout(kContains3IntTwoSlots), "True True\n");
}

// §15.1: an inline array is a VALUE type; a stored one round-trips slot for
// slot. This is the storage half of the defect, isolated from equality.
BEZEL_TEST(codegen_inline_array_elem_stride_6273,
           stored_3int_round_trips_every_slot_spec_15_1) {
    BEZEL_EXPECT_EQ(runStdout(kReadBack3Int), "10,11,12 20,21,22\n");
}

// §15.1.2 with a [2]string element — §4.6 content equality per slot.
BEZEL_TEST(codegen_inline_array_elem_stride_6273,
           contains_2string_spec_15_1_2) {
    BEZEL_EXPECT_EQ(runStdout(kContains2String), "True True\n");
}

// §15.1.2 with a [2](int) -> int element — reference identity per slot (§8.3).
BEZEL_TEST(codegen_inline_array_elem_stride_6273,
           contains_2func_spec_15_1_2) {
    BEZEL_EXPECT_EQ(runStdout(kContains2Func), "True True\n");
}

// §15.1.2: an 8-byte element ([1] function pointer) was already correct —
// pinned so a structural change to the size ladder would be caught.
BEZEL_TEST(codegen_inline_array_elem_stride_6273,
           contains_1func_8byte_unchanged_spec_15_1_2) {
    BEZEL_EXPECT_EQ(runStdout(kContains1Func), "True\n");
}

// §15.1.2: an exactly-8-byte [2]int element — the issue's boundary case.
BEZEL_TEST(codegen_inline_array_elem_stride_6273,
           contains_2int_8byte_unchanged_spec_15_1_2) {
    BEZEL_EXPECT_EQ(runStdout(kContains2Int), "True True\n");
}

// §15.1.2: a 4-byte [1]int element — the smallest inline-array element.
BEZEL_TEST(codegen_inline_array_elem_stride_6273,
           contains_1int_4byte_spec_15_1_2) {
    BEZEL_EXPECT_EQ(runStdout(kContains1Int), "True False\n");
}

// §15.1.2: a 16-byte [4]int element — the same width as [2]string, so a fix
// that only widened pointer-shaped slots would fail here.
BEZEL_TEST(codegen_inline_array_elem_stride_6273,
           contains_4int_16byte_spec_15_1_2) {
    BEZEL_EXPECT_EQ(runStdout(kContains4Int), "True True\n");
}

// stdlib §4 "Equality search" over the sibling array-backed collections,
// which share `collectionElemSize`.
BEZEL_TEST(codegen_inline_array_elem_stride_6273,
           contains_stack_queue_spec_15_1_2) {
    BEZEL_EXPECT_EQ(runStdout(kStackQueue), "True True\n");
}

// §17.5: the `in` operator uses the same per-element equality as `.Contains`.
BEZEL_TEST(codegen_inline_array_elem_stride_6273,
           in_operator_spec_17_5) {
    BEZEL_EXPECT_EQ(runStdout(kInOperator), "True True\n");
}
