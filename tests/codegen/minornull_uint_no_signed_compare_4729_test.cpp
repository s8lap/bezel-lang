// fix #4729 (spec §29.1, stdlib §5.1, v0.305): `MinOrNull` /
// `MaxOrNull` over `IEnumerable<T> where T: IComparable<T>` on unsigned
// numeric elements must compare UNSIGNED. §5.1's declaration:
//
//     public method MaxOrNull(): T?;   // null if empty
//
// returns the ELEMENT type `T` packed into `T?` — for `Array<uint>` the
// returned `uint?` carries the BIT PATTERN of the maximum element, NOT
// a signed interpretation of those bits.
//
// THE DEFECT — `src/codegen/LlvmBackend_Calls.cpp` line ~4390-4393's
// numeric-element branch unconditionally builds `builder_.CreateICmpSGT`
// / `builder_.CreateICmpSLT`, which is a SIGNED predicate regardless
// of the element's primitive kind. The legacy Min/Max arm above already
// fixes this via `accIsUnsigned` (`isUnsignedPrim(retP)`) at line ~4229-
// 4236 — but the `*OrNull` sibling, added in #2783, did not carry the
// same dispatch.
//
// MEASURED pre-fix on this tree: `Array<uint>{1u, 2u, 3u, 4294967295u}`:
//   * MaxOrNull → 3         (the largest SIGNED i32 — -1 < 1 < 2 < 3)
//   * MinOrNull → 4294967295 (the value of -1 when printed as uint —
//                             i.e. the SIGNED-minimum element, not the
//                             UNSIGNED-minimum)
// Spec requires:
//   * MaxOrNull → 4294967295 (the UNSIGNED-maximum = 0xFFFFFFFF)
//   * MinOrNull → 1          (the UNSIGNED-minimum)
// The two values differ in BOTH slots, and the printed form of -1 happens
// to coincide with the post-fix MaxOrNull, so a pinned test must check
// BOTH slots in a known order rather than relying on the bug's
// "min-or-max-as-unsigned happens to match a print coincidence".
//
// FAIL-WITHOUT protocol (CLAUDE.md "Never git stash"): copy
// `src/codegen/LlvmBackend_Calls.cpp` to `/var/tmp/bezel-scratch`, run
// `git checkout -- src/codegen/LlvmBackend_Calls.cpp` to revert to the
// parent's always-signed `*OrNull` predicate, rebuild, re-run this
// filter, confirm the captured stdout flips from `4294967295\n1\n` to
// `3\n4294967295\n`. Then restore the fix and rebuild.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

std::string progU32() {
    return std::string(
        "module M;\n"
        "import System;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var xs := new Array<uint>();\n"
        "    xs.Add(1u); xs.Add(2u); xs.Add(3u); xs.Add(4294967295u);\n"
        "    Console.WriteLine($\"{xs.MaxOrNull()}\");\n"
        "    Console.WriteLine($\"{xs.MinOrNull()}\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
}

}  // namespace

// §29.1 / stdlib §5.1, v0.305: `Array<uint>.MinOrNull()` /
// `.MaxOrNull()` returns `uint?` whose value field is the UNSIGNED
// extreme. Pre-fix prints `3\n4294967295\n` (signed comparison: -1 < 1,
// so 3 wins Max; -1 wins Min and prints as 4294967295 — bit pattern
// matches but the semantics are inverted).
BEZEL_TEST(minornull_uint_no_signed_compare_4729,
           uint_minmax_ornull_compare_unsigned) {
    BEZEL_EXPECT_EQ(runAotOut(progU32()), "4294967295\n1");
}

// Sibling — `Array<ushort>`. { 60000, 0, 1, 65535 }. Unsigned max = 65535,
// unsigned min = 0. Pre-fix: 65535 as i16 is -1; -1 < 0 < 1 < 60000, so
// 60000 wins Max and -1 wins Min (prints as 65535 — coincidental print
// match for Max only, NOT for Min which is 0 in both forms — so the
// test pins BOTH slots and confirms the bug-shape extends across widths).
BEZEL_TEST(minornull_uint_no_signed_compare_4729,
           ushort_minmax_ornull_compare_unsigned) {
    BEZEL_EXPECT_EQ(runAotOut(std::string(
        "module M;\n"
        "import System;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var xs := new Array<ushort>();\n"
        "    xs.Add(ushort(60000)); xs.Add(ushort(0)); xs.Add(ushort(1)); xs.Add(ushort(65535));\n"
        "    Console.WriteLine($\"{xs.MaxOrNull()}\");\n"
        "    Console.WriteLine($\"{xs.MinOrNull()}\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n")), "65535\n0");
}

// Sibling — `Array<byte>`. { 200, 50, 0, 255 }. Unsigned max = 255,
// unsigned min = 0. Pre-fix: 200 as i8 is -56, 50 as i8 is 50, 255 as i8
// is -1, so signed order is { -56, -1, 0, 50 } and 50 wins Max while -56
// wins Min (prints as 200). Pin both slots at a NARROW width too, so the
// fix can't accidentally leave `byte` behind.
BEZEL_TEST(minornull_uint_no_signed_compare_4729,
           byte_minmax_ornull_compare_unsigned) {
    BEZEL_EXPECT_EQ(runAotOut(std::string(
        "module M;\n"
        "import System;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var xs := new Array<byte>();\n"
        "    xs.Add(byte(200)); xs.Add(byte(50)); xs.Add(byte(0)); xs.Add(byte(255));\n"
        "    Console.WriteLine($\"{xs.MaxOrNull()}\");\n"
        "    Console.WriteLine($\"{xs.MinOrNull()}\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n")), "255\n0");
}
