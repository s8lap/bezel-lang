// Codegen — `Distinct` / `ToSet` / `ToDictionary` over an INLINE-ARRAY element
// type (§3.6 / §15.1.2, v0.338, #6271).
//
// THE DEFECT. The lazy-LINQ machinery has TWO trampolines that a membership
// operator installs into the runtime dict: an equality one
// (`buildLinqTrampoline` kind 3, the `bz$linq$eq` slot) and a hash one
// (`buildLinqHashTrampoline`). Both dispatched on a RECORD-or-PRIMITIVE test
// and otherwise fell through to a scalar tail:
//
//   * equality: `builder_.CreateICmpEQ(a, b)` — but `ICmpInst` accepts only
//     integer, pointer and vector operands, so an aggregate reaching it trips
//     `ICmpInst::AssertOK`;
//   * hash: `builder_.CreateZExtOrBitCast(v, i64())` — and `ZExt` accepts only
//     integer and pointer operands.
//
// An inline-array element type lowers to `llvm::ArrayType` (`[N x T]`), which
// is neither a record nor a Primitive TypeRef, so BOTH fell through to their
// scalar tails and the compiler died in IR VERIFICATION — a compile-time abort,
// not a wrong answer:
//
//   Invalid operand types for ICmp instruction
//     %2 = icmp eq [2 x i32] %eq.a, %eq.b
//   ZExt only operates on integer
//     %1 = zext [2 x i32] %hash.v to i64
//   bezelc: codegen error: IR verification failed (malformed IR emitted)
//
// §3.6 (v0.338) makes an inline array of a hashable element HASHABLE, and
// §15.1.2 makes two inline arrays equal iff their elements are pairwise equal.
// §8.3's equality table states the pair for the inline-array row verbatim:
// "Inline array `[N]T`, `[N]T?` | Element-by-element value equality (§15.1.2) |
// Hash combining element hashes in slot order". So these programs are VALID and
// must compile and run.
//
// MEASURED on this fix's parent (4104a219): `Distinct()` over `Array<[2]int>`
// aborts with the `icmp` message; `ToSet()` over the same aborts with BOTH.
//
// The fix routes each half to the ONE authority that already implements the
// table's row — `emitValueEquality` (whose inline-array arm recurses slot by
// slot under the array's element type, §15.1.2) and `buildValueHashTrampoline`
// (whose `emitValueHashCode` folds the slot hashes in slot order). Those are
// the same helpers `=`, record-field equality, and the generic-dictionary key
// trampolines already use, so the membership dict agrees with the language's
// own equality instead of carrying a second, divergent one.
//
// NOTE ON SCOPE. `ToDictionary` keyed by an inline array is NOT pinned here:
// on this tree that shape is refused before codegen for two SEPARATE, already-
// tracked reasons — a collection literal written directly in an inline-array key
// position (#6237, refused with an explicit diagnostic by the #5882 work) and
// an inline-array key the dict does not retain (#6238). Neither is this issue's
// defect and neither is in scope to re-open. What this fix reaches for the
// dictionary-key path is the shared hash/equality trampolines themselves, which
// `Dictionary<[N]T, V>` exercises directly (rows below) — that spelling was
// already lowered by #5882 and is unaffected by this fix either way.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

std::string runStdout(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

}  // namespace

// §3.6 / stdlib §5: `Distinct()` over an inline-array element type COMPILES and
// deduplicates. [1,2] appears twice and [3,4] once → 2 distinct elements.
// Pre-fix: IR verification abort.
BEZEL_TEST(codegen_inline_array_hash_6271,
           distinct_over_inline_array_element_compiles_spec_3_6) {
    BEZEL_EXPECT_EQ(runStdout(
        "module M; class C public static method Main(): int "
        "  var k1: [2]int := [1, 2]; "
        "  var k2: [2]int := [3, 4]; "
        "  var a: Array<[2]int> := [k1, k2, k1]; "
        "  Console.WriteLine($\"{a.Distinct().Count()}\"); "
        "  return 0; "
        "end Main; end C;"), "2\n");
}

// stdlib §5: `ToSet()` over an inline-array element type compiles, keeps both
// distinct elements, and answers §8.3 membership correctly through the set.
// Pre-fix: BOTH the ZExt and the ICmp verification aborts, in that order.
BEZEL_TEST(codegen_inline_array_hash_6271,
           to_set_over_inline_array_element_compiles_spec_3_6) {
    BEZEL_EXPECT_EQ(runStdout(
        "module M; class C public static method Main(): int "
        "  var k1: [2]int := [1, 2]; "
        "  var k2: [2]int := [3, 4]; "
        "  var a: Array<[2]int> := [k1, k2]; "
        "  using var s: Set<[2]int> := a.ToSet(); "
        "  Console.WriteLine($\"{s.Count()} {s.Contains(k1)} {s.Contains(k2)}\"); "
        "  return 0; "
        "end Main; end C;"), "2 True True\n");
}

