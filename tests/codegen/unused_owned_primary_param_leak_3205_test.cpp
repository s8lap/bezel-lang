// fix #3205 (§22.10 v0.254 — leak): an `owned` primary-constructor parameter
// that is NEVER referenced (not in a field initializer, not in the base call,
// not in any body position — the case §22.10's enumeration does not name) is
// silently dropped by the implementation: the `+1` value flows through the
// parameter slot, no field captures it, and the generated `Dispose()` walks
// no slot for it. The value leaks with zero diagnostics.
//
// MEASURED on `8b7e458`:
//   public class UnusedL(owned r: ResL, tag: int) end UnusedL;
//   using var u := new UnusedL(new ResL(5), 200);
// LSan: 16 byte(s) leaked in 1 allocation (the ResL block).
//
// ROOT CAUSE: `isCapturedOwnedPrimaryParam` (codegen, LlvmBackend_impl.hpp ~8084)
// gated capture on `posParamBodyRef && !posParamInitRef` — a positive-list shape
// drawn from §22.10's "Referenced in a body position" / "Referenced only in
// field initializers" cases. The unreferenced case fell through the negative
// list and `isOwnedReferenceField` then rejected the slot too, so
// `emitOwnedFieldDisposals` never walked the slot and the value leaked.
//
// The same predicate mirrors in sema (ExprTypecheck.cpp:11292 trivial-dispose
// classifier, PureChecker.cpp:183, OwnershipFacts.cpp:85) had the same shape
// and the same blind spot.
//
// FIX (§22.10 reading the enumeration in light of its parenthetical "Silently
// dropping the parameter would be a leak under §11.1's ownership-transfer
// clause and Appendix C.3's disposed-exactly-once invariant"): an `owned`
// primary-ctor parameter is captured whenever its value would NOT be consumed
// by another mechanism — i.e. whenever `!posParamInitRef` AND either
// `posParamBodyRef` OR ZERO references at all. This widens the existing
// "consumed by an owned-field initializer ⇒ skip capture" gate without
// regressing it: an `owned` parameter consumed by a field initializer is
// still NOT captured (the explicit field owns the obligation), and an `owned`
// parameter consumed by both an initializer AND a body is still the §22.10
// shape the predicate already excluded.
//
// SPEC QUESTION: this case is one §22.10's enumeration does not cover. The
// obvious interpretation — capture anyway to match the parenthetical's
// "no silent drops" reading — is implementable cleanly (#1 in the issue); a
// loud diagnostic rejecting the program as a safe fallback is the alternative
// (#2). The lane took interpretation (1), since it keeps the existing happy
// paths untouched and the disposal exactly once. The enumeration gap is a
// SPEC fix to file separately — see the gh issue citing #3205 as parent.
//
// OBSERVABILITY: the leak is a DIRECT leak of the owned ResL heap block
// (1 × bz_box_t* / bz_calloc_alloc = 16 byte(s) on this build), so
// `runAotChecked`'s LeakSanitizer flag it. Pre-fix `r.ran` flips to false;
// post-fix the run is clean and the counter `ResL._n` independently confirms
// disposal ran (one `r` constructed → one destruction ⇒ counter is `1`).
//
// REGRESSION COVERAGE:
//   * (A) CARDINAL — owned param referenced NOWHERE ⇒ captured as a
//     compiler-generated `owned` private field, disposed by generated Dispose.
//     Pre-fix LSan: 16 byte(s) leaked; post-fix LSan-clean + `ResL._n == 1`.
//   * (B) CONTROL — owned param referenced in BODY (the #1543 happy path).
//     Must stay LSan-clean + `ResL._n == 1`. Regression guard.
//   * (C) CONTROL — owned param consumed by a FIELD INITIALIZER (the
//     `ResourceHolder` pattern, §11.1): the value flows into the explicit
//     field's initializer, the parameter itself is NOT captured. Must stay
//     LSan-clean + `ResL._n == 1`. Regression guard; the `!posParamInitRef`
//     gate must continue to admit this case as "consumed elsewhere".
//   * (D) §21.9 / §3.6 / #1594: a bare-statement discard of a class whose
//     only `owned` member is the unreferenced capture is correctly rejected
//     as a compile error (it is now non-trivial-dispose, and bare-disposal
//     does not run a `Dispose()`).
//
// SPEC CITATION: §22.10 (Primary constructors — "`owned` parameters and
// capture" paragraph, the parenthesised "Silently dropping the parameter
// would be a leak under §11.1's ownership-transfer clause and Appendix C.3's
// disposed-exactly-once invariant" reading), v0.254; §11.1 (ownership-transfer
// clause), Appendix C.3 (disposed-exactly-once invariant).

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAot;

