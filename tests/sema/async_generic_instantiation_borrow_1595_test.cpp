// Spec-derived sema tests for fix #1595 — §30.1 "Borrow-bearing parameters
// and results are forbidden" (v0.179, #1438): "for a generic async method
// the check is re-applied after substitution, so an instantiation whose
// type argument makes a parameter borrow-bearing is rejected at the
// instantiation site."
//
// #1518/#1519 (6b9ca21) shipped the DECLARATION-site half of §30.1 in the
// ownership pass (MethodOwnership::checkAsyncSignatureBorrowBearing): it
// runs once on a method's unsubstituted body, so a generic async method
// declared `async method Foo<T>(x: T): Task` is never flagged there — `T`
// carries no declaration, so isBorrowBearingRecordOrSpan(T) is trivially
// false. The rule's own text requires a SECOND check at each INSTANTIATION
// site, once the call's concrete type argument (explicit or inferred) is
// known — this file tests that second check
// (ExprTC::checkAsyncInstantiationBorrowBearing, ExprTypecheck_Calls.cpp),
// which reuses the shared predicate common::isBorrowBearingRecordOrSpan
// (src/common/OwnershipFacts.{hpp,cpp}) so the two checks never drift apart.
//
// Authority: §30.1 (v0.179, #1438; instantiation-site re-check, #1595).

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// The distinctive substrings of the two instantiation-site diagnostics.
const char* kParamMsg = "makes parameter";
const char* kResultMsg = "makes its result";

bool hasError(const bezel::test::SemaResult& r, const std::string& sub) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(sub) != std::string::npos) return true;
    }
    return false;
}

// A generic ASYNC method with the method type parameter `T` in BOTH its
// parameter and its result position (`Task<T>`) — every test below shares
// this one declaration so a single call site exercises both the parameter
// and the result leg of the instantiation-site check at once. `T` is a bare
// method type parameter at declaration time (no declaration of its own), so
// the DECLARATION-site check (#1438, ownership pass — not run by this
// sema-only harness) could never flag it; only the substituted call-site
// check under test here can.
const char* kAsyncIdSrc =
    "static class F "
    "  public static async method Work<T>(x: T): Task<T> "
    "    return x; "
    "  end Work; "
    "end F; ";

// The non-`async` twin of kAsyncIdSrc, used to confirm the rule is
// async-only (§30.1's borrow-bearing prohibition never applies to a
// synchronous method — its borrow is bounded by the ordinary call frame,
// §28.3).
const char* kSyncIdSrc =
    "static class G "
    "  public static method Work<T>(x: T): T "
    "    return x; "
    "  end Work; "
    "end G; ";

const char* kMainTrailer =
    "class C public static method Main(): int return 0; end Main; end C;";

}  // namespace