// §8.3: the hash and the equality must AGREE — this is the substantive
// property, and "it compiles" alone would not establish it. Three elements with
// the first duplicated: a membership dict that hashes by anything other than
// the language's equality either keeps all three or collapses the wrong pair.
BEZEL_TEST(codegen_inline_array_hash_6271,
           distinct_hash_agrees_with_element_equality_spec_8_3) {
    BEZEL_EXPECT_EQ(runStdout(
        "module M; class C public static method Main(): int "
        "  var k1: [4]int := [1, 2, 3, 4]; "
        "  var k2: [4]int := [5, 6, 7, 8]; "
        "  var k3: [4]int := [9, 10, 11, 12]; "
        "  var a: Array<[4]int> := [k1, k2, k3, k1]; "
        "  Console.WriteLine($\"{a.Distinct().Count()}\"); "
        "  return 0; "
        "end Main; end C;"), "3\n");
}

// §8.3: elements differing in ONE slot stay distinct — the equality the dict
// keys on is element-wise (§15.1.2), so a hash over whole-array bytes that
// ignored a slot would collapse these. Two elements differing only in slot 3.
BEZEL_TEST(codegen_inline_array_hash_6271,
           distinct_one_slot_difference_stays_distinct_spec_15_1_2) {
    BEZEL_EXPECT_EQ(runStdout(
        "module M; class C public static method Main(): int "
        "  var k1: [3]int := [1, 2, 3]; "
        "  var k2: [3]int := [1, 2, 4]; "
        "  var a: Array<[3]int> := [k1, k2]; "
        "  Console.WriteLine($\"{a.Distinct().Count()}\"); "
        "  return 0; "
        "end Main; end C;"), "2\n");
}

// §15.1.2 / §10.5: an ENUM element inside the inline array. The element's
// equality is the enum's, so the fold must consult the declared element type
// rather than raw bytes. `k1`/`k3` are the same enum value in every slot and
// must collapse; `k2` must not.
BEZEL_TEST(codegen_inline_array_hash_6271,
           distinct_over_enum_element_inline_array_spec_10_5) {
    BEZEL_EXPECT_EQ(runStdout(
        "module M; enum E A, B; end E; class C public static method Main(): int "
        "  var k1: [2]E := [E.A, E.B]; "
        "  var k2: [2]E := [E.A, E.A]; "
        "  var a: Array<[2]E> := [k1, k2, k1]; "
        "  Console.WriteLine($\"{a.Distinct().Count()}\"); "
        "  return 0; "
        "end Main; end C;"), "2\n");
}

// §15.1 / §9.1: a RECORD element inside the inline array — the equality the
// dict keys on recurses through the record's own structural equality, and the
// hash folds its field hashes. Records inside inline arrays are the shape the
// `emitValueEquality` route must reach rather than a raw compare.
BEZEL_TEST(codegen_inline_array_hash_6271,
           distinct_over_record_element_inline_array_spec_9_1) {
    BEZEL_EXPECT_EQ(runStdout(
        "module M; record P(X: int, Y: int); class C "
        "public static method Main(): int "
        "  var k1: [2]P := [new P(1, 2), new P(3, 4)]; "
        "  var k2: [2]P := [new P(1, 2), new P(3, 5)]; "
        "  var a: Array<[2]P> := [k1, k2, k1]; "
        "  Console.WriteLine($\"{a.Distinct().Count()}\"); "
        "  return 0; "
        "end Main; end C;"), "2\n");
}

// §4.8 / §9.2: a `Dictionary` with an inline-array KEY exercises the SAME two
// trampolines as the lazy path (the membership dict is the same runtime
// object), so this row pins that the fix reaches the dictionary shape too and
// that key lookup answers §15.1.2's element-wise question. Two equal keys
// collide on one entry; a third, different key is a second entry.
BEZEL_TEST(codegen_inline_array_hash_6271,
           dictionary_inline_array_key_lookup_spec_4_8) {
    BEZEL_EXPECT_EQ(runStdout(
        "module M; class C public static method Main(): int "
        "  var k1: [3]int := [1, 2, 3]; "
        "  var k2: [3]int := [1, 2, 3]; "
        "  var k3: [3]int := [4, 5, 6]; "
        "  var d: Dictionary<[3]int, string> := new Dictionary<[3]int, string>(); "
        "  d.Add(k1, \"a\"); "
        "  d.Add(k3, \"b\"); "
        "  Console.WriteLine($\"{d.Count()} {d.ContainsKey(k1)} {d.ContainsKey(k2)}\"); "
        "  return 0; "
        "end Main; end C;"), "2 True True\n");
}

// stdlib §5: `Distinct` after a `Select` — the pipeline is what makes the
// element an inline-array VALUE rather than the collection's declared element
// type, which is the shape the original report used. The selector uses a block
// body with an explicitly-typed local, because §15.4 gives a collection literal
// in an expression-bodied lambda no determinable target type (the selector
// returns `IEnumerable<U>`).
BEZEL_TEST(codegen_inline_array_hash_6271,
           distinct_over_selected_pairs_spec_5) {
    BEZEL_EXPECT_EQ(runStdout(
        "module M; class C public static method Main(): int "
        "  var a: Array<int> := [1, 2, 3, 4]; "
        "  using var pairs := a.Select(lambda(x: int) => do "
        "    var p: [2]int := [x, x * 2]; return p; end); "
        "  Console.WriteLine($\"{pairs.Distinct().Count()}\"); "
        "  return 0; "
        "end Main; end C;"), "4\n");
}
