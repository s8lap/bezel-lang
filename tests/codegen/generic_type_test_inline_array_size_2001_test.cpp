// #2001 (§14.2 value/inline-array type args, §12.9 type-test invariance, §17.4
// is/as, v0.197) — a runtime `is`/`as`/case type test against a CONSTRUCTED
// generic type whose type ARGUMENT is an inline array must honor the array SIZE
// N, not just the element type.
//
// The defect: the per-monomorphization runtime descriptor key (#1843,
// appendDescKey) encoded an inline-array type argument's ELEMENT but not its
// size N, AND descArgsConcrete treated an inline-array argument as non-concrete
// so BOTH the object's descriptor and the `is`/`as` target's descriptor fell
// back to the shared, erased class tag. So `C<Box<[3]int>>` and `C<Box<[4]int>>`
// shared ONE descriptor: `Box<[4]int> is Box<[3]int>` returned TRUE (false
// positive) and the corresponding `as` succeeded → type confusion → an OOB heap
// write (the object has the small `[N]` layout, the cast admits the large one).
// Inline arrays are valid value-constrained generic type args (§14.2), so this
// is reachable in safe code with no FFI.
//
// The fix makes the inline-array size runtime-precise: appendDescKey encodes N
// (+ownedness), descArgsConcrete admits a folded-size inline array, and
// typeRefEqual / typeArgRuntimeReified follow. `[3]T` and `[4]T` now mint DISTINCT
// descriptors, so the type tests are precise (and #1971's over-reject is lifted).
//
// Most tests observe a bool via the process exit code (1 = true, 0 = false).

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

// Box<U> (an inline-array-friendly one-field wrapper) plus Wrap<W> (a phantom
// generic whose W is only a type argument, never stored — isolates the nested
// descriptor-key recursion), followed by a Main body. Reused across cases.
int runProg(const std::string& mainBody) {
    return run(
        "module M; "
        "class Box<U> public V: U; "
        "  public constructor(v: U) V := v; end constructor; end Box; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class C public static method Main(): int " + mainBody +
        " end Main; end C;");
}

}  // namespace

// ---- DIRECT inline-array arg: the exact reported false positive ---------------

// The reported defect: `Box<[4]int> is Box<[3]int>` was TRUE (size collision). It
// must be FALSE — the array size differs (FAILS WITHOUT the fix: descArgsConcrete
// rejected the inline-array arg, so both collapsed to the shared Box descriptor).
BEZEL_TEST(codegen_2001, direct_is_wrong_size_is_false) {
    BEZEL_EXPECT_EQ(runProg(
        "var a: [4]int; a[0] := 100; a[1] := 101; a[2] := 102; a[3] := 103; "
        "using var b := new Box<[4]int>(a); "
        "var o: Object := b; "
        "return (if o is Box<[3]int> then 1 else 0);"), 0);
}

// The genuinely-matching size still matches.
BEZEL_TEST(codegen_2001, direct_is_right_size_is_true) {
    BEZEL_EXPECT_EQ(runProg(
        "var a: [4]int; a[0] := 100; a[1] := 101; a[2] := 102; a[3] := 103; "
        "using var b := new Box<[4]int>(a); "
        "var o: Object := b; "
        "return (if o is Box<[4]int> then 1 else 0);"), 1);
}

// `as` to the wrong size yields null (§17.4) — no reinterpretation. (The result
// is bound to an INFERRED local: a typed-annotation `var w: Box<[3]int>? := ...`
// currently trips a PRE-EXISTING, orthogonal §5.1 gap — typeArgSameBase /
// typeArgIdentical lack an InlineArray case, so `variantGenericAssignable`
// rejects `Box<[3]int> := Box<[3]int>` regardless of #2001; see the report.)
BEZEL_TEST(codegen_2001, direct_as_wrong_size_is_null) {
    BEZEL_EXPECT_EQ(runProg(
        "var a: [4]int; a[0] := 1; a[1] := 2; a[2] := 3; a[3] := 4; "
        "using var b := new Box<[4]int>(a); "
        "var o: Object := b; "
        "var w := o as Box<[3]int>; "
        "return (if w = null then 1 else 0);"), 1);
}

