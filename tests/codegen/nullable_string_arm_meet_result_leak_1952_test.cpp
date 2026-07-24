// Codegen / leak — the NULLABLE sibling of #1934/#1948: an expression-position
// `case`/`when`, an if-expression, or a `??` whose result type is `string?` and
// whose selected arm MINTS a fresh owned `string?` (`1 => "a"+"b"`, `_ => null`)
// leaked the minted string when the result was bound to a local. Issue #1952.
//
// Spec: §19.2 (case/when arm-meet), §20 (if-expression), §7.7 (`??`), §28.3
// Producers (all three forms are ONE arm-meet rule; the `null`-literal arm
// "absorbs to the other arm's state (K7.5)"; a `(if c then s else null)` "has
// type string?; its ownership is §28.3's null-arm absorption"), §28.9
// (runtime-managed `string` refcount; retain/release are null/sentinel no-ops).
// Spec revision v0.220.
//
// THE BUG. #1934 (and its #1948 siblings) deliberately gated BOTH halves — the
// per-arm +1 normalization in lowerCaseExpr/lowerConditional/lowerCoalesce AND
// the `isPlusOneStringProducer` recognition — on `!nullable`, leaving `string?`
// on the old path. A nullable-`string?` arm-meet result whose producer arm mints
// `"a"+"b"` was therefore neither classified a PRODUCER (the `!nullable` gate)
// nor a BORROW (collectStringReleaseEligible's borrow branch rejects `string?`),
// so a `var s: string? := (case … => "a"+"b", …)` never released the minted +1 —
// it leaked once per binding (LSan: 32-byte header + 3-byte buffer). A nullable
// `var s: string? := "a"+"b"` (a plain concat) was ALREADY clean, because the
// `+` producer row never gated on nullable — proving the gap is specific to the
// gated arm-meet forms, and the fix is exactly to un-gate them.
//
// THE FIX (extends #1934's two coordinated halves to `string?`): the arm
// normalization mints +1 on a producer arm and leaves a `null` arm null
// (bz_string_retain is a null no-op), so the merged result is +1-or-null;
// `isPlusOneStringProducer` recognizes the nullable case/if/`??` so the bind
// classifies it a producer local and releases the +1 (null-safely) at scope end.
// Sound because the value shape is identical to the already-working nullable
// `+`/interpolation producer.
//
// SCOPE NOTE (recorded). This closes the DIRECT-binding form (the arm-meet
// expression bound to a local in the same body). The ISSUE's own probe routed
// the arm-meet through a `string?` METHOD RETURN (`Pick(): string?` bound in the
// caller); that leak is a DIFFERENT, broader gap — nullable-`string` method
// returns are NOT +1-normalized (the ReturnStmt mint and isUserMethodString
// ReturnCall are both `!nullable`-gated), so `var s := Pick()` leaks even for a
// plain `return "a"+"b"` with no arm-meet at all. That is filed separately; this
// test pins the arm-meet-specific nullable leak #1952 is titled for.
//
// OBSERVABILITY. runAotChecked links + runs under ASan/LSan. Each case binds the
// arm-meet result INSIDE A LOOP so N iterations leak N strings (unmistakable —
// not a single reachability false-negative). The exit code pins the value.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// (A) #1952 repro — a NULLABLE `case` expression whose producer arm mints
// `"a"+"b"`, bound to a `string?` local inside a loop. Each of the 5 iterations
// minted "ab" (the scrutinee C.Sel(1) selects arm 1); pre-fix all 5 leaked.
// total = 5 * "ab".Length = 10.
const char* kNullableCaseProducerArmLoop =
    "module M; class C "
    "  public static method Sel(n: int): int "
    "    return n; "
    "  end Sel; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    var i := 0; "
    "    while i < 5 do "
    "      var s: string? := (case C.Sel(1) of 1 => \"a\" + \"b\", _ => null); "
    "      total := total + s!.Length; "   // 2 each — value pin + borrow read
    "      i := i + 1; "
    "    end; "
    "    return total; "                    // 10
    "  end Main; "
    "end C;";

// (B) the NULLABLE if-expression sibling (§20), same loop shape.
const char* kNullableIfExprProducerBranchLoop =
    "module M; class C "
    "  public static method Sel(n: int): int "
    "    return n; "
    "  end Sel; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    var i := 0; "
    "    while i < 5 do "
    "      var s: string? := (if C.Sel(1) = 1 then \"a\" + \"b\" else null); "
    "      total := total + s!.Length; "
    "      i := i + 1; "
    "    end; "
    "    return total; "                    // 10
    "  end Main; "
    "end C;";

