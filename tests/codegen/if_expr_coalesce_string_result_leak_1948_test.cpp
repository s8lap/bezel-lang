// Codegen / leak — an if-expression (§20) or a null-coalescing `??` (§7.7)
// whose selected arm MINTS a fresh owned `string` (`"a" + "b"`) leaked that
// string when the result was consumed (returned). Issue #1948 — the SIBLING of
// #1934 (which fixed the expression-position `case`/`when` result). §28.3's
// producers table ties all three forms to ONE arm-meet rule.
//
// Spec: §20 (the if-expression; "(if c then v1 else v2)" producers row),
// §7.7 (null-coalescing `a ?? b`; "owned ?? owned is owned … the chosen value
// is discharged once"), §28.3 Producers (`a ?? b` row: "the meet of the
// ownership states of a and b … owned ⊓ owned = owned (exactly one of the two
// owned values is produced at runtime and is discharged once — no leak, no
// double-free)"; the `(if c then v1 else v2)` row: "identically to the a ?? b
// row above"), §28.9 (runtime-managed `string` refcount). Spec revision v0.220.
//
// THE BUG. A consumer of a string value (return / assign / bind) decides once,
// for the whole merged (PHI) value, whether it is a +1 PRODUCER (transfer the
// reference) or a +0 BORROW (mint one via bz_string_retain).
// `isPlusOneStringProducer` did not recognize a `ConditionalExpr` or a `??`
// `BinaryExpr`, so a returned/assigned if-expr/`??` result was treated as a
// borrow and RETAINED — but the taken arm (`"a" + "b"`) had already minted +1.
// The extra retain left the arm's original reference undischarged: it leaked
// once per evaluation (LSan: 32-byte header + 3-byte buffer). A plain
// `return <concat>` is clean because the ReturnStmt path recognizes the concat
// as a producer and transfers it — the gap was specific to the if/`??` RESULT
// temporary, identical to the #1934 case-expression gap.
//
// THE FIX (two coordinated halves, mirroring #1934's lowerCaseExpr exactly, per
// arm because a PHI merges the arms into one value):
//   (1) lowerConditional / lowerCoalesce: for a non-nullable owned `string`
//       result, normalize each branch/arm to own EXACTLY +1 in its own block —
//       a producer arm keeps its reference (leave it); any other arm (a
//       param/field borrow, a string-literal sentinel) is retained.
//   (2) isPlusOneStringProducer: recognize a non-nullable-`string`
//       `ConditionalExpr` and a non-nullable-`string` `??` as +1 producers, so
//       the consumer TRANSFERS the (now uniformly +1) result instead of a
//       second retain.
// Both are required: (2) alone would drop a borrow arm's reference (UAF); (1)
// alone would still let the consumer double-retain a producer arm (the leak).
//
// OBSERVABILITY. runAotChecked links + runs the generated code under ASan/LSan;
// a leaked string (or a double-free / UAF from over-releasing a borrow arm)
// flips r.ran to false. The exit code pins the correct string result (via
// `.Length`), so a premature free would corrupt it as well. Each test FAILS
// without the fix (LSan reports the leaked arm string) and PASSES with it.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// (A) #1948 repro — an `if` EXPRESSION whose then-branch mints `"a" + "b"` and
// the result is RETURNED. Pre-fix the minted "ab" leaked inside Pick.
const char* kIfExprProducerBranch =
    "module M; class C "
    "  public static method Pick(n: int): string "
    "    return (if n = 1 then \"a\" + \"b\" else \"other\"); "
    "  end Pick; "
    "  public static method Main(): int "
    "    Console.WriteLine(C.Pick(1)); "   // consumes one returned +1
    "    return C.Pick(1).Length; "        // 2 = \"ab\".Length; consumes another
    "  end Main; "
    "end C;";

// (B) MIXED if-branches: a producer branch (`"a" + "b"`) and a BORROW branch
// (`p`, a parameter). Proves the per-branch +1 normalization keeps the borrow
// branch sound (no UAF / double-free of `p`) while the producer branch does not
// leak. Value: "ab".Length (2) + "borrowed".Length (8) = 10.
const char* kIfExprMixedBranches =
    "module M; class C "
    "  public static method Pick(n: int, p: string): string "
    "    return (if n = 1 then \"a\" + \"b\" else p); "
    "  end Pick; "
    "  public static method Main(): int "
    "    var p := \"borrowed\"; "
    "    Console.WriteLine(C.Pick(2, p)); "   // borrow branch taken
    "    Console.WriteLine(C.Pick(1, p)); "   // producer branch taken
    "    return C.Pick(1, p).Length + C.Pick(2, p).Length; "  // 2 + 8 = 10
    "  end Main; "
    "end C;";

