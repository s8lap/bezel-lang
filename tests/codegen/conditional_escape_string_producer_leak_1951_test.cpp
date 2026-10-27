// Codegen / leak — a `string` PRODUCER local that escapes via ONE arm of an
// expression-position `case`/`when` or if-expression (`1 => s`) is dropped from
// scope-end release on ALL paths, so the NON-escaping path leaks it. Issue #1951
// (conditional-escape; the same §28.3 arm-meet family as #1934/#1948/#1952).
//
// Spec: §19.2 (case/when arm-meet), §20 (if-expression), §28.3 Producers (the
// arm-meet row; "an UNMARKED read of a named owned binding written as an arm is
// a BORROW arm" — K3.13 no-silent-consume), §28.9 (runtime-managed `string`
// refcount). Spec revision v0.220.
//
// THE BUG. `var s := "x" + "y";` binds a +1 producer string local. Written as a
// `case`/`if` arm result (`1 => s`), codegen TRANSFERRED s's single reference out
// of the slot (the removed `returnedStringLocalOwnsPlusOne` transfer-guard) and
// the StringEscapeWalker marked s as ESCAPING — so its scope-end release was
// suppressed on EVERY path. But the arm selects `s` on ONE path only: on every
// OTHER path (`_ => "other"`) s is never read, its transfer never runs, and the
// suppressed release never frees it → s leaked (LSan: 32-byte header + 3-byte
// buffer). §28.3 makes an unmarked named-binding arm a BORROW arm (no silent
// consume), so `s` must remain owned by its slot and be discharged once.
//
// THE FIX (two coordinated halves). (1) lowerCaseExpr / lowerConditional: RETAIN
// a bare read of a string local written as an arm/branch (mint a fresh +1 for
// the arm result), leaving s's own +1 in its slot — instead of transferring it.
// (2) StringEscapeWalker: for a `string`/`string?`-result arm-meet, a bare
// arm/branch read of the tracked local is a BORROW (not an escape), so the local
// stays scope-end-release eligible. Keyed on the SAME string-result test, the
// retain and the walker relaxation move in lockstep: s is discharged once by its
// scope-end release on the non-selected paths and by the consumer of the
// retained arm result on the selected path — balanced on every path, no leak, no
// double-free. The ReturnStmt/arrow/lambda paths keep the TRANSFER for an
// UNCONDITIONAL escape (`return s`), where the single reference genuinely flows
// out and the local is (correctly) not released.
//
// OBSERVABILITY. runAotChecked links + runs under ASan/LSan. The leak cases call
// the producer IN A LOOP so N evaluations leak N strings (unmistakable); the
// escaping/mixed cases pin exit codes so a premature free (double-free/UAF) would
// corrupt the value as well as trip ASan.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// (A) #1951 repro — the NON-escaping path. Pick(2) takes `_ => "other"`, so the
// producer local `s` ("xy") is never read on that path; pre-fix its suppressed
// scope-end release left it leaking once per call. Looped 5x. total = 5 *
// "other".Length (5) = 25.
const char* kCaseNonEscapingPathLoop =
    "module M; class C "
    "  public static method Pick(n: int): string "
    "    var s := \"x\" + \"y\"; "
    "    return (case n of 1 => s, _ => \"other\"); "
    "  end Pick; "
    "  public static method Main(): int "
    "    var total := 0; var i := 0; "
    "    while i < 5 do total := total + C.Pick(2).Length; i := i + 1; end; "
    "    return total; "                    // 25
    "  end Main; "
    "end C;";

// (B) The ESCAPING path — Pick(1) selects `1 => s`, returning the producer
// local. Proves the retain-not-transfer keeps it sound: no double-free (s's slot
// release + the returned reference balance to one free per call). Looped 5x.
// total = 5 * "xy".Length (2) = 10.
const char* kCaseEscapingPathLoop =
    "module M; class C "
    "  public static method Pick(n: int): string "
    "    var s := \"x\" + \"y\"; "
    "    return (case n of 1 => s, _ => \"other\"); "
    "  end Pick; "
    "  public static method Main(): int "
    "    var total := 0; var i := 0; "
    "    while i < 5 do total := total + C.Pick(1).Length; i := i + 1; end; "
    "    return total; "                    // 10
    "  end Main; "
    "end C;";

