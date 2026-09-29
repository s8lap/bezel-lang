// Regression test for issue #4826 — codegen `buildRecordValue`
// (src/codegen/LlvmBackend_Exprs2.cpp) named-argument branch writes into a
// field's slot without checking whether a positional argument already
// bound that slot, so `R(1, A := 2)` (where `A` is the first positional
// field) would silently overwrite the positional value IF it ever reached
// codegen (§22.5 "Optional parameters and named arguments", v0.307).
//
// §22.5, v0.307, verbatim: "Each supplied argument binds exactly one
// parameter and each parameter is bound by at most one supplied argument:
// naming a parameter already bound positionally ..., or naming the same
// parameter twice, is a compile error at the call naming that parameter"
// — and the rule's reach is stated explicitly: "every construction site in
// the language reaches it through the same binding — a call, a `new`
// expression, and a record construction alike."
//
// THE HYPOTHESIS (issue #4826, relayed HYPOTHESIS not MEASURED by the
// non-author reviewer of #4776): the codegen hazard is latent because
// sema's §22.5 binder rejects the double-bind before codegen is reached.
//
// MEASURED here on origin/main (2026-09-29): `new Q(1, X := 2)` on a plain
// positional record `record Q(X: int, Y: int);` — the literal shape
// `buildRecordValue`'s primary-ctor fast path handles (LlvmBackend_Exprs2.cpp,
// the `posFields`/`fieldArg` mapping around line 11394) — IS rejected at
// compile time:
//
//   record_positional_named_double_bind_4826.bzl:5:14: `new Q`: parameter
//   `X` is bound more than once (§22.5)
//
// confirming the hazard is unreachable today: sema's §22.5 check runs
// before `buildRecordValue`'s named-argument branch can execute. This test
// pins that rejection for the specific "plain positional record, primary
// field bound positionally then again by name" shape, since no existing
// regression row covers it (the closest, `sema_named_22_5__duplicate_
// binding_positional_then_named_diagnosed`, uses an ordinary method call,
// not a record `new`; #4496/#4518 cover base-chain and inheritance-clause
// binding; #5121 covers combined-form classes with an explicit
// constructor). No src/ change: this closes the issue's second proposed
// action ("add a sema regression row pinning the double-bind rejection, if
// one does not already exist") for the plain-record-`new` case. The
// defence-in-depth codegen assertion the issue also proposes is not added:
// the hazard is provably unreachable, and adding dead defensive code to a
// hot lowering path with no test that can exercise it is not narrow.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// ── §22.5 REJECT: positional then named, same primary field ────────────────
//
// `new Q(1, X := 2)` — the positional arg 1 fills X (index 0); the named
// arg `X := 2` names the same field again. Per §22.5 this is a compile
// error at the call, not a silent overwrite.

BEZEL_TEST(record_positional_named_double_bind_4826,
           plain_positional_record_positional_then_named_same_field_rejected) {
    auto r = runAot(
        "module M; "
        "record Q(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var q := new Q(1, X := 2); "
        "  return q.X; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "parameter `X` is bound more than once (§22.5)"));
}

// ── CONTROL: the same field bound twice by name alone is also rejected ────
//
// `new Q(X := 1, X := 2)` — no positional argument involved. Confirms the
// diagnostic is the general §22.5 double-bind check, not something keyed
// on a positional/named mix specifically.

BEZEL_TEST(record_positional_named_double_bind_4826,
           plain_positional_record_named_twice_same_field_rejected) {
    auto r = runAot(
        "module M; "
        "record Q(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var q := new Q(X := 1, X := 2); "
        "  return q.X; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "parameter `X` is bound more than once (§22.5)"));
}

// ── CONTROL: the valid mixed-form sibling still compiles (no over-reject) ──
//
// `new Q(1, Y := 2)` binds each field exactly once (X positionally, Y by
// name) — legal per §22.5 and already pinned by #4776's
// `plain_positional_record_new_Q_10_Y_20_fields_not_zero`. Repeated here
// as a same-file control so a future change to this diagnostic's trigger
// condition can't over-reject the legal mixed form without a local red row.

BEZEL_TEST(record_positional_named_double_bind_4826,
           plain_positional_record_distinct_fields_mixed_form_still_compiles) {
    auto r = runAot(
        "module M; "
        "record Q(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var q := new Q(1, Y := 2); "
        "  if q.X <> 1 then return 1; end; "
        "  if q.Y <> 2 then return 2; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
}