// (C) The `??` RHS-producer form (the exact #1948 null-coalescing repro): the
// LHS is null so the fresh `"a" + "b"` RHS is produced and returned. Pre-fix the
// minted "ab" leaked inside Pick.
const char* kCoalesceRhsProducer =
    "module M; class C "
    "  public static method Pick(p: string?): string "
    "    return p ?? (\"a\" + \"b\"); "
    "  end Pick; "
    "  public static method Main(): int "
    "    Console.WriteLine(C.Pick(null)); "   // RHS producer taken
    "    return C.Pick(null).Length; "        // 2 = \"ab\".Length
    "  end Main; "
    "end C;";

// (D) The `??` LHS-borrow form: the LHS is a NON-null HEAP string (a `+` local
// passed as a `string?`), so the coalesce yields the retained LHS borrow (no
// UAF / double-free), and the RHS producer is never evaluated. Value:
// "hi".Length (2). Exercises the entry-block LHS-arm retain.
const char* kCoalesceLhsBorrow =
    "module M; class C "
    "  public static method Pick(p: string?): string "
    "    return p ?? (\"a\" + \"b\"); "
    "  end Pick; "
    "  public static method Main(): int "
    "    var s := \"h\" + \"i\"; "            // fresh heap string (rc=1 local)
    "    Console.WriteLine(C.Pick(s)); "      // LHS borrow taken + retained
    "    return C.Pick(s).Length; "           // 2 = \"hi\".Length
    "  end Main; "
    "end C;";

// (E) The if-expression ASSIGN path: the result is BOUND to a plain `var` and
// then returned (a consuming context). Exercises the local-decl producer
// classification (collectStringReleaseEligible) + transfer.
const char* kIfExprBindThenReturn =
    "module M; class C "
    "  public static method Pick(n: int): string "
    "    var s := (if n = 1 then \"a\" + \"b\" else \"other\"); "
    "    return s; "
    "  end Pick; "
    "  public static method Main(): int "
    "    return C.Pick(1).Length; "   // 2
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// (A) #1948 repro — a returned if-expression producer branch is LSan-clean.
// §20 / §28.3 (producers arm-meet, v0.184) / §28.9.
// =====================================================================
BEZEL_TEST(if_expr_coalesce_string_result_leak_1948,
           return_if_expr_producer_branch_lsan_clean_spec_20) {
    JitResult r = runAotChecked(kIfExprProducerBranch);
    // #3733: the toolchain-SKIP guard is GONE — a SKIP (ran=false) now
    // FAILS the r.ran assertion below instead of passing silently.
    BEZEL_EXPECT_TRUE(r.ran);        // compiled + LSan-clean (arm string freed once)
    BEZEL_EXPECT_EQ(r.exitCode, 2);  // "ab".Length
}

// =====================================================================
// (B) Mixed if-branches — leak-clean AND double-free/UAF-clean. §28.3 arm-meet.
// =====================================================================
BEZEL_TEST(if_expr_coalesce_string_result_leak_1948,
           if_expr_mixed_branches_no_uaf_spec_28_3) {
    JitResult r = runAotChecked(kIfExprMixedBranches);
    // #3733: the toolchain-SKIP guard is GONE — a SKIP (ran=false) now
    // FAILS the r.ran assertion below instead of passing silently.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);  // "ab"(2) + "borrowed"(8)
}

// =====================================================================
// (C) #1948 repro — a returned `??` RHS producer is LSan-clean. §7.7 / §28.3.
// =====================================================================
BEZEL_TEST(if_expr_coalesce_string_result_leak_1948,
           return_coalesce_rhs_producer_lsan_clean_spec_7_7) {
    JitResult r = runAotChecked(kCoalesceRhsProducer);
    // #3733: the toolchain-SKIP guard is GONE — a SKIP (ran=false) now
    // FAILS the r.ran assertion below instead of passing silently.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);  // "ab".Length
}

// =====================================================================
// (D) `??` LHS borrow (heap source) — leak-clean AND no double-free of the
// retained borrow. §7.7 / §28.3 arm-meet.
// =====================================================================
BEZEL_TEST(if_expr_coalesce_string_result_leak_1948,
           coalesce_lhs_borrow_no_uaf_spec_7_7) {
    JitResult r = runAotChecked(kCoalesceLhsBorrow);
    // #3733: the toolchain-SKIP guard is GONE — a SKIP (ran=false) now
    // FAILS the r.ran assertion below instead of passing silently.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);  // "hi".Length
}

// =====================================================================
// (E) The if-expression assign path — a bound-then-returned result is
// LSan-clean. §28.3 producers / §28.9.
// =====================================================================
BEZEL_TEST(if_expr_coalesce_string_result_leak_1948,
           bind_then_return_if_expr_result_lsan_clean_spec_28_3) {
    JitResult r = runAotChecked(kIfExprBindThenReturn);
    // #3733: the toolchain-SKIP guard is GONE — a SKIP (ran=false) now
    // FAILS the r.ran assertion below instead of passing silently.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}