namespace {

// ASan toolchain skip-guard (matches #2603 / #2598 / #2587 test pattern).
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// Disposable class whose destructor bumps a static counter, identical to
// #1543 / #1594 / #2603's test scaffolding. Allows verification of disposal
// count independently of LSan.
const char* kRes =
    "class ResL "
    "  public static _n: int := 0; "
    "  private _id: int; "
    "  public constructor(id: int) _id := id; end constructor; "
    "  public method Id(): int => _id; "
    "  destructor _n := _n + 1; end destructor; "
    "end ResL; ";

// ----------------------------------------------------------------------
// Probe A — CARDINAL: `owned` primary-ctor parameter NEVER referenced.
// `r` is `owned` and unused — not in any field initializer, not in the base
// call, not in any body position. §22.10's enumeration does not name this
// case; pre-fix the parameter is silently dropped (LSan: 16 byte(s) leaked).
// Post-fix the implementation treats the zero-references case as a body-
// position equivalent — capture as compiler-generated `owned` private field,
// disposed by the generated `Dispose()` (one destruction per `r` constructed
// ⇒ counter is `1`).
// ----------------------------------------------------------------------
const std::string kUnused =
    "module M; " + std::string(kRes) +
    "public class UnusedL(owned r: ResL, tag: int) "
    "end UnusedL; "
    "class C "
    "  public static method Main(): int "
    "  do using var u := new UnusedL(new ResL(5), 200); end; "
    "  return ResL._n; "
    "end Main; end C;";

// ----------------------------------------------------------------------
// Probe B — CONTROL: same class shape, but `r` IS referenced in a body
// position (the #1543 happy path). Already worked; this is the regression
// guard so the predicate widening does not regress the body-reference case.
// ----------------------------------------------------------------------
const std::string kUsed =
    "module M; " + std::string(kRes) +
    "public class UsedL(owned r: ResL, tag: int) "
    "  public method Tag(): int => tag + r.Id(); "
    "end UsedL; "
    "class C "
    "  public static method Main(): int "
    "  do using var u := new UsedL(new ResL(5), 200); end; "
    "  return ResL._n; "
    "end Main; end C;";

// ----------------------------------------------------------------------
// Probe C — CONTROL: `owned` param consumed by a FIELD INITIALIZER (the
// §11.1 `ResourceHolder` pattern): the value flows directly into an explicit
// `owned` field's initializer, the parameter itself is NOT captured. The
// widening must continue to admit this case — `!posParamInitRef` is the
// gate's whole guard, so the body's "no init reference" reading is intact.
// ----------------------------------------------------------------------
const std::string kInitConsumed =
    "module M; " + std::string(kRes) +
    "public class InitConsumedL(owned r: ResL, tag: int) "
    "  private owned _s: ResL := r; "
    "  public method Tag(): int => tag + _s.Id(); "
    "end InitConsumedL; "
    "class C "
    "  public static method Main(): int "
    "  do using var u := new InitConsumedL(new ResL(5), 200); end; "
    "  return ResL._n; "
    "end Main; end C;";

}  // namespace

// =====================================================================
// #3205 — CARDINAL: `owned` primary-ctor parameter never referenced is
// captured and disposed (LSan-clean + counter `ResL._n == 1`).
// Pre-fix LSan: 16 byte(s) leaked in 1 allocation (the ResL block).
// =====================================================================
BEZEL_TEST(codegen_unused_owned_primary_param_leak_3205,
           unused_owned_primary_param_captured_and_disposed_spec_22_10) {
    // per §22.10 (v0.254) — "Silently dropping the parameter would be a leak"
    // reading; see file header.
    JitResult r = runAotChecked(kUnused);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);            // LSan-clean (the #3205 leak flips this)
    BEZEL_EXPECT_EQ(r.exitCode, 1);      // ResL._n == 1 (one r constructed + disposed)
}

// =====================================================================
// #3205 — CONTROL: owned param in body (the #1543 happy path). Must stay
// LSan-clean + counter `1`.
// =====================================================================
BEZEL_TEST(codegen_unused_owned_primary_param_leak_3205,
           body_referenced_owned_primary_param_unchanged_spec_22_10) {
    JitResult r = runAotChecked(kUsed);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);      // counter unchanged from probe A's value
}

// =====================================================================
// #3205 — CONTROL: owned param consumed by a field initializer (the §11.1
// `ResourceHolder` pattern). The widening must continue to admit this case
// as "consumed by another owned-storage member" — the explicit field takes
// the obligation, NOT a capture field.
// =====================================================================
BEZEL_TEST(codegen_unused_owned_primary_param_leak_3205,
           init_consumed_owned_primary_param_unchanged_spec_22_10) {
    JitResult r = runAotChecked(kInitConsumed);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// =====================================================================
// #3205 — §21.9 / §3.6: a bare-statement discard of a class whose ONLY
// `owned` member is an unreferenced `owned` primary-ctor parameter (no user
// destructor, no other `owned` field) is correctly rejected as a compile
// error. The widening makes the slot non-trivial-dispose, so the §21.9
// bare-discard reject now fires where it did not pre-fix.
// =====================================================================
BEZEL_TEST(codegen_unused_owned_primary_param_leak_3205,
           bare_discard_of_unused_owned_only_param_rejected_spec_21_9) {
    auto r = runAot(
        std::string("module M; ") + kRes +
        "public class UnusedOnlyL(owned r: ResL, tag: int) "
        "end UnusedOnlyL; "
        "class C public static method Main(): int "
        "  new UnusedOnlyL(new ResL(1), 7); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());  // confirm not just _n leaking
}
