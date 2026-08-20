// #2008 (§12.9 type-test invariance, §2.8.3 rule 4 monomorphization, §15.1 subtyping,
// v0.197) — an upcast runtime `is`/`as` through a GENERIC base class or
// interface whose type ARGUMENT is a folded-size inline array must match.
//
// The defect (a completeness regression introduced by #2001): #2001 encoded an
// inline-array type argument's size N in the descriptor key and made the
// top-level concreteness gate (descArgsConcrete / descTypeArgConcrete)
// inline-array-aware, so `D<[3]int>` mints a per-instantiation descriptor. But
// instanceDescriptorFor's super-chain phase still computed each node's
// concreteness with typeRefIdentityToken (which returns nullptr for an
// InlineArray). So `D<[3]int>`'s super-chain recorded its base B as
// NON-concrete and stored the shared, erased B descriptor — while the target
// `is B<[3]int>` resolves B's PER-INSTANTIATION descriptor (a distinct global).
// The two never met: `D<[3]int> is B<[3]int>` returned FALSE (should be TRUE);
// `is D<[3]int>` (self, top-level gate) was correctly TRUE. Same for a generic
// interface (`K<T> : IMarker<T>`; `K<[3]int> is IMarker<[3]int>` was FALSE).
//
// The direction is SAFE — a false NEGATIVE, not a type confusion: the
// memory-unsafe cases (`D<[3]int> is B<[4]int>`) stayed correctly FALSE because
// the descriptor keys still differ by N. The fix uses the same descTypeArgConcrete
// predicate in the super-chain phase, so an inline-array base arg is concrete and
// the base descriptor is minted per-instantiation, matching the target's global.
//
// Tests observe a bool via the process exit code (1 = true, 0 = false).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// A 3-level generic chain E<T> : D<T> : B<T> (B abstract; T is phantom, never
// stored — isolates the descriptor-key / super-chain minting from field layout),
// a generic interface IMarker<T> with an implementer K<T>, and a Box<U> used only
// as a NESTED type argument. Each carries an int Tag so instances have state and
// inherited GetTag() exercises a method call across the specialized chain.
int runProg(const std::string& mainBody) {
    return run(
        "module M; "
        "abstract class B<T> public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "  public method GetTag(): int return Tag; end GetTag; end B; "
        "class D<T> : B<T> "
        "  public constructor(t: int) base(t); end constructor; end D; "
        "sealed class E<T> : D<T> "
        "  public constructor(t: int) base(t); end constructor; end E; "
        "interface IMarker<T> end IMarker; "
        "sealed class K<T> : IMarker<T> public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; end K; "
        "class Box<U> public V: U; "
        "  public constructor(v: U) V := v; end constructor; end Box; "
        "class C public static method Main(): int " + mainBody +
        " end Main; end C;");
}

}  // namespace

// ---- THE FIX (fail-without / pass-with): upcast through a GENERIC BASE class
// with an inline-array arg now matches. `D<[3]int> is B<[3]int>` == TRUE.
// FAILS WITHOUT the fix (super-chain recorded the shared, erased B descriptor;
// the target resolved B<[3]int>'s per-instantiation global → returned FALSE).
BEZEL_TEST(codegen_2008, base_class_inline_array_is_true) {
    BEZEL_EXPECT_EQ(runProg(
        "using var d := new D<[3]int>(7); "
        "var o: Object := d; "
        "return (if o is B<[3]int> then 1 else 0);"), 1);
}

// Companion for a GENERIC INTERFACE: `K<[3]int> is IMarker<[3]int>` == TRUE.
// Also FAILS WITHOUT the fix (same super-chain concreteness gap for interfaces).
BEZEL_TEST(codegen_2008, base_iface_inline_array_is_true) {
    BEZEL_EXPECT_EQ(runProg(
        "using var k := new K<[3]int>(7); "
        "var o: Object := k; "
        "return (if o is IMarker<[3]int> then 1 else 0);"), 1);
}

// The SELF type-test was already correct (top-level gate, #2001) and stays TRUE.
BEZEL_TEST(codegen_2008, self_inline_array_is_true) {
    BEZEL_EXPECT_EQ(runProg(
        "using var d := new D<[3]int>(7); "
        "var o: Object := d; "
        "return (if o is D<[3]int> then 1 else 0);"), 1);
}

// ---- #2001 MEMORY-SAFETY PRESERVED: the wrong-size upcast stays FALSE (the key
// invariant the fix must not break — a `D<[3]int>` must never test as any
// `B<[N != 3]int>`; the descriptor keys differ by N).
BEZEL_TEST(codegen_2008, base_class_wrong_size_is_false) {
    BEZEL_EXPECT_EQ(runProg(
        "using var d := new D<[3]int>(7); "
        "var o: Object := d; "
        "return (if o is B<[4]int> then 1 else 0);"), 0);
}

