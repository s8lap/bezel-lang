// fix #4793 (stdlib §6.5, v0.307): SharedEventSource<T> deliberately does NOT
// implement IEvent<T> — an upcast `var e: IEvent<T> := src` (where `src` is a
// SharedEventSource<T>) must be REJECTED at sema. The legal
// `var e: ISharedEvent<T> := src` upcast must still be ADMITTED — SharedEventSource<T>
// implements ISharedEvent<T> per its declaration.
//
// This file pins both halves of that pair at sema time using the shipping
// `runAot` / `runAotChecked` AOT oracle (the JIT harness is a lenient oracle
// that bypasses the ownership/definite-assignment passes — the bug here
// surfaces in the typecheck pass, which AOT runs and which JIT skips, so
// runAot* is the only oracle the assertion can be made on).
//
// Pre-#4793 the upcast `SharedEventSource<int>` -> `IEvent<int>` compiles
// silently and the program runs (the IEvent<S>.Subscribe call is also accepted
// because `e` is now IEvent<S>, where Subscribe is `scoped`-typed and admits a
// capturing handler). Post-#4793 the upcast is a §6.5 compile error naming the
// offending source/interface pair, and the legal ISharedEvent<S> upcast
// (parallel shape, just a different interface) still compiles end-to-end and
// runs the §24.3 Raise path.
//
// Test plan:
//   T1  REJECT row: the exact #4793 repro from the issue. The diagnostic
//       must contain a UNIQUE substring of the §5.1 "not implicitly
//       convertible to the target type" text — the existing
//       reportNotConvertible message (no new diagnostic introduced here).
//   T2  COMPILE-ACCEPT row: the parallel ISharedEvent<S> upcast — compile-
//       accept only (Subscribe on the interface face is the independent
//       #2457 gap, not in #4793's scope — the §6.5 runtime itself is the
//       additional gap l6_5_0017 marks, also out of scope here).

#include <string>

#include "support/aot_support.hpp"          // runAot / runAotChecked
#include "support/aot_result_asserts.hpp"   // diagnosticMessageContains
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// T1 — REJECT (the bug). The diagnostic surfaces through the §5.1 "not
// implicitly convertible" path: variantGenericAssignable (L7916) sees
// SharedEventSource as a fully-modeled builtin (L7553's
// builtinBorrowingCollectionFullyModeled), finds no `IEvent<int>` ancestor
// in builtinInterfaceAncestorsOf (L7485, the registered table starting at
// L7403), and returns false. checkAssignable then calls reportNotConvertible
// (L4343), whose text is "the N has a type that is not implicitly convertible
// to the target type (§5.1)" — §5.1's general reuse, not a new diagnostic.
// Per cycle-25 oracle discipline, the test pins a UNIQUE substring of that
// message (not just "type error" / "ERROR"), and asserts `r.ran == false`
// because a wrong-acccpt pre-fix would have produced `r.ran == true` and
// exit==0.
//
// Pre-fix: program compiles (wrong); r.ran == true, exit == 0; the `not
// implicitly convertible` substring is absent.
// Post-fix: program is rejected at sema; r.ran == false; exit == -999
// sentinel; the substring is present.
int runToExit(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// Substring of the §5.1 not-convertible message that pin-points the rejection
// site (vs. a generic "type error" or `EXPECT_NE(exitCode, 0)` shape). It is
// the literal text emitted by `reportNotConvertible` (L4343) without a `reason`
// suffix — the same message format tests/conformance/rules/cases/l4_4_0007.bzl
// relies on for Stack<Dog>→IList<Dog>, which goes through the same path. A
// test that only asserted `r.ran == false` would be vacuously green if the
// compiler crashed or a later reject fired; the substring pins *this* rule.
constexpr const char* kNotConvertible =
    "not implicitly convertible to the target type";

const char* kEventRejectPreamble =
    "module M;\n"
    "import System.Threading;\n";

} // namespace

// ============================================================================
// fix #4793 / §6.5 (v0.307): SharedEventSource<T> deliberately does NOT
// implement IEvent<T> — `var e: IEvent<T> := src` is REJECTED at sema.
// ============================================================================
BEZEL_TEST(shared_event_source_upcast_4793,
           shared_event_source_to_ievent_rejected_spec_6_5_issue_4793) {
    const std::string src = std::string(kEventRejectPreamble) +
        "class C public static method Demo(): int "
        "  using var src := new SharedEventSource<int>(); "
        "  var e: IEvent<int> := src; "
        "  return 0; "
        "end Demo; end C;";
    auto r = runAot(src);
    // Post-fix: rejected at sema — the program never ran.
    BEZEL_EXPECT_FALSE(r.ran);
    // Pin the diagnostic by the §5.1 not-convertible substring (kNotConvertible
    // above). A weaker "no diagnostics" or "EXPECT_NE(exitCode, 0)" oracle
    // would not pin the rule (Stack<Dog>→IList<Dog>'s #1851 fix surfaces
    // through the same path; both belong here).
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNotConvertible));
    // And, transitively, exit code == -999 sentinel for compile-time reject
    // (rather than a runtime failure with a real exit code).
    BEZEL_EXPECT_EQ(runToExit(src), -999);
}

// ============================================================================
// fix #4793 / §6.5 (v0.307): the PARALLEL ISharedEvent<S> upcast is the
// legal face — SharedEventSource<S> implements it, ISharedEvent<S>'s
// Subscribe is capture-free, the upcast itself compiles end-to-end. Used as
// the accept-control to prove the rejection above isn't an over-rejection
// blanketing every upcast out of SharedEventSource<S>.
//
// COMPILE-ACCEPT row only: T2 deliberately does NOT exercise Subscribe on
// the `ISharedEvent<S>`-typed receiver. The interface-method dispatch
// (`ISharedEvent.Subscribe`) is the #2457 gap (l6_5_0017 / l6_5_0007 marks
// it as a runtime+acceptability gap independent of #4793's rejection scope)
// and the test would fail-on-both-sides for that reason. The §6.5 runtime
// itself (Raise + capture-free enforcement on the concrete `Raise` and
// `Subscribe` positions on SharedEventSource<S>) is also out of scope
// here — the contract for #4793 is the upcast's COMPILE outcome.
// ============================================================================
BEZEL_TEST(shared_event_source_upcast_4793,
           shared_event_source_to_ishared_event_accepted_spec_6_5_issue_4793) {
    // The canonical legal-upcast shape: `var e: ISharedEvent<int> := src;`
    // where `src` is a SharedEventSource<int>. Compile-accepted. We do not
    // call Subscribe through `e` here (see header above).
    const std::string src = std::string(kEventRejectPreamble) +
        "class C public static method Main(): int "
        "  using var src := new SharedEventSource<int>(); "
        "  var e: ISharedEvent<int> := src; "
        "  return 0; "
        "end Main; end C;";
    // ACCEPT-ORACLE: the legal upcast COMPILES (no error diagnostic). On the
    // pre-fix tree this was already passing (variantGenericAssignable's
    // builtin-vs-builtin path conservatively accepts when no ancestor is
    // registered), and post-fix it MUST still pass — the new entry in
    // builtinInterfaceAncestors (L7403/L7440) is the named-ancestor match
    // ISharedEvent<int>, exactly what §6.5 says. A failure here would mean
    // the reject scope from T1 broadened beyond SharedEventSource→IEvent<S>.
    auto r = runAot(src);
    // The hard accept pin: NO ERROR-level diagnostic at all.
    bool hadErrorDiag = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error) { hadErrorDiag = true; break; }
    }
    BEZEL_EXPECT_FALSE(hadErrorDiag);
}