// `as` to the correct size succeeds (non-null).
BEZEL_TEST(codegen_2001, direct_as_right_size_non_null) {
    BEZEL_EXPECT_EQ(runProg(
        "var a: [4]int; a[0] := 1; a[1] := 2; a[2] := 3; a[3] := 4; "
        "using var b := new Box<[4]int>(a); "
        "var o: Object := b; "
        "var w := o as Box<[4]int>; "
        "return (if w <> null then 1 else 0);"), 1);
}

// ---- MEMORY SAFETY (the headline): the false-positive `as` enabled an OOB write.
// Under generated-code AddressSanitizer (runAotChecked): hold a SMALL Box<[2]int>,
// try to reinterpret it as a MUCH larger Box<[64]int>, and only if the (unsound)
// cast succeeds write far past the real 2-int allocation. WITHOUT the fix the cast
// wrongly succeeds and the write is an OOB heap-buffer-overflow → ASan fires →
// r.ran is false. WITH the fix `as` returns null, the OOB path is unreachable, and
// the program cleanly returns 0.
BEZEL_TEST(codegen_2001, memory_safety_false_positive_as_no_oob) {
    auto r = runAotChecked(
        "module M; "
        "class Box<U> public V: U; "
        "  public constructor(v: U) V := v; end constructor; end Box; "
        "class C public static method Main(): int "
        "  var a: [2]int; a[0] := 1; a[1] := 2; "
        "  using var small := new Box<[2]int>(a); "
        "  var o: Object := small; "
        "  var w := o as Box<[64]int>; "  // inferred local (see §5.1-gap note above)
        "  if w <> null then "
        "    w.V[63] := 999999; "   // OOB ~248 bytes past the real region (base)
        "    return 1; "            // the cast wrongly succeeded
        "  end; "
        "  return 0; "              // sound: `as` returned null
        "end Main; end C;");
    // SKIP if the ASan toolchain is unavailable (contract of runAotChecked).
    if (!r.ran && r.diagnostics.hasErrors()) {
        bool skipped = false;
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.message.find("runAotChecked: SKIP") != std::string::npos)
                skipped = true;
        if (skipped) return;
    }
    BEZEL_EXPECT_TRUE(r.ran);       // no ASan fire, clean exit
    BEZEL_EXPECT_EQ(r.exitCode, 0); // `as` correctly returned null
}

// ---- NESTED inline-array arg (also #1971's runtime shape): the size distinction
// must hold at every depth. `Wrap<Box<[4]int>> is Wrap<Box<[3]int>>` → FALSE.
BEZEL_TEST(codegen_2001, nested_is_wrong_size_is_false) {
    BEZEL_EXPECT_EQ(runProg(
        "using var outer := new Wrap<Box<[4]int>>(5); "
        "var o: Object := outer; "
        "return (if o is Wrap<Box<[3]int>> then 1 else 0);"), 0);
}

// Nested, matching size → TRUE.
BEZEL_TEST(codegen_2001, nested_is_right_size_is_true) {
    BEZEL_EXPECT_EQ(runProg(
        "using var outer := new Wrap<Box<[4]int>>(5); "
        "var o: Object := outer; "
        "return (if o is Wrap<Box<[4]int>> then 1 else 0);"), 1);
}

// ---- CONTROL: element precision preserved (must not regress) ------------------

// `Box<int> is Box<string>` — element differs → still FALSE (not over-split junk).
BEZEL_TEST(codegen_2001, control_element_wrong_is_false) {
    BEZEL_EXPECT_EQ(runProg(
        "using var b := new Box<int>(7); "
        "var o: Object := b; "
        "return (if o is Box<string> then 1 else 0);"), 0);
}

