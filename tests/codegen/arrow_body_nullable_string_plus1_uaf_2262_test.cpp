// Codegen / UAF — fix #2262: the arrow-body (`=> expr`) half of the nullable
// `string?` return-mint was missed by #2196. After #2196 the caller-side
// `isUserMethodStringReturnCall` classifies a `string?` method-call as a +1
// producer and arms a scope-end release via collectStringReleaseEligible,
// but the callee-side arrow-body lowering still gated the +1 mint on
// `!declRet->nullable` — so the caller released a value the callee never
// retained. `_name`'s refcount drops one below its true count: premature
// free of the field's string, then UAF on the next read, or double-free at
// object disposal.
//
// THE FIX. Drop `!declRet->nullable` from the arrow-body string-return mint
// in LlvmBackend_Setup.cpp:9197, mirroring the ReturnStmt change that #2196
// already made (LlvmBackend_Stmts.cpp:~2460). The TRANSFER predicates
// (`isPlusOneStringProducer`, `returnedStringLocalOwnsPlusOne`) already
// short-circuit correctly for nullable returns, so the only behavior change
// is the RETAIN half firing on `=> fieldRef` / `=> param` / `=> null` /
// `=> "literal"` arrow bodies whose declared return type is `string?`.
// `bz_string_retain` is null-safe (no-op on a sentinel/null), so the
// extension is sound for null/literal returns.
//
// Spec §28.3 (Local variables, ownership, and borrows — producers) +
// §22.3 (Parameter modifiers), v0.282.
//
// OBSERVABILITY. runAotChecked links + runs under ASan/LSan. (A) and (B)
// are the FAIL-WITHOUT probes — an arrow body returning a FIELD or a PARAM
// of declared return type `string?` whose caller binds the result to a
// local in a loop. Each iteration drops the field/param's rc one below
// its true count; after N iterations the buffer is freed N times below its
// real owner count, then the NEXT iteration's access to `_name` /
// `param.Length` is a heap-use-after-free. (C) is the non-nullable
// sibling — pre-existing behavior (retained), a regression-guard. (D) is
// the block-body sibling — pre-existing behavior (already fixed by #2196),
// a regression-guard. (E) is the `=> null` arrow body — null-safe retain,
// the local is released with no effect.

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::checkedToolchainSkipped;
using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// True if `text` carries an ASan / LSan report headline — the FAIL-WITHOUT
// oracle. Mirrors tests/support/aot_support.hpp:hasSanitizerReport but
// inlined so the test is self-contained (the harness's helper is internal).
bool hasSanitizerReport(const std::string& text) {
    static const char* markers[] = {
        "ERROR: AddressSanitizer", "ERROR: LeakSanitizer",
        "SUMMARY: AddressSanitizer", "detected memory leaks",
        "heap-use-after-free", "double-free", "use-after-free",
    };
    for (const char* m : markers)
        if (text.find(m) != std::string::npos) return true;
    return false;
}

// (A) FAIL-WITHOUT probe — the issue's own repro. A `class C` with
// `public method Pick(): string? => _name;` (arrow body returning a field
// borrow, declared return type `string?`). The caller binds the result to
// a `string?` local inside a 5-iteration loop and reads `.Length`. Pre-fix
// the arrow body never retained `_name`, but the caller classified `Pick()`
// as a +1 producer (#2196) and released the local — so each iteration
// decremented `_name`'s rc one below its true count. After the field's
// buffer was freed, the next iteration's `_name.Length` read is a
// heap-use-after-free. PASS-WITH: total = 5 * "ab".Length (2) = 10;
// ASan-clean.
const char* kArrowBodyFieldNullableString =
    "module M; class C "
    "  private _name: string := \"a\" + \"b\"; "
    "  public method Pick(): string? => _name; "
    "  public static method Main(): int "
    "    var c := new C(); "
    "    var total := 0; "
    "    var i := 0; "
    "    while i < 5 do "
    "      var s := c.Pick(); "
    "      total := total + s!.Length; "
    "      i := i + 1; "
    "    end; "
    "    return total; "
    "  end Main; "
    "end C;";