// §30.1 / §9.7 / #1595: an INFERRED type argument that is a borrow-bearing
// record (a record with a class-typed field, §9.7) makes `Work`'s parameter
// `x` borrow-bearing at THIS instantiation — rejected, even though `Work`'s
// own (unsubstituted) declaration is not itself borrow-bearing.
BEZEL_TEST(async_generic_instantiation_1595, param_inferred_record_rejected) {
    auto r = parseSemaAll(
        std::string("module M; class Res end Res; record Holder(R: Res); ") +
        kAsyncIdSrc +
        "class C public static method Main(): int "
        "  var res := new Res(); var h := new Holder(res); "
        "  var t := F.Work(h); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kParamMsg));
}

// Same instantiation as above: the RESULT type `Task<T>` is ALSO substituted
// to the borrow-bearing `Holder` here (T appears in both positions on
// kAsyncIdSrc), so the result leg of the check fires on the very same call.
BEZEL_TEST(async_generic_instantiation_1595, result_inferred_record_rejected) {
    auto r = parseSemaAll(
        std::string("module M; class Res end Res; record Holder(R: Res); ") +
        kAsyncIdSrc +
        "class C public static method Main(): int "
        "  var res := new Res(); var h := new Holder(res); "
        "  var t := F.Work(h); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kResultMsg));
}

// §30.1 / #1595: the same rejection via an EXPLICIT type argument
// (`Work<Holder>(h)`) — the explicit-type-args substitution path
// (ExprTypecheck_Calls.cpp's `explicitSubst`) is a separate code path from
// argument-inference and must be checked independently.
BEZEL_TEST(async_generic_instantiation_1595, param_explicit_record_rejected) {
    auto r = parseSemaAll(
        std::string("module M; class Res end Res; record Holder(R: Res); ") +
        kAsyncIdSrc +
        "class C public static method Main(): int "
        "  var res := new Res(); var h := new Holder(res); "
        "  var t := F.Work<Holder>(h); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kParamMsg));
    BEZEL_EXPECT_TRUE(hasError(r, kResultMsg));
}

// §30.1 / stdlib §4.15 / #1595: a builtin SPAN type argument
// (`ArraySpan<byte>`, a borrow-bearing VIEW with no user Record declaration,
// §9.7) is rejected the same way a user borrow-bearing record is — the
// shared predicate (common::isBorrowBearingRecordOrSpan) recognises both
// shapes identically.
BEZEL_TEST(async_generic_instantiation_1595, param_explicit_span_rejected) {
    auto r = parseSemaAll(
        std::string("module M; ") + kAsyncIdSrc +
        "class C public static method Main(): int "
        "  var arr := new Array<byte>(1); "
        "  var t := F.Work<ArraySpan<byte>>(arr.AsSpan()); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kParamMsg));
}

// ANTI-OVER-REJECT (§30.1 / #1595): an inferred `int` type argument is a
// primitive, never borrow-bearing — no error from this rule.
BEZEL_TEST(async_generic_instantiation_1595, param_inferred_int_accepted) {
    auto r = parseSemaAll(
        std::string("module M; ") + kAsyncIdSrc +
        "class C public static method Main(): int "
        "  var t := F.Work(42); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasError(r, kParamMsg));
    BEZEL_EXPECT_FALSE(hasError(r, kResultMsg));
}

// ANTI-OVER-REJECT (§30.1 / §9.7 / #1595): an inferred RECORD type argument
// whose only field is a value type (no class/interface-typed field) is NOT
// borrow-bearing (§9.7) — no error, mirroring the declaration-side #1518
// `param_runtime_managed_record_accepted` sibling test.
BEZEL_TEST(async_generic_instantiation_1595,
           param_inferred_value_only_record_accepted) {
    auto r = parseSemaAll(
        std::string("module M; record Pair(X: int, Y: int); ") + kAsyncIdSrc +
        "class C public static method Main(): int "
        "  var p := new Pair(1, 2); var t := F.Work(p); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasError(r, kParamMsg));
    BEZEL_EXPECT_FALSE(hasError(r, kResultMsg));
}

// §30.1 / §30.8 / #1595: `CancellationToken` is the single stdlib-certified
// borrow-bearing-record exception (its source-outlives-settlement obligation
// is certified normatively, §30.8) — admitted as a type argument that binds
// a parameter/result to it, exactly as it is admitted at the declaration
// site (#1518 `param_cancellation_token_accepted`).
BEZEL_TEST(async_generic_instantiation_1595,
           param_inferred_cancellation_token_accepted) {
    auto r = parseSemaAll(
        std::string("module M; ") + kAsyncIdSrc +
        "class C public static method Main(): int "
        "  var t := F.Work(CancellationToken.None); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasError(r, kParamMsg));
    BEZEL_EXPECT_FALSE(hasError(r, kResultMsg));
}

// §30.1 / #1595 (scope guard): the instantiation-site check is `async`-only,
// exactly like the declaration-site check (#1518
// `nonasync_span_param_accepted`) — a SYNCHRONOUS generic method's borrow
// captured by a value argument is bounded by the ordinary call frame
// (§28.3) and needs no state-machine-frame rule at all. Same borrow-bearing
// type argument as the rejected async case above, on the sync twin: no
// error.
BEZEL_TEST(async_generic_instantiation_1595,
           nonasync_generic_method_not_checked) {
    auto r = parseSemaAll(
        std::string("module M; class Res end Res; record Holder(R: Res); ") +
        kSyncIdSrc +
        "class C public static method Main(): int "
        "  var res := new Res(); var h := new Holder(res); "
        "  var t := G.Work(h); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasError(r, kParamMsg));
    BEZEL_EXPECT_FALSE(hasError(r, kResultMsg));
}

// Sanity: the generic method DECLARATION alone (never called) reports
// neither instantiation-site diagnostic — the check is call-site-triggered,
// not a latent declaration-time false positive. (The `kMainTrailer` module
// unit must still compile as a whole for the harness to run to this pass.)
BEZEL_TEST(async_generic_instantiation_1595, declaration_alone_not_flagged) {
    auto r = parseSemaAll(std::string("module M; ") + kAsyncIdSrc +
                          kMainTrailer);
    BEZEL_EXPECT_FALSE(hasError(r, kParamMsg));
    BEZEL_EXPECT_FALSE(hasError(r, kResultMsg));
}
