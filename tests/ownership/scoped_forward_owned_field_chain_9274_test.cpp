// Ownership over-rejection — fix #9274. A method forwards its OWN `scoped`
// handler parameter to a `scoped` call on an OWNED-FIELD chain of `this`
// (`_changed.Add(h)`, `this._changed.Add(h)`, `_changed += h`,
// `_sub := _changed.Subscribe(h)`, `return _changed.Subscribe(h)`,
// `_child.Reg(h)`), and the BLOCK-bodied callee was rejected.
//
// Spec: §28.14 (v0.359), the callee side of a `scoped` parameter: "The callee
// may store the callback, but only in `scoped` fields of the bounding object
// itself (below), or forward it to another `scoped` parameter whose bounding
// object is the same object". The inner call's bounding object is resolved by
// §28.14's own forms at its own site:
//
//   "3. a call whose return is `owned` and is directly assigned to (or
//    initializes) an `owned` field of `this` → `this`;"
//   "4. a call whose receiver is a member chain rooted at `this` or a local
//    binding in which every hop is an `owned` field — → the chain head:
//    `this` for `this._clicked.Add(h)` (and the bare `_clicked += h` spelling
//    — the stdlib §6.5 event pattern) … A chain containing a non-owned
//    (borrow) field hop, an indexer, a getter, or an intervening call
//    resolves no bounding object".
//
// and §17.2: "`lhs += h` rewrites to `lhs.Add(h)` … and `lhs += h` evaluates
// `lhs` only as the receiver of the desugared call". §28.14's call-bounded
// default names exactly this remedy: "a callee that needs to register a
// callback it received must itself declare the parameter `scoped` (the
// forwarding rule above then requires the same bounding object)".
//
// THE DEFECT. The callee-side forward check (`scopedCalleeWalkExpr`,
// "Admitted context 3") admitted a forward only when the callee expression was
// literally `this.M` or a bare `M` (form 5 on `this`): it predates v0.180's
// forms 3/4. A `+=` was walked as a VALUE READ of the handler.
//
// ROWS 1–8 are the issue's shapes, RED on r30-c1 (rejected with the forward
// diagnostic, or the value-read one for `+=`) and GREEN with the fix. Each
// loops 200x and prints the per-iteration hit count under checked disposal,
// so a double release of the forwarded closure (the #9056 defect class, which
// the `+=` arm now needs a sink retain for) or a missed invocation shows in
// stdout. Two spec-decided CONTROLS (green on both trees) keep the forms that
// resolve no `this` bound rejected: a GETTER hop and a BORROW-field hop.
// Further controls (parameter receiver, `using var` local in the callee) are
// in the conformance corpus.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

std::string repeatLine(const std::string& v, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += v + "\n";
    return out;
}

// §28.14's forward diagnostic, verbatim.
const char* kScopedForward =
    "`handler` is a `scoped` parameter — it may be forwarded only to another "
    "`scoped` parameter of a call on the same bounding object (`this`) "
    "(§28.14, v0.175)";

const char* const kCounter =
    "module P; "
    "public class Counter2 "
    "  private _n: int := 0; "
    "  public method Hit(t: double) => _n := _n + 1; "
    "  public property N: int => _n; "
    "end Counter2; "
    "public class Child "
    "  private scoped _h: ((double) -> ())?; "
    "  public method Reg(scoped h: (double) -> ()) => _h := h; "
    "  public method Fire(t: double) "
    "    _h?(t); "
    "  end Fire; "
    "end Child; ";

// A Hub whose `On` member is `onMember`, with every field the rows need.
std::string hub(const std::string& onMember) {
    return "public class Hub "
           "  private owned readonly _changed: EventSource<double> := "
           "    new EventSource<double>(); "
           "  private owned _sub: Subscription? := null; "
           "  private owned readonly _child: Child := new Child(); "
           "  public property Changed: IEvent<double> => _changed; "
           "  public method Fire(t: double) "
           "    _changed.Raise(t); "
           "    _child.Fire(t); "
           "  end Fire; " +
           onMember +
           "end Hub; ";
}