// (B) FAIL-WITHOUT probe — the `=> param` sibling. A method `Echo(p:
// string?): string? => p` returns the parameter as-is. To exercise the
// real +1 path the caller passes a fresh-owned string each iteration (a
// `+` concat mints +1 — not a sentinel). Pre-fix the callee did not
// mint +1 for the nullable return, so the caller received the same
// pointer it passed in (with rc=1 already, since the caller's local
// was just created by the concat) — the caller's scope-end release
// then dropped rc from 1 to 0, freeing the buffer. The next iteration
// creates a fresh string (different pointer), so this is NOT a
// cross-iteration UAF — it's a same-iteration refcount imbalance that
// surfaces only under ASan: each iteration's release is unbalanced
// (no matching retain from the callee). PASS-WITH: total = 5 * 2 = 10.
const char* kArrowBodyParamNullableString =
    "module M; class C "
    "  public method Echo(p: string?): string? => p; "
    "  public static method Main(): int "
    "    var c := new C(); "
    "    var total := 0; "
    "    var i := 0; "
    "    while i < 5 do "
    "      var s := c.Echo(\"a\" + \"b\"); "
    "      total := total + s!.Length; "
    "      i := i + 1; "
    "    end; "
    "    return total; "
    "  end Main; "
    "end C;";

// (C) REGRESSION GUARD — non-nullable sibling of (A). `=> _name` for a
// `string` (NOT `string?`) declared return type. Pre-#2262 AND post-#2262
// this RETAINs (the `!declRet->nullable` gate DID NOT block non-nullable
// returns; this is the well-behaved path). After #2262 the predicate
// drop is the only change; this cell must stay green and ASan-clean.
// total = 5 * "ab".Length (2) = 10.
const char* kArrowBodyFieldNonNullableString =
    "module M; class C "
    "  private _name: string := \"a\" + \"b\"; "
    "  public method Pick(): string => _name; "
    "  public static method Main(): int "
    "    var c := new C(); "
    "    var total := 0; "
    "    var i := 0; "
    "    while i < 5 do "
    "      var s := c.Pick(); "
    "      total := total + s.Length; "
    "      i := i + 1; "
    "    end; "
    "    return total; "
    "  end Main; "
    "end C;";

// (D) REGRESSION GUARD — block-body (`begin … return _name; end`)
// sibling of (A). The ReturnStmt path was already fixed by #2196 — the
// +1 mint fires for a nullable `string?` return. After #2262 this stays
// green and ASan-clean (the arrow-body fix is narrow — does NOT alter
// the ReturnStmt path). total = 5 * "ab".Length (2) = 10.
const char* kBlockBodyFieldNullableString =
    "module M; class C "
    "  private _name: string := \"a\" + \"b\"; "
    "  public method Pick(): string? "
    "    return _name; "
    "  end Pick; "
    "  public static method Main(): int "
    "    var c := new C(); "
    "    var total := 0; "
    "    var i := 0; "
    "    while i < 5 do "
    "      var s := c.Pick(); "
    "      total := total + s!.Length; "
    "      i := i + 1; "
    "    end; "
    "    return total; "
    "  end Main; "
    "end C;";

// (E) `=> null` arrow body — null-safe retain. The retain on a null value
// is a no-op (bz_string_retain is null-safe), and the caller's scope-end
// release is also a no-op. Pre-fix this also did the right thing (the
// callee didn't retain, but the caller didn't release because the
// local is bound to null — collectStringReleaseEligible's BORROW branch
// rejects `string?` and the PRODUCER branch's `isUserMethodStringReturnCall`
// returned true at the call site but the callee had not minted either,
// so the local was tracked for neither). Post-fix: the callee mints (a
// no-op on null) and the caller releases (a no-op on null). Net effect:
// same as pre-fix (the null sentinel is never refcounted), so this cell
// is a regression-guard against any accidental double-null-refcounting
// crash. count = 5 (every iteration null).
const char* kArrowBodyNullNullableString =
    "module M; class C "
    "  public method Pick(): string? => null; "
    "  public static method Main(): int "
    "    var c := new C(); "
    "    var count := 0; "
    "    var i := 0; "
    "    while i < 5 do "
    "      var s := c.Pick(); "
    "      if s = null then count := count + 1; end; "
    "      i := i + 1; "
    "    end; "
    "    return count; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// (A) FAIL-WITHOUT probe / fix #2262 — the issue's own repro. A `=> field`