// `Box<int> is Box<int>` — same element → still TRUE.
BEZEL_TEST(codegen_2001, control_element_right_is_true) {
    BEZEL_EXPECT_EQ(runProg(
        "using var b := new Box<int>(7); "
        "var o: Object := b; "
        "return (if o is Box<int> then 1 else 0);"), 1);
}

// ---- CONTROL: genuine dedup preserved — two `Box<[3]int>` share ONE descriptor.
// A second, independently-constructed Box<[3]int> must still be `is Box<[3]int>`
// (the fix SPLITS on differing size, it must not stop deduping identical sizes).
BEZEL_TEST(codegen_2001, control_same_size_dedup_is_true) {
    BEZEL_EXPECT_EQ(runProg(
        "var a: [3]int; a[0] := 1; a[1] := 2; a[2] := 3; "
        "using var b1 := new Box<[3]int>(a); "
        "var a2: [3]int; a2[0] := 9; a2[1] := 8; a2[2] := 7; "
        "using var b2 := new Box<[3]int>(a2); "
        "var o1: Object := b1; "
        "var o2: Object := b2; "
        // b1 is Box<[3]int> AND b2 is Box<[3]int> — both share the descriptor.
        "var m1 := (if o1 is Box<[3]int> then 1 else 0); "
        "var m2 := (if o2 is Box<[3]int> then 1 else 0); "
        "return m1 * 10 + m2;"), 11);
}

// ---- SPECIALIZATION SPLIT: both [3] and [4] monomorphizations built + used in ONE
// module (separate descriptors, separate specialized vtables/methods) — no codegen
// crash / ODR-ish collision, and all cross type-tests resolve precisely.
BEZEL_TEST(codegen_2001, specialization_split_both_used_no_collision) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box<U> public V: U; public Label: int; "
        "  public constructor(v: U, lbl: int) V := v; Label := lbl; end constructor; "
        "  public method GetLabel(): int return Label; end GetLabel; end Box; "
        "class C public static method Main(): int "
        "  var a3: [3]int; a3[0] := 30; a3[1] := 31; a3[2] := 32; "
        "  var a4: [4]int; a4[0] := 40; a4[1] := 41; a4[2] := 42; a4[3] := 43; "
        "  using var b3 := new Box<[3]int>(a3, 7); "
        "  using var b4 := new Box<[4]int>(a4, 9); "
        "  var s := b3.GetLabel() + b4.GetLabel(); "   // 16
        "  var o3: Object := b3; var o4: Object := b4; "
        "  var t1 := (if o3 is Box<[3]int> then 1 else 0); "  // 1
        "  var t2 := (if o3 is Box<[4]int> then 1 else 0); "  // 0
        "  var t3 := (if o4 is Box<[4]int> then 1 else 0); "  // 1
        "  var t4 := (if o4 is Box<[3]int> then 1 else 0); "  // 0
        "  return s + 10*t1 + 20*t2 + 30*t3 + 40*t4; "  // 16+10+0+30+0 = 56
        "end Main; end C;"), 56);
}

// ---- #1971 admission, RUNTIME-CORRECT: a case-binding `var x: C<[3]int>` against
// a generic interface scrutinee IMarker<int> (C<T>: IMarker<T>, IMarker<int>). The
// fix admits it (formerly rejected "not runtime-distinguishable") and it matches
// ONLY genuine C<[3]int> values.
BEZEL_TEST(codegen_2001, iface_bind_inline_array_admitted_matches) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IMarker<T> end IMarker; "
        "sealed class C<T> : IMarker<T>, IMarker<int> public Val: int; "
        "  public constructor(v: int) Val := v; end constructor; end C; "
        "class Program public static method Main(): int "
        "  var c := new C<[3]int>(42); "
        "  var m: IMarker<int> := c; "
        "  var n := (case m of "
        "    var x: C<[3]int> => x.Val, "
        "    _ => -1); "
        "  return n; "   // 42 (admitted + matches)
        "end Main; end Program;"), 42);
}