// A 200-iteration Main: register a capturing lambda through `hub.On`, fire
// once, print the hit count.
std::string program(const std::string& onMember) {
    return std::string(kCounter) + hub(onMember) +
           "public class Program "
           "  public static method Main() "
           "    for var i in 0..<200 do "
           "      using var c := new Counter2(); "
           "      using var hub := new Hub(); "
           "      hub.On(lambda(t) => c.Hit(t)); "
           "      hub.Fire(1.0); "
           "      Console.WriteLine(c.N); "
           "    end; "
           "  end Main; "
           "end Program;";
}

void expectClean(const JitResult& r, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// 1 — THE ISSUE'S `On`: `_changed.Add(handler)` in a block body. Form 4: the
// receiver `_changed` is an implicit `this` chain through one `owned` hop.
BEZEL_TEST(ownership_9274_scoped_forward_owned_chain,
           block_add_on_owned_field_admitted_spec_28_14_form_4) {
    auto r = runAotChecked(program(
        "  public method On(scoped handler: (double) -> ()) "
        "    _changed.Add(handler); "
        "  end On; "));
    expectClean(r, repeatLine("1", 200));
}

// 2 — the explicit `this._changed.Add(handler)` spelling (spec form 4's own
// example).
BEZEL_TEST(ownership_9274_scoped_forward_owned_chain,
           block_this_qualified_add_admitted_spec_28_14_form_4) {
    auto r = runAotChecked(program(
        "  public method On(scoped handler: (double) -> ()) "
        "    this._changed.Add(handler); "
        "  end On; "));
    expectClean(r, repeatLine("1", 200));
}

// 3 — THE ISSUE'S `On2`: `_changed += handler` (§17.2: the desugared
// `_changed.Add(handler)`; spec form 4 "the bare `_clicked += h` spelling").
// Base: rejected as a value read. With the fix the sink also takes its own
// reference on the borrowed formal (the #9056 discipline), so the 200 loops
// run clean under checked disposal.
BEZEL_TEST(ownership_9274_scoped_forward_owned_chain,
           block_plus_eq_on_owned_field_admitted_spec_28_14_form_4_spec_17_2) {
    auto r = runAotChecked(program(
        "  public method On(scoped handler: (double) -> ()) "
        "    _changed += handler; "
        "  end On; "));
    expectClean(r, repeatLine("1", 200));
}

// 4 — the arrow-bodied `=> _changed += handler` reaches the same assignment
// statement walk, so it was rejected on the base tree too.
BEZEL_TEST(ownership_9274_scoped_forward_owned_chain,
           arrow_plus_eq_on_owned_field_admitted_spec_28_14_form_4_spec_17_2) {
    auto r = runAotChecked(program(
        "  public method On(scoped handler: (double) -> ()) "
        "    => _changed += handler; "));
    expectClean(r, repeatLine("1", 200));
}

// 5 — THE ISSUE'S `On3`: `_sub := _changed.Subscribe(handler)`. Form 3: an
// `owned` result directly assigned to an `owned` field of `this`.
BEZEL_TEST(ownership_9274_scoped_forward_owned_chain,
           block_subscribe_into_owned_field_admitted_spec_28_14_form_3) {
    auto r = runAotChecked(program(
        "  public method On(scoped handler: (double) -> ()) "
        "    _sub := _changed.Subscribe(handler); "
        "  end On; "));
    expectClean(r, repeatLine("1", 200));
}

// 6 — THE ISSUE'S `OnSub`: `return _changed.Subscribe(handler);` from an
// owned-return method (form 4 on the inner call). The caller holds the
// returned handle in a `using var`.
BEZEL_TEST(ownership_9274_scoped_forward_owned_chain,
           block_return_subscribe_on_owned_field_admitted_spec_28_14_form_4) {
    auto r = runAotChecked(
        std::string(kCounter) +
        hub("  public method OnSub(scoped handler: (double) -> ()): "
            "      owned Subscription "
            "    return _changed.Subscribe(handler); "
            "  end OnSub; ") +
        "public class Program "
        "  public static method Main() "
        "    for var i in 0..<200 do "
        "      using var c := new Counter2(); "
        "      using var hub := new Hub(); "
        "      using var s := hub.OnSub(lambda(t) => c.Hit(t)); "
        "      hub.Fire(1.0); "
        "      Console.WriteLine(c.N); "
        "    end; "
        "  end Main; "
        "end Program;");
    expectClean(r, repeatLine("1", 200));
}

// 7 — a USER-AUTHORED `scoped` callee on an `owned readonly` child field:
// `_child.Reg(handler)` (form 4, one `owned` hop; spec §28.14's registration-
// lifetime residual "or a storing owned child").
BEZEL_TEST(ownership_9274_scoped_forward_owned_chain,
           block_forward_to_owned_user_child_admitted_spec_28_14_form_4) {
    auto r = runAotChecked(program(
        "  public method On(scoped handler: (double) -> ()) "
        "    _child.Reg(handler); "
        "  end On; "));
    expectClean(r, repeatLine("1", 200));
}

// 8 — form 3 with a PARAMETER receiver: `_reg := tok.Register(cb)`. The forms
// are "tested in order; the first that resolves determines the bounding
// object", and form 3 (the owned `Subscription` assigned to an `owned` field
// of `this`) resolves `this` before form 5 would give the parameter `tok`. The
// sink is `CancellationToken.Register`, so this also keeps #9056's Register
// retain exercised on a spec-legal spelling (the arrow `=> tok.Register(cb)`
// is the form-5 shape §28.14 rejects; see #9201).
BEZEL_TEST(ownership_9274_scoped_forward_owned_chain,
           block_register_into_owned_field_admitted_spec_28_14_form_3) {
    auto r = runAotChecked(
        "module P; "
        "import System.Threading; "
        "public class Box "
        "  private _v: int := 0; "
        "  public method Bump() => _v := _v + 1; "
        "  public property V: int => _v; "
        "end Box; "
        "public class Reg "
        "  private owned _reg: Subscription? := null; "
        "  public method OnCancel(tok: CancellationToken, scoped cb: () -> ()) "
        "    _reg := tok.Register(cb); "
        "  end OnCancel; "
        "end Reg; "
        "public class Program "
        "  public static method Main() "
        "    for var i in 0..<200 do "
        "      using var box := new Box(); "
        "      using var r := new Reg(); "
        "      using var cts := new CancellationTokenSource(); "
        "      var tok := cts.Token; "
        "      r.OnCancel(tok, lambda() => box.Bump()); "
        "      cts.Cancel(); "
        "      Console.WriteLine(box.V); "
        "    end; "
        "  end Main; "
        "end Program;");
    expectClean(r, repeatLine("1", 200));
}

// CONTROL 1 (spec-decided, green on both trees) — a GETTER hop: form 4 "A
// chain containing … a getter … resolves no bounding object" (the
// transparent-getter lift is "recorded, not shipped").
BEZEL_TEST(ownership_9274_scoped_forward_owned_chain,
           control_block_subscribe_through_getter_rejected_spec_28_14_form_4) {
    auto r = runAot(
        std::string(kCounter) +
        hub("  public method OnSub(scoped handler: (double) -> ()): "
            "      owned Subscription "
            "    return this.Changed.Subscribe(handler); "
            "  end OnSub; ") +
        "public class Program "
        "  public static method Main() "
        "    Console.WriteLine(\"ok\"); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kScopedForward));
}

// CONTROL 2 (spec-decided, green on both trees) — a BORROW-field hop: "across
// a borrow hop the storing object is externally owned and may outlive the
// chain head". (Its arrow twin is the measured ASan SEGV S1, pinned by #9201.)
BEZEL_TEST(ownership_9274_scoped_forward_owned_chain,
           control_block_add_on_borrow_field_rejected_spec_28_14_form_4) {
    auto r = runAot(
        "module P; "
        "public class Hub "
        "  private _src: EventSource<double>; "
        "  public constructor(src: EventSource<double>) "
        "    _src := src; "
        "  end constructor; "
        "  public method On(scoped handler: (double) -> ()) "
        "    _src.Add(handler); "
        "  end On; "
        "end Hub; "
        "public class Program "
        "  public static method Main() "
        "    Console.WriteLine(\"ok\"); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kScopedForward));
}
