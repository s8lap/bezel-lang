// Fix #1697 — a lambda literal's return-value conversion at a "closed slot"
// (a fully-determined function-typed parameter, §22.6 "Lambda-literal
// arguments", v0.184) is miscompiled when there is a SINGLE applicable
// candidate — no overload resolution to trigger it.
//
// Repro (from the issue, types-probes/b2_single_convert.bz):
//   M(f: (int) -> long) called as M((x) => x * 2)
// The lambda's inferred return type is `int` (from `x * 2`, x: int); §22.6
// admits the call because `int` implicitly converts to the target return
// slot `long` (§5.1). Before the fix, LLVM IR verification failed:
//   Function return type does not match operand type of return inst!
//     ret i32 %5
//    i64
//
// Root cause: lowerLambda (LlvmBackend_Exprs2.cpp) derives the closure's
// LLVM return type (retTy) from the lambda's own inferred return TypeRef,
// but falls back to expectedFunctionType_->funcReturn (the closed slot's
// declared return type) whenever `retTy == i32()` — a sentinel originally
// meant for "unresolved". A body genuinely typed `int` also lowers to i32(),
// so the fallback fires and overrides retTy to the target's LLVM type
// (i64 for `long`) — correctly declaring the closure function's signature —
// but the expression-body emit path never converted the *value* `v` to
// match: it called `builder_.CreateRet(v)` directly with the un-widened i32,
// producing the ret-type mismatch above. The nullable-value-type branch
// (fix #541) already coerces in the analogous T? case; this lane adds the
// same coercion for a non-nullable numeric target, mirroring the block-body
// ReturnStmt fix (§5.1, #746 R41-H, LlvmBackend_Stmts.cpp) that already
// widens a named method's `return` value against `currentFn_->getReturnType()`.
//
// Spec derivation: §22.6 "Lambda-literal arguments" (v0.184) — "The lambda's
// parameters take the candidate's (substituted) parameter types and the body
// is typed; the candidate is applicable at the position when the lambda's
// inferred return type equals the candidate's return slot, implicitly
// converts to it ([§5.1])..." — and §5.1 (numeric widening conversions).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ---------------------------------------------------------------------------
// §22.6 / §5.1: the issue's exact repro shape — single-candidate closed slot,
// expression-bodied lambda, int→long widening on return.
// ---------------------------------------------------------------------------

// M(f: (int) -> long) called M((x) => x * 2); f(5) => 10, widened to long.
// Before fix: IR verification failure (ran=false) -> sentinel -999.
// After fix: 10.
BEZEL_TEST(lambda_return_conversion_closed_slot_1697,
           single_candidate_int_to_long_spec_22_6_5_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public static class Program "
        "  public static method M(f: (int) -> long): long => f(5); "
        "  public static method Main(): int "
        "    var r := M((x) => x * 2); "
        "    return int(r); "
        "  end Main; "
        "end Program;"),
        10);
}

// ---------------------------------------------------------------------------
// §22.6 / §5.1: the widening must be a genuine SIGN-EXTENSION, not merely
// "compiles" — a value round-tripped through `int(r)` cannot distinguish a
// correct sign-extend from an incorrect zero-extend (both share the low 32
// bits), so this test observes the high bits directly via a signed
// comparison against a `long` zero.
// ---------------------------------------------------------------------------

// f(3) = 3 - 10 = -7 (int), widened to long. A negative `int` source that is
// incorrectly zero-extended (instead of sign-extended) becomes a large
// POSITIVE long (0x00000000FFFFFFF9), so `r < 0L` would wrongly read false.
// Correct sign-extension keeps r == -7L, so `r < 0L` is true.
BEZEL_TEST(lambda_return_conversion_closed_slot_1697,
           single_candidate_negative_value_sign_extends_spec_5_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public static class Program "
        "  public static method M(f: (int) -> long): long => f(3); "
        "  public static method Main(): int "
        "    var r := M((x) => x - 10); "
        "    if r < 0L then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end Program;"),
        1);
}

// ---------------------------------------------------------------------------
// CONTROLS
// ---------------------------------------------------------------------------

// §22.6 CONTROL: a single-candidate closed slot whose lambda return type is
// EXACT (no conversion, int -> int) must still work — the fix must not
// disturb the no-conversion path (retTy == i32() legitimately from an
// int-returning slot too, but v already matches).
BEZEL_TEST(lambda_return_conversion_closed_slot_1697,
           control_exact_match_no_conversion_spec_22_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public static class Program "
        "  public static method M(f: (int) -> int): int => f(5); "
        "  public static method Main(): int "
        "    return M((x) => x * 2); "
        "  end Main; "
        "end Program;"),
        10);
}

// §22.6 CONTROL: a BLOCK-BODIED lambda at the same closed slot (int -> long)
// already routes its `return` through the shared ReturnStmt/emitReturn path
// (LlvmBackend_Stmts.cpp), which already applies coerceNumericToType against
// `currentFn_->getReturnType()` — this must keep working unchanged. This
// isolates the bug to the EXPRESSION-body emit path specifically.
BEZEL_TEST(lambda_return_conversion_closed_slot_1697,
           control_block_body_already_converts_spec_22_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public static class Program "
        "  public static method M(f: (int) -> long): long "
        "    var r := f(3); "
        "    return r; "
        "  end M; "
        "  public static method Main(): int "
        "    var r := M((x) => do return x - 10; end); "
        "    if r < 0L then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end Program;"),
        1);
}

// §7.1 / §541 CONTROL: the pre-existing NULLABLE-value-type conversion path
// (int literal in a `(K) -> int?` closed slot) must be unaffected by adding
// the non-nullable numeric coercion alongside it.
BEZEL_TEST(lambda_return_conversion_closed_slot_1697,
           control_nullable_value_conversion_unaffected_spec_7_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public static class Program "
        "  public static method Apply(k: string, f: (string) -> int?): int? "
        "    return f(k); "
        "  end Apply; "
        "  public static method Main(): int "
        "    var r: int? := Apply(\"k\", (k: string) => 7); "
        "    if r = null then return -1; end; "
        "    return r!; "
        "  end Main; "
        "end Program;"),
        7);
}
