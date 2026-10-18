// Codegen — issue #2448 / spec §9.7 ("Records and disposal") + §28.9
// (runtime-managed string reference counting), docs revision v0.235.
//
// #1747's `collectRecordCallResultReleaseEligible` (LlvmBackend_Setup.cpp)
// releases a plain `var` RECORD local's string field(s) at scope end when
// the initializer is ANY CallExpr returning a record-with-string-fields
// type and the local is proven non-escaping. The gate is blind to the
// CALLEE's own body: it was reasoning (per its own, now-corrected, comment)
// that the callee always "mints the +1" the caller's release balances. No
// such mint ever existed in codegen (`emitRecordStringFieldRetains` /
// `currentReturnIsRecord_` were declared but never invoked/set anywhere —
// grep-verified, dead code from an intended-but-unbuilt design; #2453
// removed both, so they no longer appear in src/ at all). A record's
// string field in fact carries whatever +1/borrow state it had AT
// CONSTRUCTION, unchanged by a by-value return (a struct copy touches no
// refcount). That is sound only when the callee's returned value is a
// fresh, alias-free construction — NOT when it is a value ALREADY
// reachable from a longer-lived binding (a plain field read).
//
// VERIFY-FIRST: `Holder.Get(): P return this.f; end Get;` — `var p :=
// h.Get();` bound in an inner scope, `h` still live afterward. Confirmed
// via `bezelc --sanitize=address` on unmodified main:
// AddressSanitizer: heap-use-after-free — `p`'s scope-end release frees
// `h.f.s`'s string; the subsequent read of `h.f.s` is a genuine UAF. A
// second shape — `Mk(): P return new P(this.tag); end Mk;` (a FRESH
// construction whose CONSTRUCTOR ARGUMENT is itself a borrow, not a +1
// producer) — reproduces the identical UAF class, proving "return is a
// NewExpr" alone is not a sufficient callee-side proof either.
//
// Fix: `isProvablyFreshRecordFactoryMethod` /
// `isFreshRecordConstructionOfExactType` (LlvmBackend_Setup.cpp) make the
// caller-side gate reason about the CALLEE's actual body instead of only
// its static return type. Admitted ONLY when: the callee's ENTIRE body is
// exactly one statement, a `return new P(args...);` (or an equivalent
// expression-bodied `=> new P(args...)`) — deliberately narrow, avoiding a
// general control-flow walk that could silently miss a return hidden in an
// unrecognized construct and re-admit an unsafe callee; `P` is a
// POSITIONAL record with NO explicit constructor (so args bind directly
// and unconditionally to fields — an explicit constructor's body is opaque
// and could route an argument anywhere, or ignore it); every argument is
// positional (no name/ref/out/owned modifier); no positional field is
// itself a nested record/choice (out of scope for this check, matching
// #2425's separate nested-depth mechanism); and every STRING-typed
// positional field's argument is a string literal or a +1-producer
// expression (isPlusOneStringProducer) — never a borrow. Narrowing the
// gate is preferred over the wider, unsound one: the consequence for an
// excluded shape is a bounded LEAK, never a UAF.
//
// REGRESSION CHECK: the EXISTING #1747 tests
// (record_string_field_call_release_1747_test.cpp) still pass unchanged —
// `Mk(): P return new P($"hello", 8); end Mk;` is exactly the one-
// statement / NewExpr / +1-producer-arg shape this fix continues to admit,
// so that test was NOT encoding the #2448 bug (it happens to already be a
// SOUND shape) — verified by re-running it against this fix.

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"   // runAotChecked / runAotCheckedLenient
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::checkedToolchainSkipped;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

// ===========================================================================
// The issue's own repro: a callee that returns a BORROWED field
// (`return this.f;`) — not a fresh construction. Pre-fix: heap-use-after-
// free (ASan hard abort). Post-fix: the caller-side gate no longer admits
// this callee (its body is not a `new P(...)` return), so no release is
// registered — the program runs to completion (a leak may remain, since
// nothing else disposes a record's string fields either — §9.7 "a record
// has no disposer" — but NOT a crash).
// ===========================================================================
BEZEL_TEST(record_call_result_borrowed_return_uaf_2448,
           borrowed_field_return_no_uaf_spec_9_7_28_9) {
    auto r = runAotCheckedLenient(
        "module M; record P(s: string): ISerializable, IDeserializable end P; "
        "class Holder "
        "  public f: P; "
        "  public constructor(f: P) this.f := f; end constructor; "
        "  public method Get(): P return this.f; end Get; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var h := new Holder(new P(\"hello\" + \"-world-borrow-probe\")); "
        "  do "
        "    var p := h.Get(); "
        "    if p.s = \"\" then return 1; end; "
        "  end; "
        "  if h.f.s <> \"hello-world-borrow-probe\" then return 2; end; "
        "  return 42; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);   // false pre-fix: ASan heap-use-after-free
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// A second, distinct shape proving "the return is a NewExpr" alone is
// insufficient: the constructor ARGUMENT itself is a borrow (`this.tag`,
// a plain field read — not a `+`/interpolation +1 producer). Pre-fix:
// the SAME heap-use-after-free class (the caller's release frees the
// string `h.tag` still references). Post-fix: rejected by the per-
// argument isPlusOneStringProducer check, so no release is registered.
// This program happens to be fully LSan-clean post-fix too (Holder's own
// destructor releases `h.tag` once, unaliased by any leftover `p`
// release) — asserted via the lenient (UAF-focused) harness regardless,
// so the pin does not depend on that incidental cleanliness.
// ===========================================================================
BEZEL_TEST(record_call_result_borrowed_return_uaf_2448,
           fresh_construction_with_borrowed_ctor_arg_no_uaf_spec_9_7_28_9) {
    auto r = runAotCheckedLenient(
        "module M; record P(s: string): ISerializable, IDeserializable end P; "
        "class Holder "
        "  public tag: string; "
        "  public constructor(tag: string) this.tag := tag; end constructor; "
        "  public method Mk(): P return new P(this.tag); end Mk; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var h := new Holder(\"hello\" + \"-world-borrow-ctor-arg-probe\"); "
        "  do "
        "    var p := h.Mk(); "
        "    if p.s = \"\" then return 1; end; "
        "  end; "
        "  if h.tag <> \"hello-world-borrow-ctor-arg-probe\" then return 2; end; "
        "  return 42; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);   // false pre-fix: ASan heap-use-after-free
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// Documented coverage cost: a MULTI-STATEMENT factory body (a local
// computed first, then a fresh construction returned) is conservatively
// REJECTED by the one-statement-body requirement, even though this
// particular body IS actually alias-free (the fresh-construction argument
// is a +1 producer, `x`, itself bound from a +1 producer with no other
// alias). Pinned as "no crash", NOT "leak-free" — the bounded-leak
// trade-off the coordinator's brief explicitly sanctioned over widening
// the gate to a general (and riskier) control-flow walk.
// ===========================================================================
BEZEL_TEST(record_call_result_borrowed_return_uaf_2448,
           multi_statement_factory_body_conservatively_excluded_spec_9_7) {
    auto r = runAotCheckedLenient(
        "module M; record P(s: string): ISerializable, IDeserializable end P; "
        "class C public static method Mk(): P "
        "  var x := \"hello\" + \"-world-multi-stmt-probe\"; "
        "  return new P(x); "
        "end Mk; "
        "public static method Main(): int "
        "  var p := Mk(); "
        "  if p.s = \"\" then return 1; end; "
        "  return 42; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);   // no crash — the excluded shape's release
                                // simply does not fire (a bounded leak,
                                // not asserted leak-free here)
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}
