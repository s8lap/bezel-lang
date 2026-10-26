// Codegen — a BLOCK-bodied lambda skipped the nullable-value return coercion
// its EXPRESSION-bodied sibling performs. Issue #2716; the third body-form
// asymmetry of the same root shape after #2688 (`string`) and #2691 (closure).
//
// Spec (revision v0.240):
//   §27.3 "Lambda expressions" — a lambda's return-type ascription is a return
//     contract of its own; the two body spellings of the same lambda must
//     produce the same value and the same representation.
//   §7.2 "Nullable value types" — a nullable VALUE type `T?` has a distinct runtime
//     representation from its payload (`{ i8 hasValue; T value }`), so a bare
//     `T` reaching a `T?` return edge must be wrapped.
//   §5.1 "Implicit conversions" — a narrower numeric implicitly widens to the
//     declared type; the widen must happen BEFORE the nullable wrap, or the
//     payload width does not match.
//
// THE BUG. `lowerLambda` nulls `currentReturnTypeNode_` (#2515, for reasons
// that remain valid), and a lambda has no declared return-type NODE at all —
// so the block path's `coerceReturnToNullableValue`, which sat inside
// `if (v && rs->value && currentReturnTypeNode_)`, never ran in a lambda body.
// The expression path performs the identical promotion from `retSemaTy`
// (LlvmBackend_Exprs2.cpp, the #541 branch — numeric widen included), so the
// two forms of the same lambda disagreed.
//
// ⚠ THE ISSUE'S FILED REPRO IS STALE, and this file deliberately does not use
// it. `lambda(): int? => do return 3; end` compiles and returns 3 on the
// pre-fix compiler: `emitReturn` (LlvmBackend_Setup.cpp) has its own
// nullable-prim fallback for a payload of EXACTLY the slot width. The shapes
// that reach only `coerceReturnToNullableValue` are the ones with no such
// backstop — the #843 narrower-numeric WIDEN (`long?` from an `int` literal,
// `double?` from an `int`), the `T?`-through-monomorphisation resolve, and the
// layout-identical record payload. Verified on the parent commit:
//     lambda(): int?  => do return 3;  end   -> exit 33 (already worked)
//     lambda(): long? => do return 42; end   -> "Function return type does not
//                                               match operand type of return
//                                               inst! ret i32 42 / { i8, i64 }"
// So this file pins the WIDENING shape and keeps the exact-width shape as a
// control.
//
// THE FIX. The nullable arm falls back to `currentLambdaReturnSemaTy_` — the
// field #2688 introduced for exactly this reason — when no return-type node is
// in scope. It is the ONLY one of the four representation arms that gets the
// lambda fallback: neither lambda body form ever calls `boxForTarget` or
// `wrapAsIEnumerableForTarget`, so adding those would BREAK the agreement
// rather than restore it. The +1-mint arms are untouched.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// (A) The real repro: a `long?` block-bodied lambda returning an `int` literal.
// The payload must be widened i32 -> i64 before the `{ i8, i64 }` wrap.
const char* kBlockLongNullable =
    "module M; "
    "class P "
    "  public static method Main(): int "
    "    var f := lambda(): long? => do return 42; end; "
    "    var q := f(); "
    "    if q = null then return 9; end; "
    "    return 62; "
    "  end Main; "
    "end P;";

// (B) The EXPRESSION-bodied sibling of exactly (A). Agreement between the two
// body forms is the invariant; this side already worked (the #541 branch).
const char* kExprLongNullable =
    "module M; "
    "class P "
    "  public static method Main(): int "
    "    var f := lambda(): long? => 42; "
    "    var q := f(); "
    "    if q = null then return 9; end; "
    "    return 62; "
    "  end Main; "
    "end P;";

