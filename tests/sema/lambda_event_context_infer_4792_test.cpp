// narrow #4792 (WIP, parked: needs #4857) (§6.5 / §27.4, v0.307): the event
// pattern's `+=` / `-=` on an `EventSource<T>` / `AsyncEventSource<T>` /
// `SharedEventSource<T>` field desugars to `Add(handler)` / `Remove(handler)`,
// both with a typed-parameter lambda signature — `(T) -> ()`, `(T) -> Task`,
// or `(T) -> ()` (capture-free) depending on which source class is at the
// target. The lambda's single untyped parameter inherits T positionally.
//
// The legacy `EventSymbol` thread for `+=` / `-=` in
// `src/sema/ExprTypecheck.cpp`'s `visitStmt(AssignmentStmt)` was the only
// place this inference was ever performed; it keys on the target being an
// `EventSymbol`. But §24 "Events (retired member kind)" is explicit that
// events are not a member kind in v1 — the §6.5 pattern (an `owned readonly
// EventSource<T>` field + an `IEvent<T>` property) is the only form a v1
// program can write, and the `EventSymbol` branch is dead in practice. So
// `_e += lambda(n) => …` on the field itself fell through silently: the
// lambda's single untyped parameter got no target type and the §27.4 "no
// target type to infer from" reject fired.
//
// This file pins the §27.4 inference on the §6.5 producer side AND on the
// §6.5 consumer (`IEvent<T>`) side. The cross-domain `ISharedEvent<T>`
// twin case was originally here as case 3 but is tracked separately —
// `tryLowerEventCall` does not recognize `ISharedEvent<T>.Subscribe` or
// `SharedEventSource<T>.Raise` (separate from #4857's compound-assignment
// arm); see follow-up for the move.
//
// Cases 1/4 are RED until #4857 (codegen: EventSource += / -= lambda
// doesn't emit Add call) lands, which is the correct state — narrowing the
// oracles to compile-only would HIDE the silent runtime crash, owner rule
// per main ruling. Case 5 is RED until #4869 (sema: position-aware
// assignability check on Add/Remove slot) lands. The Subscribe-call arm
// (case 2) goes through the call-argument seeding which already handled the
// IEvent<T> face and emits correct codegen for Subscribe + Raise, so it
// passes end-to-end on this tree.
//
// Cases that would still fail for §22.8 (readonly-onceness, separate from
// §27.4 inference) are deliberately avoided: the `+=` is in a CONSTRUCTOR
// body where the field is a PLAIN `owned` field initialized in the same body,
// not a `readonly` field with a field-initializer (the §22.8 second-assignment
// violation that l6_5_0009 carries is a separate concern, out of #4792's
// scope).

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"   // runAot — AOT + native exec oracle
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Finds the FIRST diagnostic whose message contains `needle`, or nullptr.
const bezel::Diagnostic* findDiag(const bezel::test::JitResult& r,
                                   const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return &d;
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// ACCEPT — the §6.5 event pattern threads the payload type T into the
// handler lambda's single untyped parameter. Each of these compiled
// pre-fix with `parameter `_` / `n` needs a type annotation: no target
// type to infer from (§27.4)`.
// ---------------------------------------------------------------------------

// `EventSource<int> += lambda(n) => …` in a constructor (the form
// l6_5_0009 uses; the plain `owned` field + constructor-body init
// avoids the §22.8 readonly-onceness side-issue, which is outside #4792).
// Pre-fix: §27.4 "no target type to infer from" reject on `n`. Post-fix:
// `n` is inferred `int`, the body prints it, the program runs and prints
// `7`. Pins the §6.5 pattern's producer-side `+=` form. (RED until #4857:
// the codegen half of `+=` on EventSource<T> is a separate defect that
// #4792 does not scope. Narrowing the oracle to compile-only would HIDE
// that silent crash per owner rule.)
BEZEL_TEST(lambda_event_context_infer_4792,
           event_source_plus_eq_infers_payload_spec_27_4) {
    auto r = runAot(
        "module M; import System; "
        "class Beeper "
        "  private owned _beep: EventSource<int>; "
        "  public constructor() "
        "    _beep := new EventSource<int>(); "
        "    _beep += lambda(n) => Console.WriteLine(n); "
        "  end constructor; "
        "  public method Fire(n: int) _beep.Raise(n); end Fire; "
        "end Beeper; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Beeper(); "
        "    b.Fire(7); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("7") != std::string::npos);
}

// `IEvent<int>.Subscribe(lambda(_) => …)` — the call-argument thread on
// `IEvent<T>.Subscribe(scoped handler: (T) -> ())` already threads T
// positionally into the lambda's parameter list (the `(int) -> ()`
// subscription in l6_5_0001 / l6_5_0007 line 34 has always typed `_` as
// `int`). Pre-fix: this ACCEPT path was already correct; included here so
// the brief's regression-guard requirement ("explicit annotation in the
// same context must STILL compile clean") has a positive control beside
// it. End-to-end passes — the call-arg thread AND the IEvent<T>.Raise
// codegen both work. Pins the IEvent<T> face.
//
// Per §6.5, `IEvent<T>` has only `Subscribe` — it deliberately omits
// `Raise`. The producer-side `Raise` is on the underlying
// `EventSource<T>`, so the test raises through a `Fire` method on the
// owning class (the §6.5 pattern's intended public surface for
// triggering). Pre-fix would have rejected `n` with §27.4 "no target
// type to infer from" on the Subscribe arm regardless of this detail.
BEZEL_TEST(lambda_event_context_infer_4792,
           ievent_subscribe_untyped_param_already_accepts_pre_fix) {
    auto r = runAot(
        "module M; import System; "
        "class Ticker "
        "  private owned readonly _tick: EventSource<int> := new(); "
        "  public property Tick: IEvent<int> => _tick; "
        "  public method Fire(n: int) _tick.Raise(n); end Fire; "
        "end Ticker; "
        "class C "
        "  public static method Main(): int "
        "    using var t := new Ticker(); "
        "    using var sub := t.Tick.Subscribe(lambda(n) => Console.WriteLine(n)); "
        "    t.Fire(9); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("9") != std::string::npos);
}

// `SharedEventSource<int>.Subscribe(lambda(_) => …)` — the SHARED-source
// twin: `ISharedEvent<T>.Subscribe(handler: (T) -> ())` (capture-free
// admission per §6.5 / §28.14). MOVED to follow-up: `tryLowerEventCall` in
// `src/codegen/LlvmBackend_Calls2.cpp` recognizes `IEvent<T>` for
// `.Subscribe` and `EventSource<T>`/`AsyncEventSource<T>` for all event
// methods, but does NOT recognize `ISharedEvent<T>.Subscribe` or
// `SharedEventSource<T>.Raise` (separate codegen gap from #4857's
// compound-assignment arm). The follow-up test will re-pin this row once
// the `tryLowerEventCall` recognition is added.

// `EventSource<int> += lambda(n: int) => …` (EXPLICIT annotation) on the
// SAME pattern — must compile clean and not double-infer / collide with
// the seeded payload type. The lambda's `n: int` annotation wins over the
// target-type thread (the `p.type` branch in the lambda visitor runs
// unconditionally before the `pendingLambdaParamType_` fallback), so a
// matching type is admitted and a mismatched type is rejected at the body
// (not at the annotation site). This row guards that the new
// `pendingLambdaParamType_` thread does not double-infer or reject the
// explicit annotation. (RED until #4857: same codegen path as case 1.)
BEZEL_TEST(lambda_event_context_infer_4792,
           event_source_plus_eq_explicit_annotation_still_accepts_spec_27_4) {
    auto r = runAot(
        "module M; import System; "
        "class Beeper "
        "  private owned _beep: EventSource<int>; "
        "  public constructor() "
        "    _beep := new EventSource<int>(); "
        "    _beep += lambda(n: int) => Console.WriteLine(n); "
        "  end constructor; "
        "  public method Fire(n: int) _beep.Raise(n); end Fire; "
        "end Beeper; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Beeper(); "
        "    b.Fire(13); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("13") != std::string::npos);
}

// `EventSource<int> += lambda(n: string) => …` — EXPLICIT annotation
// that DISAGREES with the seeded payload type. The body is unreachable
// here, but the assignment position (§5.1) rejects the mismatch: the
// handler parameter type `string` cannot satisfy the `Add(scoped
// handler: (int) -> ())` position. This is the load-bearing guard that
// the new thread supplies the CORRECT type — if it threaded a wrong type
// (e.g. `EventSource<int>` instead of `int`, or `object`), this row
// would flip the other way and the call would either still type-check
// silently (wrong-value) or reject with an unhelpful diagnostic.
BEZEL_TEST(lambda_event_context_infer_4792,
           event_source_plus_eq_wrong_annotation_rejected_spec_27_4) {
    auto r = runAot(
        "module M; import System; "
        "class Beeper "
        "  private owned _beep: EventSource<int>; "
        "  public constructor() "
        "    _beep := new EventSource<int>(); "
        "    _beep += lambda(n: string) => Console.WriteLine(n); "
        "  end constructor; "
        "  public method Fire(n: int) _beep.Raise(n); end Fire; "
        "end Beeper; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Beeper(); "
        "    b.Fire(0); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
    // The §5.1 assignability check on `+=` reports the disagreement — the
    // error text is whatever the standard surface emits at the
    // Add-position mismatch (typically a `cannot assign` / `cannot
    // convert` style diagnostic). What we pin here is that a row where
    // the seeded payload type is consulted MUST reject a wrong
    // annotation, not silently accept it: an oracle that flips to PASS
    // would mean the new thread isn't actually being consulted at all.
    // Belt-and-braces: the helper also reads `r.message` and rejects
    // crash/signal/timeout/ASan reports (issue #3892 — a bare
    // `BEZEL_EXPECT_FALSE(r.ran)` cannot tell a real rejection apart from
    // a compiler crash), which makes this row protective for
    // check_ambiguous_ran_false.py.
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    const auto* reject =
        findDiag(r, "string") != nullptr ? findDiag(r, "string")
                                         : findDiag(r, "cannot");
    BEZEL_EXPECT_TRUE(reject != nullptr);
}
