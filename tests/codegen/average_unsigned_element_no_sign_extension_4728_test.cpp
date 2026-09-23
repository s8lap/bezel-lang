// fix #4728 (spec §29.1, stdlib §5 + §11.1 Average widening, v0.305):
// `IEnumerable<byte>.Average()` must NOT sign-extend its unsigned element
// through the sum-then-divide accumulator before widening to `double`.
// §5 declares `Average(): double` for every numeric primitive element;
// stdlib §11.1's narrowing/widening table promotes a `byte` element to
// `double` BEFORE the sum, not through a SIGNED `int` intermediate — the
// accumulator's job is to preserve the element's numeric value, not to
// fold it through `i32` first.
//
// THE DEFECT — `src/codegen/LlvmBackend_Calls.cpp` line ~3603-3608's
// `toDouble` lambda unconditionally calls `builder_.CreateSIToFP(v, dblTy)`,
// which reinterprets the element's bits as signed (SIToFP, not UIToFP). For
// `byte` 250 the i8 element loads as 0xFA → signed -6 → double -6.0; for
// `byte` 130 the i8 element loads as 0x82 → signed -126 → double -126.0.
// The sum folds { -6, 0, 125, -126 } = -7.0, divided by 4 = -1.75.
//
// MEASURED pre-fix on this tree: `Console.WriteLine($"{xs.Average()}")`
// over `Array<byte>{250, 0, 125, 130}` prints `-1.75`. Spec requires
// `126.25` (= 505 / 4). The two values differ by ~130 — comfortably outside
// any rounding-noise band — so the assertion pin is exact, not approximate.
//
// FAIL-WITHOUT protocol (CLAUDE.md "Never git stash" rule): copy
// `src/codegen/LlvmBackend_Calls.cpp` to `/var/tmp/bezel-scratch`, run
// `git checkout -- src/codegen/LlvmBackend_Calls.cpp` to revert to the
// parent's accumulator, rebuild `bezel_tests_codegen`, re-run this filter,
// confirm `126.25` → `-1.75` mismatch in the captured stdout. Then restore
// the fix, rebuild, and confirm `126.25` matches.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// Value channel: stdoutText carries the program's printed `double`. Pin
// the EXACT byte sequence the spec requires — `126.25` then `\n`. The
// pre-fix compiler prints `-1.75\n`, so a byte-equality check on the
// captured stdout discriminates the bug.
std::string progAvgByte() {
    return std::string(
        "module M;\n"
        "import System;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var xs := new Array<byte>();\n"
        "    xs.Add(byte(250)); xs.Add(byte(0)); xs.Add(byte(125)); xs.Add(byte(130));\n"
        "    Console.WriteLine($\"{xs.Average()}\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
}

}  // namespace

// §29.1 / stdlib §5 / §11.1, v0.305: `Array<byte>.Average()` over
// {250, 0, 125, 130}. Spec value = 505 / 4 = 126.25 (the unsigned sum,
// divided by count, widened to double). Pre-fix value = -1.75 (the
// sign-extended sum -7 / 4).
BEZEL_TEST(average_unsigned_element_no_sign_extension_4728,
           byte_array_average_is_unsigned_sum_over_count) {
    BEZEL_EXPECT_EQ(runAotOut(progAvgByte()), "126.25");
}

// Sibling — `Array<ushort>` over {60000, 0, 1} = 60001 / 3 ≈ 20000.333….
// Pre-fix: `ushort` 60000 as i16 is -5536, sum = -5536 + 0 + 1 = -5535,
// -5535 / 3 ≈ -1845. The bug surfaces at a different width family too;
// one row per width guards against a fix that only patches `byte`.
BEZEL_TEST(average_unsigned_element_no_sign_extension_4728,
           ushort_array_average_is_unsigned_sum_over_count) {
    BEZEL_EXPECT_EQ(runAotOut(std::string(
        "module M;\n"
        "import System;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var xs := new Array<ushort>();\n"
        "    xs.Add(ushort(60000)); xs.Add(ushort(0)); xs.Add(ushort(1));\n"
        "    Console.WriteLine($\"{xs.Average()}\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n")), "20000.333333333332");
}

// Sibling — `Array<uint>` over {3000000000u, 1u, 2u, 3u} =
// 3000000006 / 4 = 750000001.5. Pre-fix: 3000000000u as i32 is -1294967296,
// plus 1 + 2 + 3 = -1294967290, / 4 = -323741822.5 — far from 750000001.5.
// Same width-32 root cause, exercised at a different (negative) signed
// interpretation so the fix can't accidentally target only the smaller
// widths.
BEZEL_TEST(average_unsigned_element_no_sign_extension_4728,
           uint_array_average_is_unsigned_sum_over_count) {
    BEZEL_EXPECT_EQ(runAotOut(std::string(
        "module M;\n"
        "import System;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var xs := new Array<uint>();\n"
        "    xs.Add(3000000000u); xs.Add(1u); xs.Add(2u); xs.Add(3u);\n"
        "    Console.WriteLine($\"{xs.Average()}\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n")), "750000001.5");
}
