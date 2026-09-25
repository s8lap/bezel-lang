// fix #4808 — sema compound `Object op= Object` is admitted; `o += o` produces
// garbage bytes (silent wrong code) and `o *= o` ICEs (IR verifier failure:
// Intrinsic has incorrect return type). Spec §25 / §8.1 / §6.6 (v0.307) say
// no arithmetic operator is defined on `Object`, so both must be compile errors
// like the binary `o + o`.
//
// THE BUG. The §6.6 compound-assignment desugar (ExprTypecheck.cpp, the
// "Arithmetic" family arm) computed R from primitive Widening alone, with no
// gate checking whether the target's primitive type actually SUPPORTS the
// operator. For a bool target, the Bool-exclusion gate caught it (§3.5).
// For Object (primitive, but with no arithmetic operators defined), the
// Widening path admitted it: `Object` widens nothing, so `rPrim = rTy->primitive`
// set R = Object, and since R == T the write-back was admitted and the
// malformed IR reached LLVM's verifier.
//
// THE FIX. Add a gate in the "Arithmetic" family arm (the `else` branch
// starting around ExprTypecheck.cpp line ~18850): if the target's static type
// is Object (P::Object), emit a §25 / §8.1 compile error — exactly the same
// diagnostic the binary `o + o` path emits, derived from the spec's "no
// arithmetic operator is defined on Object" rule.
//
// Sibling sanity: `o += 1` after narrowing `o` to `int` must still work.
// (Not tested here — narrowing is tested separately in the conformance suite.)

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {

std::string prog4808(std::string_view body) {
    return std::string(
               "module M; public class Program "
               "public static method Main(): int ") +
           std::string(body) + " end Main; end Program;";
}

}  // namespace

// §25/§8.1/§6.6 (fix #4808, v0.307): `o += o` on an un-narrowed Object target
// must be rejected — no arithmetic operator is defined on Object.
BEZEL_TEST(compound_object_arith_reject_4808, object_plus_equals_object_rejected) {
    auto r = runAot(prog4808(
        " var o: Object := 5; o += o; return 0; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `+` is not defined for type `Object`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7" "25"));
}

// §25/§8.1/§6.6 (fix #4808, v0.307): `o -= o` similarly must be rejected.
BEZEL_TEST(compound_object_arith_reject_4808, object_minus_equals_object_rejected) {
    auto r = runAot(prog4808(
        " var o: Object := 5; o -= o; return 0; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `-` is not defined for type `Object`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7" "25"));
}

// §25/§8.1/§6.6 (fix #4808, v0.307): `o *= o` — the ICE case, same fix needed.
BEZEL_TEST(compound_object_arith_reject_4808, object_times_equals_object_rejected) {
    auto r = runAot(prog4808(
        " var o: Object := 5; o *= o; return 0; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `*` is not defined for type `Object`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7" "25"));
}

// §25/§8.1/§6.6 (fix #4808, v0.307): `o /= o` also must be rejected.
BEZEL_TEST(compound_object_arith_reject_4808, object_div_equals_object_rejected) {
    auto r = runAot(prog4808(
        " var o: Object := 5; o /= o; return 0; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `/` is not defined for type `Object`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7" "25"));
}

// §25/§8.1/§6.6 (fix #4808, v0.307): bitwise compound ops on Object too.
BEZEL_TEST(compound_object_arith_reject_4808, object_and_equals_object_rejected) {
    auto r = runAot(prog4808(
        " var o: Object := 5; o &= o; return 0; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `&` is not defined for type `Object`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7" "25"));
}
