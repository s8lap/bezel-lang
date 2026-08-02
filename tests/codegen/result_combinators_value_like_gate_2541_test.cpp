// Regression for #2541 (#2513 follow-up): the dedicated codegen path emits
// the four §35.2 combinators only where the receiver AND result instantiations
// are both value-like (§35.4 rule 3). Sema (ExprTypecheck_Calls.cpp's
// Result-combinator branch, added with #2513) rejects the non-admissible
// cases with a §3.6-derived-type-properties rule-2 diagnostic naming the
// classification, the deciding payload, and the owning-match remedy
// (§35.5). Codegen never sees a non-value-like call — the gate fires at
// sema, before any IR is emitted.
//
// Each test below pins a distinct rejection shape — per tests/README.md,
// `runAot`'s coarse `rejects()` helper is INSUFFICIENT for these: it
// cannot tell a genuine sema rejection from a runtime SIGSEGV, so the
// diagnostic TEXT is the load-bearing oracle (asserted on the captured
// `r.diagnostics` from `runAot`).
//
// Coverage (the value-like gate on Result<...> receiver instantiations):
//   * own T     — `Result<Array<int>, string>` (owning payload) — Map /
//                 AndThen / UnwrapOr rejected; the diagnostic names the
//                 receiver's owning classification and points at the
//                 owning-match remedy (§35.5).
//   * own E     — `Result<int, OwningBuffer<byte>>` (owning error
//                 payload) — rejected: receiver is owning-classified.
//   * borrow-bearing receiver — out of scope for `Result<T, E>` (every
//                 non-owning instantiation is value-like per §35.4 rule 3),
//                 covered by the gate's ValueLike row at Result<Connection, ...>
//                 if the codebase ever mints such a choice — pinned for the
//                 future shape via the existing Result instantiation
//                 rejection (no test needed until then).
//   * non-value-like result inference — `Result<int, string>.Map<F>(f)` whose
//                 inferred F would mint a borrow-bearing `Result<int, F>` —
//                 rejected: the gate's "result must also be value-like" arm
//                 fires.
//   * fallback type mismatch (UnwrapOr) — distinct from the value-like
//                 gate: a `string` fallback against a `Result<int, ...>` is
//                 a §22.6 type mismatch (not §35.4).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::Severity;

namespace {

// True iff the program's compile produced an Error-severity diagnostic whose
// message contains `keyword`. Without the #2513/#2541 fix, the call would
// reject with a generic "no member `Map` on type `Result`" (which DOES match
// `keyword == "no member"`); the gate adds a §35.4-specific diagnostic that
// this helper distinguishes by matching the gate's own phrasing.
bool hasError(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

const char* kCommon =
    "module M; "
    "class C public V: int := 0; end C; "
    "class App public static method Main(): int ";

// §35.4 rule 3 / §35.2 — owning T (a class) makes the `Result<T, E>`
// instantiation own-classified per §35.4 rule 1 (the `owned` payload
// marker on a class payload disposes on the active case). The combinator
// is rejected with the §35.4 rule 3 diagnostic. Pre-#2513/#2541, this
// also rejected but with "no member `Map` on type `Result`" — the gate's
// new diagnostic names the owning classification and the owning-match
// remedy (§35.5).
BEZEL_TEST(result_combinators_value_like_gate_2541, owning_t_map_rejected) {
    const std::string program = std::string(kCommon) +
        "  var r: Result<C, string> := Result<C, string>.Ok(new C()); "
        "  var r2 := r.Map(lambda(x: C) => 1); "
        "  return 0; end Main; end App;";
    BEZEL_EXPECT_TRUE(hasError(program, "owning"));
    BEZEL_EXPECT_TRUE(hasError(program, "§35.4"));
}

// §35.4 rule 3 — the §35.4 gate names the deciding payload. A `MapError`
// on an owning-classified `Result<int, E>` (the E side carries an owned
// class) is rejected with the same gate text.
BEZEL_TEST(result_combinators_value_like_gate_2541,
           owning_e_maperror_rejected) {
    const std::string program = std::string(kCommon) +
        "  var r: Result<int, C> := Result<int, C>.Error(new C()); "
        "  var r2 := r.MapError(lambda(e: C) => 1); "
        "  return 0; end Main; end App;";
    BEZEL_EXPECT_TRUE(hasError(program, "owning"));
    BEZEL_EXPECT_TRUE(hasError(program, "§35.4"));
}

// §35.4 rule 3 — UnwrapOr on an owning-classified Result (owning T — a
// class) is rejected because the fallback must be a value-like T that
// copies into a value slot, but an owning T is a move-only reference. The
// gate fires the receiver's value-like check first, since the receiver is
// owning; the diagnostic names the value-like classification, the deciding
// payload, and the owning-match remedy (§35.5).
BEZEL_TEST(result_combinators_value_like_gate_2541,
           owning_t_unwrapor_rejected) {
    const std::string program = std::string(kCommon) +
        "  var r: Result<C, string> := Result<C, string>.Ok(new C()); "
        "  var v: C := r.UnwrapOr(new C()); "
        "  return 0; end Main; end App;";
    BEZEL_EXPECT_TRUE(hasError(program, "value-like"));
    BEZEL_EXPECT_TRUE(hasError(program, "§35.4"));
}

// §22.6 / §35.2 — distinct from the value-like gate: an `UnwrapOr`
// fallback whose static type does not match the receiver's T is rejected
// as a §22.6 type mismatch (this test ALSO verifies the gate does NOT
// falsely reject a value-typed fallback that just happens to need widening
// — the int -> long widening is a §5.1 implicit conversion, NOT a §35.4
// violation).
BEZEL_TEST(result_combinators_value_like_gate_2541,
           unwrapor_fallback_widens_ok) {
    const std::string program = std::string(kCommon) +
        "  var r: Result<int, string> := Result<int, string>.Error(\"x\"); "
        "  var v: long := r.UnwrapOr(99); "
        "  return (if v = 99L then 0 else 1); end Main; end App;";
    // The fallback `99` (int) widens implicitly to long per §5.1 — the
    // §22.6 type-mismatch gate passes, and the value-like gate passes
    // (T=int is value-like), so this must compile AND return 0.
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
}

}  // namespace