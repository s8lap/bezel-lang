// fix #2980 — the three sibling box-Equals thunks (`beq`/`peq`/`iaeq`) carried
// the SAME two-load vtable dereference that ASan caught in `bz$uniteq` during
// the 08-07 run (spec §8.3, v0.240). Each thunk loaded word 0 of `other` as a
// vtable pointer and then DEREFERENCED IT to read vtable[0] (the type
// descriptor), comparing descriptors. `Equals(other: Object?)` per §8.3 is
// dispatched through lowerObjectEquals, which sends a NON-string, non-null
// `other` of ANY Object kind to the vtable Equals slot — including a
// `%bz.string`, whose word 0 is the char-data pointer, not a vtable. The second
// load then read 8 bytes from the char-data global of an interned literal (a
// 2-byte `[2 x i8]` for "x") — a global-buffer-overflow under ASan, invisible
// to the plain oracle (the read lands in mapped memory and the comparison
// correctly yields false), exactly the #2633 defect class.
//
// THE DEFECT IS REACHABLE, NOT LATENT. The issue's premise ("latent, not
// reproduced — two attempts ran clean") was a measurement gap: both prior
// attempts failed to confirm via IR that the dispatch actually reached the
// thunks. Confirmed here on HEAD d2a3d3e via BEZEL_DUMP_IR + ASan probes for
// all three:
//   peq  (primitive box): `var i: Object := 7; var s: Object := "x"; i.Equals(s);`
//     → bz$primeq$int is the indirect callee at the vtable Equals slot (IR line
//       `%eq.obj.r = call i1 %10(ptr %5, ptr %6)`), and under ASan:
//       "global-buffer-overflow ... in bz$primeq$int ... 0 bytes after global
//        variable '.bz.str.data.49' ... of size 2 ... ascii string 'x'"
//   beq  (record box):     `var p: Object := new Pt(1,2); var s: Object := "x";
//                          p.Equals(s);`
//     → "global-buffer-overflow ... in bz$boxeq$Pt"
//   iaeq (inline-array box): `var a: [3]int := [1,2,3]; var oa: Object := a;
//                            var s: Object := "x"; oa.Equals(s);`
//     → "global-buffer-overflow ... in bz$iaeq$_3_int"
//
// THE FIX. Each thunk now does ONE load of `other`'s word 0 and ONE load of
// `self`'s word 0, comparing the vtable pointers directly — no second
// dereference of `other`. This is exactly as discriminating as comparing the
// descriptors: a box's word 0 IS its vtable, and the vtable uniquely identifies
// the type (for a generic record each instantiation mints its own vtable via
// specializedBoxVtableFor, so #1939 leg 2's invariant — same-instantiation boxes
// match, sibling instantiations do not — holds under vtable identity exactly as
// it did under descriptor identity). This is the same one-load fix bz$uniteq
// already took; bz$uniteq compared against a static global because the unit type
// has no instantiations, while `self`'s own word 0 is the general form that
// works uniformly for all three sibling thunks.
//
// WHAT EACH TEST SHOWS. Each test calls `<boxedValue>.Equals(<string>)` — the
// shape that reaches the thunk with a `%bz.string` operand. The comparison must
// return false (a primitive/record/inline-array box is never equal to a string).
// Under the PLAIN oracle the test passes both with and without the fix (the
// overflow reads mapped memory and yields false silently — the same shape that
// made bz$uniteq "structurally invisible to the gate, however many times it
// ran"). Under the ASan oracle (`--full` / BEZEL_AOT_FORCE_SANITIZE=address),
// pre-fix the program aborts with global-buffer-overflow before reaching the
// return; post-fix it returns the expected discriminator. The positive controls
// (same-type boxes DO equal) verify the fix did not break equality.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// Wraps a method body in the standard module/class/Main shell.
std::string prog(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " end Main; end C;";
}

// A record declaration prepended to the module when the beq thunk is exercised.
std::string progWithRecord(const std::string& body) {
    return "module M; record Pt(X: int, Y: int); "
           "class C public static method Main(): int " + body +
           " end Main; end C;";
}

}  // namespace

// =====================================================================
// #2980 / §8.3 — peq (primitive box) thunk: a boxed primitive's Equals must
// safely reject a `%bz.string` operand. Pre-fix: global-buffer-overflow in
// bz$primeq$int under ASan.
// =====================================================================

