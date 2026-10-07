// Codegen / leak — an expression-position `case`/`when` whose selected arm
// MINTS a fresh owned `string` (`"a" + "b"`) leaked that string when the
// case-expression result was consumed (returned / assigned). Issue #1934.
//
// Spec: §19.2 (case/when arms; "the ownership of the case/when result follows
// the arm-meet rule of §28.3's producers table", v0.184), §28.3 Producers
// (`v1 if c else v2` / expression-position `case`/`when` row: the result is the
// meet of the arms — owned ⊓ owned = owned, exactly one arm's value produced at
// run time and discharged once), §28.9 (runtime-managed `string` refcount
// discipline). Spec revision v0.197.
//
// THE BUG. A consumer of a string value (return / assign / bind) decides once,
// for the whole value, whether it is a +1 PRODUCER (transfer the reference) or a
// +0 BORROW (mint one via bz_string_retain). `isPlusOneStringProducer` did not
// recognize a `case`/`when` expression, so a returned/assigned case result was
// treated as a borrow and RETAINED — but the taken arm (`"a" + "b"`) had already
// minted +1. The extra retain left the arm's original reference undischarged: it
// leaked once per evaluation (LSan: 32-byte header + 3-byte buffer). A plain
// `return <concat>` is clean because the ReturnStmt path recognizes the concat
// as a producer and transfers it — the gap was specific to the case-expression
// RESULT temporary.
//
// THE FIX (two coordinated halves, mirroring the single-expression consumer
// normalization applied per ARM because a PHI merges the arms into one value):
//   (1) lowerCaseExpr (LlvmBackend_Exprs2.cpp): for a non-nullable owned
//       `string` result, normalize each arm to own EXACTLY +1 in the arm's own
//       block — a producer arm already carries its reference (leave it); any
//       other arm (a param/field borrow, a string-literal sentinel) is retained.
//   (2) isPlusOneStringProducer (LlvmBackend_Setup.cpp): recognize a
//       non-nullable-`string` `case`/`when` as a +1 producer, so the consumer
//       TRANSFERS the (now uniformly +1) result instead of a second retain.
// Both are required: (2) alone would drop a borrow arm's reference (UAF); (1)
// alone would still let the consumer double-retain a producer arm (the leak).
//
// OBSERVABILITY. runAotChecked links + runs the generated code under ASan/LSan;
// a leaked string (or a double-free / UAF from over-releasing a borrow arm)
// flips r.ran to false. The exit code pins the correct string result (via
// `.Length`), so a premature free would corrupt it as well as flip r.ran. Each
// test FAILS without the fix (LSan reports the leaked arm string) and PASSES
// with it.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is absent:
// runAotChecked returns ran=false with a "SKIP" diagnostic. clang is required to
// build this repo, so a skip is not expected — guard anyway.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