// The self wrong-size test also stays FALSE (unchanged #2001 behavior).
BEZEL_TEST(codegen_2008, self_wrong_size_is_false) {
    BEZEL_EXPECT_EQ(runProg(
        "using var d := new D<[3]int>(7); "
        "var o: Object := d; "
        "return (if o is D<[4]int> then 1 else 0);"), 0);
}

// And the interface wrong-size upcast stays FALSE.
BEZEL_TEST(codegen_2008, base_iface_wrong_size_is_false) {
    BEZEL_EXPECT_EQ(runProg(
        "using var k := new K<[3]int>(7); "
        "var o: Object := k; "
        "return (if o is IMarker<[4]int> then 1 else 0);"), 0);
}

// ---- CONTROL: concrete (non-inline-array) base args are UNCHANGED — these
// worked before #2008 and must keep working. `D<int> is B<int>` == TRUE.
BEZEL_TEST(codegen_2008, base_class_concrete_arg_is_true) {
    BEZEL_EXPECT_EQ(runProg(
        "using var d := new D<int>(7); "
        "var o: Object := d; "
        "return (if o is B<int> then 1 else 0);"), 1);
}

// And a wrong concrete base arg stays FALSE (invariance, §12.9).
BEZEL_TEST(codegen_2008, base_class_concrete_wrong_arg_is_false) {
    BEZEL_EXPECT_EQ(runProg(
        "using var d := new D<int>(7); "
        "var o: Object := d; "
        "return (if o is B<string> then 1 else 0);"), 0);
}

// Concrete interface arg control: `K<int> is IMarker<int>` == TRUE.
BEZEL_TEST(codegen_2008, base_iface_concrete_arg_is_true) {
    BEZEL_EXPECT_EQ(runProg(
        "using var k := new K<int>(7); "
        "var o: Object := k; "
        "return (if o is IMarker<int> then 1 else 0);"), 1);
}

// ---- DEEPER CHAIN (E<T> : D<T> : B<T>): an E<[3]int> value tests TRUE against
// its whole ancestor chain at the right size, and FALSE at the wrong size — the
// per-instantiation base descriptor is minted for EVERY transitive base.
// (Bit-packed so the whole result stays inside the 8-bit process exit code that
// runAot observes: bits [E,D,B,bad] — all-ancestors match + wrong-size miss is
// 0b0111 = 7.)
BEZEL_TEST(codegen_2008, deep_chain_all_ancestors_inline_array) {
    BEZEL_EXPECT_EQ(runProg(
        "using var e := new E<[3]int>(7); "
        "var o: Object := e; "
        "var isE := (if o is E<[3]int> then 1 else 0); "   // self  -> bit0
        "var isD := (if o is D<[3]int> then 1 else 0); "   // base  -> bit1
        "var isB := (if o is B<[3]int> then 1 else 0); "   // grand -> bit2
        "var bad := (if o is B<[4]int> then 1 else 0); "   // wrong -> bit3 (0)
        "return isE + isD * 2 + isB * 4 + bad * 8;"), 7);
}

// ---- NESTED inline-array arg through the base: the size distinction must hold
// at depth. `D<Box<[3]int>> is B<Box<[3]int>>` == TRUE; `is B<Box<[4]int>>` FALSE.
BEZEL_TEST(codegen_2008, nested_inline_array_base_is_true_wrong_false) {
    BEZEL_EXPECT_EQ(runProg(
        "using var d := new D<Box<[3]int>>(7); "
        "var o: Object := d; "
        "var good := (if o is B<Box<[3]int>> then 1 else 0); "  // 1
        "var bad := (if o is B<Box<[4]int>> then 1 else 0); "   // 0
        "return good * 10 + bad;"), 10);
}

// ---- DEDUP / no-ODR + method calls: BOTH D<[3]int> and D<[4]int> built and used
// in ONE module. Their base B descriptors carry DISTINCT keys (differ by N), so
// they neither collide (duplicate-symbol crash) nor over-dedup; every cross
// upcast type-test resolves precisely, and the inherited GetTag() dispatches.
BEZEL_TEST(codegen_2008, specialization_split_base_descriptors_no_collision) {
    BEZEL_EXPECT_EQ(runProg(
        "using var d3 := new D<[3]int>(5); "
        "using var d4 := new D<[4]int>(9); "
        "var tags := d3.GetTag() + d4.GetTag(); "        // 14
        "var o3: Object := d3; var o4: Object := d4; "
        "var a := (if o3 is B<[3]int> then 1 else 0); "         // 1
        "var b := (if o3 is B<[4]int> then 1 else 0); "         // 0
        "var c := (if o4 is B<[4]int> then 1 else 0); "         // 1
        "var e := (if o4 is B<[3]int> then 1 else 0); "         // 0
        // 14 + 10 + 0 + 40 + 0 = 64 (kept < 256 for the 8-bit exit code).
        "return tags + 10*a + 20*b + 40*c + 80*e;"), 64);
}