BEZEL_TEST(codegen_box_equals_thunk_2980,
           primitive_box_equals_string_operand_spec_8_3) {
    // bit 1: positive — two boxed ints of the same value ARE equal.
    // bit 4: negative — a boxed int is NOT equal to a string (must NOT fire).
    // Want 1. Under ASan pre-fix the program aborts on i.Equals(s) before
    // returning; post-fix it returns 1.
    JitResult r = runAot(prog(
        "var i: Object := 7; "
        "var i2: Object := 7; "
        "var s: Object := \"x\"; "
        "var acc := 0; "
        "if i.Equals(i2) then acc := acc + 1; end; "
        "if i.Equals(s) then acc := acc + 4; end; "
        "return acc;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// =====================================================================
// #2980 / §8.3 — beq (record box) thunk: a boxed record's Equals must safely
// reject a `%bz.string` operand. Pre-fix: global-buffer-overflow in
// bz$boxeq$Pt under ASan.
// =====================================================================

BEZEL_TEST(codegen_box_equals_thunk_2980,
           record_box_equals_string_operand_spec_8_3) {
    // bit 1: positive — two boxed records of the same value ARE equal.
    // bit 4: negative — a boxed record is NOT equal to a string.
    // Want 1. Under ASan pre-fix the program aborts on p.Equals(s); post-fix 1.
    JitResult r = runAot(progWithRecord(
        "var p: Object := new Pt(1, 2); "
        "var p2: Object := new Pt(1, 2); "
        "var s: Object := \"x\"; "
        "var acc := 0; "
        "if p.Equals(p2) then acc := acc + 1; end; "
        "if p.Equals(s) then acc := acc + 4; end; "
        "return acc;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// =====================================================================
// #2980 / §8.3 — iaeq (inline-array box) thunk: a boxed inline array's Equals
// must safely reject a `%bz.string` operand. Pre-fix: global-buffer-overflow in
// bz$iaeq$_3_int under ASan.
// =====================================================================

BEZEL_TEST(codegen_box_equals_thunk_2980,
           inline_array_box_equals_string_operand_spec_8_3) {
    // bit 1: positive — two boxed inline arrays of the same values ARE equal.
    // bit 4: negative — a boxed inline array is NOT equal to a string.
    // Want 1. Under ASan pre-fix the program aborts on oa.Equals(s); post-fix 1.
    JitResult r = runAot(prog(
        "var a: [3]int := [1, 2, 3]; "
        "var b: [3]int := [1, 2, 3]; "
        "var oa: Object := a; "
        "var ob: Object := b; "
        "var s: Object := \"x\"; "
        "var acc := 0; "
        "if oa.Equals(ob) then acc := acc + 1; end; "
        "if oa.Equals(s) then acc := acc + 4; end; "
        "return acc;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// =====================================================================
// #2980 / §8.3 — cross-kind discrimination: a boxed value must not equal a
// box of a DIFFERENT value kind either. This pins the type-discrimination the
// fix preserves (comparing vtable pointers instead of descriptors is exactly
// as discriminating — a different box kind carries a different vtable). If a
// future change made the vtable comparison accept a wrong-kind operand, this
// test fails.
// =====================================================================

BEZEL_TEST(codegen_box_equals_thunk_2980,
           box_equals_rejects_different_box_kind_spec_8_3) {
    // bit 1: same-primitive positive (int = int).
    // bit 2: same-record positive (Pt = Pt).
    // bit 4: cross-kind — int box vs record box (must NOT fire).
    // bit 8: cross-kind — record box vs int box (must NOT fire).
    // Want 3 (1 + 2).
    JitResult r = runAot(progWithRecord(
        "var i: Object := 7; "
        "var i2: Object := 7; "
        "var p: Object := new Pt(1, 2); "
        "var p2: Object := new Pt(1, 2); "
        "var acc := 0; "
        "if i.Equals(i2) then acc := acc + 1; end; "
        "if p.Equals(p2) then acc := acc + 2; end; "
        "if i.Equals(p) then acc := acc + 4; end; "
        "if p.Equals(i) then acc := acc + 8; end; "
        "return acc;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}
