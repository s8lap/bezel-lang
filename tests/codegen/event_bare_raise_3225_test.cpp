// Codegen / runtime — the §24 raise-syntax BARE-CALL form `Ev(payload)`
// silently no-ops on a DIRECT class event, issue #3225 (§24.3 / §24.5).
//
// The bare-call raise form is the §24 raise-syntax shorthand for
// `this.Ev.Raise(payload)` inside the declaring class: `Ev(payload)` inside
// the Fire() method must invoke every subscribed handler with `payload`.
// Before the fix, codegen's `tryLowerEventCall` only recognized the
// `.Subscribe`/`.Raise`/etc. member-access callee shape; the bare-call form
// (whose callee is a bare IdentifierExpr pointing to the EventSymbol, NOT
// wrapped in a MemberAccessExpr) fell through to the ordinary dispatch path,
// where the EventSymbol callee had no matching arm and the whole Raise was
// silently dropped. The program compiled cleanly, ran, exited 0 — and the
// subscribed handler NEVER observably fired.
//
// Sister fix #3211 carried the Subscribe-handler-typing end (the explicit
// `s.Ev.Subscribe(...)` shape) across a module boundary; #3225 carries the
// matching direct-class bare-call raise in a single module. The two are the
// same §24.3 surface, observed from opposite ends.
//
// VERIFY-FIRST (measured pre-fix on HEAD 6da66d4):
//   The repro's compiled binary ran clean but printed NOTHING and exited 0;
//   the subscribed lambda never fired. The interface-typed variant
//   (`i.Ready.Subscribe(...)` on an `ISource`-typed receiver) observably
//   fires — the differing root cause is the bare IdentifierExpr callee shape
//   vs the MemberAccessExpr callee shape, NOT the receiver's static type.
//
// Test plan (per §24.3 / §24.5):
//   3225-a  Direct-class event, bare-call raise: the issue's repro shape.
//          Subscribe → Raise → assert the handler ran (exit code = 5).
//   3225-b  Direct-class event, bare-call raise with NO payload (no-payload
//          event form). Subscribe → Raise() → assert the handler ran
//          (exit code = 1).
//   3225-c  Direct-class event, explicit `.Raise(payload)` form. Control
//          twin for 3225-a: the `.Raise` spelling was already correct
//          pre-fix (regression-pin). exit code = 5.
//   3225-d  Direct-class event, bare-call raise after dispose: the bare-call
//          Raise reaches the SAME backing source the Dispose path severed.
//          exit code = 5.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

int run(const std::string& src) {
    JitResult r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// 3225-a — direct-class event, bare-call `Ev(payload)`. The subscribed handler
// MUST fire (exit code = 5). Pre-fix: silent no-op, exit 0, stdout empty.
// The handler is capture-admitting (a class instance is captured by value
// pointer; §28); §22.12 rule 5's capture-free restriction is for STATIC
// events only, not direct instance events.
BEZEL_TEST(codegen_event_bare_raise_3225, direct_class_bare_call_fires) {
    const std::string src =
        "module M; "
        "record Pay(N: int); "
        "class Src "
        "  public event Ev: Pay; "
        "  public constructor() end constructor; "
        "  public method Fire() Ev(new Pay(5)); end Fire; "
        "end Src; "
        "class Box public total: int := 0; "
        "  public method Add(n: int) total := total + n; end Add; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); "
        "  using var s := new Src(); "
        "  using var sub := s.Ev.Subscribe(lambda(e) => do b.Add(e.N); end); "
        "  s.Fire(); "
        "  return b.total; "                              // expect 5
        "end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 5);
}

// 3225-b — direct-class NO-PAYLOAD event, bare-call `Ev()`. The subscribed
// handler MUST fire on each Raise. Pre-fix: silent no-op, exit 0.
// Mirrors the `Raise()` / `Raise(())` equivalence documented in §24.5 / #737
// — `Raise()` with no payload reaches every subscribed handler.
BEZEL_TEST(codegen_event_bare_raise_3225,
           direct_class_no_payload_bare_call_fires) {
    const std::string src =
        "module M; "
        "class Src "
        "  public event Ev; "
        "  public constructor() end constructor; "
        "  public method Fire() Ev(); end Fire; "
        "end Src; "
        "class Box public total: int := 0; "
        "  public method Inc() total := total + 1; end Inc; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); "
        "  using var s := new Src(); "
        "  using var sub := s.Ev.Subscribe(lambda() => do b.Inc(); end); "
        "  s.Fire(); s.Fire(); "
        "  return b.total; "                              // expect 2
        "end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 2);
}

// 3225-c — direct-class event, EXPLICIT `.Raise(payload)` form. The control
// twin for 3225-a: this spelling was already correct pre-fix (codegen's
// tryLowerEventCall MemberAccessExpr-callee path), so it stays green. Pins
// the regression: the fix to the bare-call path did NOT regress the
// member-access path.
BEZEL_TEST(codegen_event_bare_raise_3225, direct_class_member_raise_fires) {
    const std::string src =
        "module M; "
        "record Pay(N: int); "
        "class Src "
        "  public event Ev: Pay; "
        "  public constructor() end constructor; "
        "  public method Fire() Ev.Raise(new Pay(5)); end Fire; "
        "end Src; "
        "class Box public total: int := 0; "
        "  public method Add(n: int) total := total + n; end Add; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); "
        "  using var s := new Src(); "
        "  using var sub := s.Ev.Subscribe(lambda(e) => do b.Add(e.N); end); "
        "  s.Fire(); "
        "  return b.total; "                              // expect 5
        "end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 5);
}

// 3225-d — direct-class event, bare-call raise after Dispose. The bare-call
// Raise must reach the SAME backing source that Subscription.Dispose severed.
// Disposes the subscription at the inner `do…end` scope; the outer Raise
// then fires zero handlers, the post-dispose Fire adds 5.
BEZEL_TEST(codegen_event_bare_raise_3225,
           direct_class_bare_call_after_dispose_no_fire) {
    const std::string src =
        "module M; "
        "record Pay(N: int); "
        "class Src "
        "  public event Ev: Pay; "
        "  public constructor() end constructor; "
        "  public method Fire() Ev(new Pay(5)); end Fire; "
        "end Src; "
        "class Box public total: int := 0; "
        "  public method Add(n: int) total := total + n; end Add; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); "
        "  using var s := new Src(); "
        "  do "
        "    using var sub := s.Ev.Subscribe(lambda(e) => do b.Add(e.N); end); "
        "  end; "                                        // sub disposed at scope end
        "  s.Fire(); "                                    // handler gone — no-op
        "  return b.total; "                              // expect 0
        "end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}