// (C) the NULL arm actually taken — the arm-meet result is null. Proves the
// null-arm path is sound (no retain/release of a non-null value, no crash): the
// null branch contributes null, and the scope-end release is a null no-op. s is
// null, so we count nothing; loop returns 0.
const char* kNullableCaseNullArmLoop =
    "module M; class C "
    "  public static method Sel(n: int): int "
    "    return n; "
    "  end Sel; "
    "  public static method Main(): int "
    "    var count := 0; "
    "    var i := 0; "
    "    while i < 5 do "
    "      var s: string? := (case C.Sel(2) of 1 => \"a\" + \"b\", _ => null); "
    "      if s = null then count := count + 1; end; "
    "      i := i + 1; "
    "    end; "
    "    return count; "                    // 5 (every iteration null)
    "  end Main; "
    "end C;";

// (E) the INFERRED-TYPE form (no `: string?` annotation): `var s := (case …
// _ => null)`. §19.2's null-arm rule types the case `string?`; lowerCaseExpr's
// Path B previously wrapped the `string?` result (a bare `ptr`) into a `{i8,
// ptr}` tagged struct because the non-null `"a"+"b"` arm was misclassified as a
// "non-ptr value" needing reconciliation. The declared-type local path unwrapped
// that struct, but the inferred-type (plain `var`) path did not, leaving the
// minted string un-refcounted → leak. Fixed by only reconciling a GENUINE
// non-pointer arm (matching lowerConditional). total = 5 * "ab".Length = 10.
const char* kInferredNullableCaseProducerLoop =
    "module M; class C "
    "  public static method Sel(n: int): int "
    "    return n; "
    "  end Sel; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    var i := 0; "
    "    while i < 5 do "
    "      var s := (case C.Sel(1) of 1 => \"a\" + \"b\", _ => null); "
    "      total := total + s!.Length; "
    "      i := i + 1; "
    "    end; "
    "    return total; "                    // 10
    "  end Main; "
    "end C;";

// (F) the NULLABLE `??` sibling (§7.7): a producer LHS if-expr coalesced with a
// null-tolerant fallback, result `string?`. Exercises the lowerCoalesce nullable
// arm normalization. LHS is non-null ("ab"), so the fallback is not evaluated.
// total = 5 * 2 = 10.
const char* kNullableCoalesceProducerLoop =
    "module M; class C "
    "  public static method Sel(n: int): int "
    "    return n; "
    "  end Sel; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    var i := 0; "
    "    while i < 5 do "
    "      var f: string? := null; "
    "      var s: string? := (if C.Sel(1) = 1 then \"a\" + \"b\" else null) ?? f; "
    "      total := total + s!.Length; "
    "      i := i + 1; "
    "    end; "
    "    return total; "                    // 10
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// (A) #1952 repro — a nullable `case`-expression producer arm bound in a loop
// is LSan-clean. §19.2 / §28.3 (arm-meet, null-arm absorption) / §28.9.
// =====================================================================
BEZEL_TEST(nullable_string_arm_meet_result_leak_1952,
           nullable_case_producer_arm_loop_lsan_clean_spec_19_2) {
    JitResult r = runAotChecked(kNullableCaseProducerArmLoop);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);          // 5 minted strings each freed once
    BEZEL_EXPECT_EQ(r.exitCode, 10);   // 5 * "ab".Length
}

// =====================================================================
// (B) The nullable if-expression sibling — LSan-clean. §20 / §28.3 / §28.9.
// =====================================================================
BEZEL_TEST(nullable_string_arm_meet_result_leak_1952,
           nullable_if_expr_producer_branch_loop_lsan_clean_spec_20) {
    JitResult r = runAotChecked(kNullableIfExprProducerBranchLoop);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// =====================================================================
// (C) The null arm taken — sound (no crash, no spurious retain/release).
// §28.3 null-arm absorption (K7.5) / §28.9.
// =====================================================================
BEZEL_TEST(nullable_string_arm_meet_result_leak_1952,
           nullable_case_null_arm_loop_sound_spec_28_3) {
    JitResult r = runAotChecked(kNullableCaseNullArmLoop);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// =====================================================================
// (E) The INFERRED-type form — no `: string?` annotation. Exercises the
// lowerCaseExpr Path B reference-vs-value reconciliation fix. §19.2 / §7.1.
// =====================================================================
BEZEL_TEST(nullable_string_arm_meet_result_leak_1952,
           inferred_nullable_case_producer_loop_lsan_clean_spec_19_2) {
    JitResult r = runAotChecked(kInferredNullableCaseProducerLoop);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// =====================================================================
// (F) The nullable `??` sibling — LSan-clean. §7.7 / §28.3 / §28.9.
// =====================================================================
BEZEL_TEST(nullable_string_arm_meet_result_leak_1952,
           nullable_coalesce_producer_loop_lsan_clean_spec_7_7) {
    JitResult r = runAotChecked(kNullableCoalesceProducerLoop);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}