// (C) The two forms compared IN ONE PROGRAM, on a float widen (`int` -> f64,
// an SIToFP rather than an SExt). The test asserts the two agree rather than
// asserting a literal, so it cannot pass by both forms being wrong the same
// way — the block form is built by the fixed path and the expr form by the
// pre-existing #541 path.
const char* kBothFormsDoubleNullable =
    "module M; "
    "class P "
    "  public static method Main(): int "
    "    var f := lambda(): double? => do return 3; end; "
    "    var g := lambda(): double? => 3; "
    "    var q := f(); "
    "    var r := g(); "
    "    if q = null then return 9; end; "
    "    if r = null then return 8; end; "
    "    if q! = r! then return 63; end; "
    "    return 1; "
    "  end Main; "
    "end P;";

// (D) CONTROL — the exact-width shape the issue filed, which ALREADY worked via
// emitReturn's own fallback. It must keep working: this pins that the new
// coercion does not double-wrap an already-tagged struct.
const char* kBlockIntNullableControl =
    "module M; "
    "class P "
    "  public static method Main(): int "
    "    var f := lambda(): int? => do return 3; end; "
    "    var q := f(); "
    "    if q = null then return 9; end; "
    "    return q! + 30; "
    "  end Main; "
    "end P;";

// (E) OVER-CORRECTION GUARD — #2688's subject. The nullable fallback keys on
// the SAME field (`currentLambdaReturnSemaTy_`) the block-bodied `string` mint
// keys on, so this pins that the mint arm is unchanged: a block-bodied
// `string`-returning lambda still mints the +1 its caller releases. Getting it
// wrong in the other direction is a DOUBLE FREE, not a leak, which is why this
// guard runs on the ASan oracle. Strings are built at RUN TIME — `"ab" + "cd"`
// folds to an RC sentinel `bz_string_release` no-ops on, making a literal-built
// probe vacuous.
const char* kStringBlockLambdaStillMints =
    "module M; "
    "class P "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var s := $\"v-{i}\"; "
    "      var f := lambda(): string => do return s; end; "
    "      var t := f(); "
    "      total := total + t.Length; "
    "    end; "
    "    if total = 71 then return 64; end; "
    "    return 1; "
    "  end Main; "
    "end P;";

}  // namespace

// per §27.3, §7.1, §5.1 (issue #2716)
BEZEL_TEST(codegen_lambda_block_nullable_return_2716,
           block_bodied_long_nullable_lambda_coerces_spec_7_1) {
    JitResult r = runAot(kBlockLongNullable);
    BEZEL_EXPECT_TRUE(r.ran);   // pre-fix: "ret i32 42" vs `{ i8, i64 }`
    BEZEL_EXPECT_EQ(r.exitCode, 62);
}

// per §27.3, §7.1
BEZEL_TEST(codegen_lambda_block_nullable_return_2716,
           expression_bodied_long_nullable_lambda_control_spec_7_1) {
    JitResult r = runAot(kExprLongNullable);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 62);
}

// per §27.3, §5.1
BEZEL_TEST(codegen_lambda_block_nullable_return_2716,
           both_body_forms_agree_on_double_nullable_spec_27_3) {
    JitResult r = runAot(kBothFormsDoubleNullable);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 63);
}

// CONTROL — worked pre-fix, must not be double-wrapped. // per §7.1
BEZEL_TEST(codegen_lambda_block_nullable_return_2716,
           block_bodied_int_nullable_exact_width_control_spec_7_1) {
    JitResult r = runAot(kBlockIntNullableControl);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 33);
}

// OVER-CORRECTION GUARD — the #2688 mint must still fire. Passes before and
// after by design; its job is to go red if the mint arm is disturbed.
// per §27.3, §28.9 (#2688)
BEZEL_TEST(codegen_lambda_block_nullable_return_2716,
           string_block_lambda_still_mints_its_plus_one_2688) {
    JitResult r = runAotChecked(kStringBlockLambdaStillMints);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 64);
}

// The same guard on the PLAIN oracle (#2633/#2629) — a double free is often
// visible as a crash without a sanitizer, and `--full` has masked defects the
// plain gate catches. // per §27.3, §28.9
BEZEL_TEST(codegen_lambda_block_nullable_return_2716,
           string_block_lambda_still_mints_its_plus_one_plain_2688) {
    JitResult r = runAot(kStringBlockLambdaStillMints);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 64);
}