// (C) The if-expression sibling (§20), non-escaping path. Same shape, `(if n = 1
// then s else "other")`, Pick(2) → "other". total = 5 * 5 = 25.
const char* kIfExprNonEscapingPathLoop =
    "module M; class C "
    "  public static method Pick(n: int): string "
    "    var s := \"x\" + \"y\"; "
    "    return (if n = 1 then s else \"other\"); "
    "  end Pick; "
    "  public static method Main(): int "
    "    var total := 0; var i := 0; "
    "    while i < 5 do total := total + C.Pick(2).Length; i := i + 1; end; "
    "    return total; "                    // 25
    "  end Main; "
    "end C;";

// (D) MIXED arms: a producer LOCAL (`s`), a param BORROW (`p`), and a string
// LITERAL. Exercises all three arm kinds through one case-expression — the
// producer local is retained + released, the param borrow is retained (no UAF /
// double-free of `p`), the literal is a sentinel no-op. Value (single calls):
// "xy"(2) + "borrowed"(8) + "other"(5) = 15.
const char* kMixedProducerBorrowLiteralArms =
    "module M; class C "
    "  public static method Pick(n: int, p: string): string "
    "    var s := \"x\" + \"y\"; "
    "    return (case n of 1 => s, 2 => p, _ => \"other\"); "
    "  end Pick; "
    "  public static method Main(): int "
    "    var p := \"borrowed\"; "
    "    Console.WriteLine(C.Pick(1, p)); "   // producer-local arm
    "    Console.WriteLine(C.Pick(2, p)); "   // param-borrow arm
    "    Console.WriteLine(C.Pick(3, p)); "   // literal arm
    "    return C.Pick(1, p).Length + C.Pick(2, p).Length + C.Pick(3, p).Length; "
    "  end Main; "                            // 2 + 8 + 5 = 15
    "end C;";

}  // namespace

// =====================================================================
// (A) #1951 repro — the non-escaping path is LSan-clean. §19.2 / §28.3.
// =====================================================================
BEZEL_TEST(conditional_escape_string_producer_leak_1951,
           case_non_escaping_path_loop_lsan_clean_spec_28_3) {
    JitResult r = runAotChecked(kCaseNonEscapingPathLoop);
    BEZEL_EXPECT_TRUE(r.ran);          // 5 producer locals each freed once
    BEZEL_EXPECT_EQ(r.exitCode, 25);   // 5 * "other".Length
}

// =====================================================================
// (B) The escaping path — retain-not-transfer, no double-free. §28.3 arm-meet.
// =====================================================================
BEZEL_TEST(conditional_escape_string_producer_leak_1951,
           case_escaping_path_loop_no_double_free_spec_28_3) {
    JitResult r = runAotChecked(kCaseEscapingPathLoop);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);   // 5 * "xy".Length
}

// =====================================================================
// (C) The if-expression sibling — non-escaping path LSan-clean. §20 / §28.3.
// =====================================================================
BEZEL_TEST(conditional_escape_string_producer_leak_1951,
           if_expr_non_escaping_path_loop_lsan_clean_spec_20) {
    JitResult r = runAotChecked(kIfExprNonEscapingPathLoop);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 25);
}

// =====================================================================
// (D) Mixed producer-local / param-borrow / literal arms — leak-clean AND
// double-free/UAF-clean across all three arm kinds. §28.3 arm-meet.
// =====================================================================
BEZEL_TEST(conditional_escape_string_producer_leak_1951,
           mixed_producer_borrow_literal_arms_no_uaf_spec_28_3) {
    JitResult r = runAotChecked(kMixedProducerBorrowLiteralArms);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 15);   // "xy"(2) + "borrowed"(8) + "other"(5)
}