bool diagContains(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// (A) The exact #1934 repro: a `case` EXPRESSION arm mints `"a" + "b"` and the
// result is RETURNED. Main consumes the returned string (WriteLine surfaces an
// otherwise-register-masked leak; `.Length` pins the value = 2). Pre-fix the
// minted "ab" leaked inside Pick.
const char* kReturnProducerArm =
    "module M; class C "
    "  public static method Pick(n: int): string "
    "    return (case n of "
    "      1 => \"a\" + \"b\", "   // fresh owned string minted in the arm
    "      _ => \"other\"); "
    "  end Pick; "
    "  public static method Main(): int "
    "    Console.WriteLine(C.Pick(1)); "   // consumes one returned +1
    "    return C.Pick(1).Length; "        // 2 = \"ab\".Length; consumes another
    "  end Main; "
    "end C;";

// (B) MIXED arms: a producer arm (`"a" + "b"`) and a BORROW arm (`p`, a
// parameter). This proves the per-arm +1 normalization keeps the borrow arm
// sound: Pick(2,p) returns a retained borrow (no UAF / double-free of `p`),
// Pick(1,p) returns the minted concat (no leak). A producer-only fix WITHOUT the
// per-arm mint would over-transfer the borrow arm and double-free `p`. Value:
// "ab".Length (2) + "borrowed".Length (8) = 10.
const char* kMixedBorrowAndProducerArms =
    "module M; class C "
    "  public static method Pick(n: int, p: string): string "
    "    return (case n of "
    "      1 => \"a\" + \"b\", "   // producer arm (+1)
    "      _ => p "                // borrow arm (+0) — must be retained
    "); "
    "  end Pick; "
    "  public static method Main(): int "
    "    var p := \"borrowed\"; "
    "    Console.WriteLine(C.Pick(2, p)); "   // borrow arm taken
    "    Console.WriteLine(C.Pick(1, p)); "   // producer arm taken
    "    return C.Pick(1, p).Length + C.Pick(2, p).Length; "  // 2 + 8 = 10
    "  end Main; "
    "end C;";

// (C) The ASSIGN path: the case result is BOUND to a plain `var` and then
// returned (a consuming context). Exercises the local-decl producer
// classification (collectStringReleaseEligible) + transfer, not the ReturnStmt
// path directly.
const char* kBindThenReturn =
    "module M; class C "
    "  public static method Pick(n: int): string "
    "    var s := (case n of "
    "      1 => \"a\" + \"b\", "
    "      _ => \"other\"); "
    "    return s; "
    "  end Pick; "
    "  public static method Main(): int "
    "    return C.Pick(1).Length; "   // 2
    "  end Main; "
    "end C;";

// (D) The scrutinee-less sibling `when` (§19.1) — the same §28.3 arm-meet rule
// (§19.2, "identical to ?? and the if expression"). A `when` arm mints
// `"a" + "b"`; the result is returned and consumed.
const char* kWhenProducerArm =
    "module M; class C "
    "  public static method Pick(n: int): string "
    "    return (when "
    "      n = 1 => \"a\" + \"b\", "
    "      _ => \"other\"); "
    "  end Pick; "
    "  public static method Main(): int "
    "    Console.WriteLine(C.Pick(1)); "
    "    return C.Pick(1).Length; "   // 2
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// (A) #1934 repro — a returned `case`-expression producer arm is LSan-clean.
// §19.2 / §28.3 (producers arm-meet, v0.184) / §28.9.
// =====================================================================
BEZEL_TEST(case_expr_string_result_leak_1934,
           return_case_producer_arm_lsan_clean_spec_19_2) {
    JitResult r = runAotChecked(kReturnProducerArm);
    if (wasSkipped(r)) return;   // ASan toolchain unavailable — skip, not a fail
    BEZEL_EXPECT_TRUE(r.ran);        // compiled + LSan-clean (arm string freed once)
    BEZEL_EXPECT_EQ(r.exitCode, 2);  // "ab".Length
}

// =====================================================================
// (B) Mixed producer + borrow arms — leak-clean AND double-free/UAF-clean.
// Proves the per-arm +1 mint keeps a borrow arm sound. §28.3 arm-meet.
// =====================================================================
BEZEL_TEST(case_expr_string_result_leak_1934,
           mixed_borrow_and_producer_arms_no_uaf_spec_28_3) {
    JitResult r = runAotChecked(kMixedBorrowAndProducerArms);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);         // no leak (producer arm), no double-free (borrow arm)
    BEZEL_EXPECT_EQ(r.exitCode, 10);  // "ab"(2) + "borrowed"(8)
}

// =====================================================================
// (C) The assign path — a bound-then-returned case result is LSan-clean.
// §28.3 producers / §28.9.
// =====================================================================
BEZEL_TEST(case_expr_string_result_leak_1934,
           bind_then_return_case_result_lsan_clean_spec_28_3) {
    JitResult r = runAotChecked(kBindThenReturn);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}

// =====================================================================
// (D) The scrutinee-less `when` sibling — same arm-meet rule, same clean result.
// v0.289 (§19, §22.4, Appendix A.6/A.7, proposal 0122 L1): the `when`-led
// expression is retired — this test now verifies rejection.
// =====================================================================
BEZEL_TEST(case_expr_string_result_leak_1934,
           return_when_producer_arm_lsan_clean_spec_19_1) {
    JitResult r = runAotChecked(kWhenProducerArm);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "a `when`-led expression is retired"));
}