// arrow body whose declared return type is `string?` returns a raw
// borrow; the caller classifies it as +1 (#2196) and releases at scope
// end — but the callee never retained, so the field's refcount drops one
// below its true count: UAF on the next field read. ASan-clean post-fix.
// §28.3 / §22.3, v0.282.
// =====================================================================
BEZEL_TEST(arrow_body_nullable_string_plus1_uaf_2262,
           arrow_body_field_nullable_string_loop_lsan_clean_spec_28_3) {
    JitResult r = runAotChecked(kArrowBodyFieldNullableString);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);          // 5 iterations, each release balanced
    BEZEL_EXPECT_FALSE(hasSanitizerReport(r.stdoutText));  // ASan/LSan-clean
    // ASan errors print to stderr; runAotChecked surfaces them via stdoutText
    // when the program terminates abnormally — keep both guards.
    for (const auto& d : r.diagnostics.diagnostics())
        BEZEL_EXPECT_EQ(d.message.find("AddressSanitizer"),
                        std::string::npos);
    BEZEL_EXPECT_EQ(r.exitCode, 10);   // 5 * "ab".Length
}

// =====================================================================
// (B) FAIL-WITHOUT probe / fix #2262 — `=> param` arrow body for a
// `string?` method. Same UAF shape as (A) but via the parameter borrow.
// §28.3 / §22.3, v0.282.
// =====================================================================
BEZEL_TEST(arrow_body_nullable_string_plus1_uaf_2262,
           arrow_body_param_nullable_string_loop_lsan_clean_spec_28_3) {
    JitResult r = runAotChecked(kArrowBodyParamNullableString);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasSanitizerReport(r.stdoutText));
    for (const auto& d : r.diagnostics.diagnostics())
        BEZEL_EXPECT_EQ(d.message.find("AddressSanitizer"),
                        std::string::npos);
    BEZEL_EXPECT_EQ(r.exitCode, 10);   // 5 * "ab".Length
}

// =====================================================================
// (C) REGRESSION GUARD — non-nullable `string` arrow body (`=> _name`
// returning `string`). Was already RETAINed pre-#2262; must stay
// ASan-clean post-#2262 (the fix is narrow — only relaxes the gate,
// does not break the existing path). §28.3 / §22.3, v0.282.
// =====================================================================
BEZEL_TEST(arrow_body_nullable_string_plus1_uaf_2262,
           arrow_body_field_non_nullable_string_loop_lsan_clean_spec_28_3) {
    JitResult r = runAotChecked(kArrowBodyFieldNonNullableString);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasSanitizerReport(r.stdoutText));
    for (const auto& d : r.diagnostics.diagnostics())
        BEZEL_EXPECT_EQ(d.message.find("AddressSanitizer"),
                        std::string::npos);
    BEZEL_EXPECT_EQ(r.exitCode, 10);   // 5 * "ab".Length
}

// =====================================================================
// (D) REGRESSION GUARD — block-body nullable `string?` return (the #2196
// fix's ReturnStmt path). Was already RETAINed pre-#2262; must stay
// ASan-clean post-#2262 (the fix targets the arrow-body path only).
// §28.3 / §22.3, v0.282.
// =====================================================================
BEZEL_TEST(arrow_body_nullable_string_plus1_uaf_2262,
           block_body_field_nullable_string_loop_lsan_clean_spec_28_3) {
    JitResult r = runAotChecked(kBlockBodyFieldNullableString);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasSanitizerReport(r.stdoutText));
    for (const auto& d : r.diagnostics.diagnostics())
        BEZEL_EXPECT_EQ(d.message.find("AddressSanitizer"),
                        std::string::npos);
    BEZEL_EXPECT_EQ(r.exitCode, 10);   // 5 * "ab".Length
}

// =====================================================================
// (E) `=> null` arrow body — null-safe retain. Both the callee's retain
// and the caller's release are no-ops on a null/sentinel value, so the
// loop just counts nulls. Regression-guard against any accidental
// null-refcounting crash introduced by the predicate relaxation.
// §28.3 / §22.3, v0.282.
// =====================================================================
BEZEL_TEST(arrow_body_nullable_string_plus1_uaf_2262,
           arrow_body_null_nullable_string_loop_lsan_clean_spec_28_3) {
    JitResult r = runAotChecked(kArrowBodyNullNullableString);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasSanitizerReport(r.stdoutText));
    for (const auto& d : r.diagnostics.diagnostics())
        BEZEL_EXPECT_EQ(d.message.find("AddressSanitizer"),
                        std::string::npos);
    BEZEL_EXPECT_EQ(r.exitCode, 5);    // 5 null iterations
}
