// Regression tests for issue #9201 — §28.14 *Scoped callback parameters:
// storage-bounded capture* (v0.359) at an EXPRESSION-BODIED method (`=> E`).
//
// THE DEFECT.  §28.14's `scoped`-parameter contract is TWO-SIDED, and its
// callee side binds EVERY USER-AUTHORED CALLEE:
//
//   "**The callee may store the callback**, but only in `scoped` fields **of
//   the bounding object itself** (below), or forward it to another `scoped`
//   parameter whose bounding object is the same object; it may invoke it. It
//   may not return it, move it into non-`scoped` storage, or store it in any
//   other object. … **These obligations are compiler-verified for
//   user-authored callees.**" (§28.14, v0.359)
//
// A BLOCK body runs them: `MethodOwnership::run` runs the §28.14
// callee-obligation walk (`scopedCalleeScan` → `scopedCalleeWalkStmt`), whose
// `return E;` arm reports the RETURN sink and whose CallExpr arm reports the
// FORWARD sink.  An EXPRESSION body (`=> E`) ran NEITHER — the
// expression-body driver (`MethodOwnership::runExprBodyOwningReturnCheck`)
// never reached the §28.14 callee-obligation walk — so the method's OWN
// `scoped` parameter could be forwarded off `this`, stored anywhere else, or
// RETURNED with no diagnostic at all: a SILENT ACCEPTANCE.
//
// MEASURED on r30-c1 (d2482278b, the base of this lane), issue #9201's own
// program compiles with ZERO diagnostics, while its block-bodied twin is
// rejected with the three §28.14 diagnostics asserted below.
//
// THE REPRO (issue #9201, verbatim):
//
//   module M;
//   import System;
//   public class Sink
//     public constructor() end constructor;
//     public method Take(scoped cb: () -> int): int return 0; end Take;
//   end Sink;
//   public class Fl
//     private _l: Array<() -> int> := [];
//     public constructor() end constructor;
//     public method F(scoped q: () -> int): int => (new Sink()).Take(q);
//     public method S(scoped q: () -> int): int => this._l.Add(q);
//     public method R(scoped q: () -> int): () -> int => q;
//   end Fl;
//
// ROWS.  One reject row per sink the `=> E` body silently accepted, each
// asserting the BLOCK twin's own diagnostic text, plus the STATIC-method
// sibling, plus the scoped-FIELD read-out twin, plus ONE admitted control —
// the legal arrow body that forwards `q` to a `scoped` parameter of `this`,
// which §28.14 admits and which must stay admitted.
//
// ORACLE.  `runAot`.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// §28.14's forward diagnostic, verbatim — the text the BLOCK-bodied twin
// gets on the same tree.
const char* kScopedForward =
    "`q` is a `scoped` parameter — it may be forwarded only to another "
    "`scoped` parameter of a call on the same bounding object (`this`) "
    "(§28.14, v0.175)";

// §28.14's return diagnostic, verbatim.
const char* kScopedReturned =
    "`q` is a `scoped` parameter — it may only be invoked, stored into a "
    "`scoped` field of `this`, or forwarded to a `scoped` parameter of a call "
    "on `this`; it may not be returned (§28.14, v0.175)";

// §28.14's scoped-CONSTRUCTION return diagnostic, verbatim — the
// class/interface-typed `scoped` parameter's twin of the above.
const char* kScopedBorrowReturned =
    "`s` is a `scoped` borrow parameter — it may only be stored into a plain "
    "field of the bounding object `this` itself, or forwarded to a `scoped` "
    "parameter of a call on `this`; it may not be returned (§28.14, v0.180)";

// §28.14's scoped-FIELD read-out diagnostic, verbatim.
const char* kScopedFieldReturned =
    "`_cb` is a `scoped` field — it may be invoked, overwritten from a "
    "`scoped` parameter, or cleared to null, but never read out as an "
    "escaping value; it may not be returned (§28.14, v0.175)";

void expectReject(const JitResult& r, const char* diagnostic) {
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, diagnostic));
}

}  // namespace

// ---- FORWARD OFF `this`: "forward it to another `scoped` parameter whose
// ---- bounding object is the same object" ----------------------------------

// REJECT — issue #9201's own repro, method `F`: the expression-bodied
// method forwards its own `scoped` parameter into ANOTHER object's `scoped`
// parameter.  RED on the base tree (the program compiles there); the
// diagnostic is the BLOCK twin's.
BEZEL_TEST(expr_body_scoped_param_9201,
           expression_body_forwards_scoped_param_off_this_rejected_spec_28_14) {
    auto r = runAot(
        "module M;\n"
        "import System;\n"
        "public class Sink\n"
        "  public constructor() end constructor;\n"
        "  public method Take(scoped cb: () -> int): int return 0; end Take;\n"
        "end Sink;\n"
        "public class Fl\n"
        "  public constructor() end constructor;\n"
        "  public method F(scoped q: () -> int): int => (new Sink()).Take(q);\n"
        "end Fl;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(\"ok\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    expectReject(r, kScopedForward);
}

// ---- STORE INTO A COLLECTION: "store it in any other object" ---------------

// REJECT — issue #9201's method `S`: the expression-bodied method stores its
// own `scoped` parameter into a collection reachable beyond the call.  RED on
// the base tree; the diagnostic is the BLOCK twin's.
BEZEL_TEST(expr_body_scoped_param_9201,
           expression_body_stores_scoped_param_into_collection_rejected_spec_28_14) {
    auto r = runAot(
        "module M;\n"
        "import System;\n"
        "public class Fl\n"
        "  private _l: Array<() -> int> := [];\n"
        "  public constructor() end constructor;\n"
        "  public method S(scoped q: () -> int): int => this._l.Add(q);\n"
        "end Fl;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(\"ok\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    expectReject(r, kScopedForward);
}

// ---- RETURN: "It may not return it" ----------------------------------------

// REJECT — issue #9201's method `R`: the expression-bodied method RETURNS its
// own `scoped` parameter.  RED on the base tree; the diagnostic is the BLOCK
// twin's.
BEZEL_TEST(expr_body_scoped_param_9201,
           expression_body_returns_scoped_param_rejected_spec_28_14) {
    auto r = runAot(
        "module M;\n"
        "import System;\n"
        "public class Fl\n"
        "  public constructor() end constructor;\n"
        "  public method R(scoped q: () -> int): () -> int => q;\n"
        "end Fl;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(\"ok\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    expectReject(r, kScopedReturned);
}

// ---- SCOPED CONSTRUCTION (class/interface-typed `scoped` parameter) ---------

// REJECT — the §28.14 *scoped construction* twin: a CLASS-typed `scoped`
// parameter returned from an expression-bodied STATIC method.  §28.14
// (v0.180): "It may not return it or store it anywhere else."  RED on the
// base tree; the diagnostic is the BLOCK twin's.
BEZEL_TEST(expr_body_scoped_param_9201,
           static_expression_body_returns_scoped_borrow_param_rejected_spec_28_14) {
    auto r = runAot(
        "module M;\n"
        "import System;\n"
        "public class Sink\n"
        "  public constructor() end constructor;\n"
        "end Sink;\n"
        "public class Fl\n"
        "  public constructor() end constructor;\n"
        "  public static method S(scoped s: Sink): owned Sink => s;\n"
        "end Fl;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(\"ok\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    expectReject(r, kScopedBorrowReturned);
}

// ---- THE STATIC-METHOD SIBLING OF THE FORWARD SINK -------------------------

// REJECT — the same forward sink on a STATIC method: a static method has no
// `this`, so NOTHING is the same bounding object — every forward of its own
// `scoped` parameter is off-object.  (§28.14, v0.359: "a `scoped` parameter
// on a static method is admitted only when the method's return is `owned`",
// so the declaration itself is admitted and the OBLIGATION is what bites.)
// RED on the base tree; the diagnostic is the BLOCK twin's.
BEZEL_TEST(expr_body_scoped_param_9201,
           static_expression_body_forwards_scoped_param_off_object_rejected_spec_28_14) {
    auto r = runAot(
        "module M;\n"
        "import System;\n"
        "public class Sink\n"
        "  public constructor() end constructor;\n"
        "  public method Make(scoped cb: () -> int): owned Sink "
        "return new Sink(); end Make;\n"
        "end Sink;\n"
        "public class Fl\n"
        "  public constructor() end constructor;\n"
        "  public static method S(scoped q: () -> int): owned Sink "
        "=> new Sink().Make(q);\n"
        "end Fl;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(\"ok\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    expectReject(r, kScopedForward);
}

// ---- THE SCOPED-FIELD READ-OUT TWIN -----------------------------------------

// REJECT — §28.14's `scoped` FIELD obligation ("never read out as an
// escaping value") in an expression body.  The block twin is rejected with
// the SAME text; RED on the base tree.
BEZEL_TEST(expr_body_scoped_param_9201,
           expression_body_reads_out_scoped_field_rejected_spec_28_14) {
    auto r = runAot(
        "module M;\n"
        "import System;\n"
        "public class Fl\n"
        "  private scoped _cb: (() -> int)?;\n"
        "  public constructor() end constructor;\n"
        "  public method M(): () -> int? => this._cb;\n"
        "end Fl;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(\"ok\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    expectReject(r, kScopedFieldReturned);
}

// ---- THE ADMITTED FORWARD — the control that must STAY admitted ------------

// ACCEPT — §28.14 admits the forward "to another `scoped` parameter whose
// bounding object is the same object": `this.Take(q)` forwards the method's
// own `scoped` parameter into ANOTHER `scoped` parameter of a call on `this`.
// This row is GREEN on the base tree — it is the suite's BOUNDARY CONTROL,
// and it is red under a plausible wrong fix: one that rejects any use of a
// `scoped` parameter in an expression body (the over-broad reading of
// "scoped parameters may not be returned"), or one that resolves the
// bounding object to the RECEIVER rather than to `this` and so rejects the
// admitted forward.  The same case, with §28.14's deciding sentence, is in
// the lane's `corpus-candidates/`.
BEZEL_TEST(expr_body_scoped_param_9201,
           expression_body_forwards_scoped_param_to_this_scoped_param_accepted_spec_28_14) {
    auto r = runAot(
        "module M;\n"
        "import System;\n"
        "public class Fl\n"
        "  private scoped _cb: (() -> int)?;\n"
        "  public constructor() end constructor;\n"
        "  public method Forward(scoped q: () -> int): int => this.Take(q);\n"
        "  public method Take(scoped cb: () -> int): int\n"
        "    this._cb := cb;\n"
        "    return 0;\n"
        "  end Take;\n"
        "end Fl;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(\"ok\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---- THE ARROW-ONLY USE-AFTER-FREES (ruling r43-rule-9201-9056, S1/S2) ------
//
// Two arrow-bodied forwards the base tree ACCEPTED resolve no `this` bound
// under §28.14's forms, and both were MEASURED as ASan SEGVs on the base tree
// (`--checked-disposal --sanitize=address`): the caller's capture is admitted
// against a bound the inner registration does not respect. Each is now
// rejected with the forward diagnostic its block twin gets.

// S1 — REJECT. A BORROW-field hop (`_src.Add(handler)`, `_src` a non-owned
// field): §28.14 form 4, "A chain containing a non-owned (borrow) field hop
// … resolves no bounding object … across a borrow hop the storing object is
// externally owned and may outlive the chain head". Base: accepted, then
// `AddressSanitizer: SEGV … in bz$P.Counter2$Hit` at the `src.Raise(2.0)`
// after the `do` block that held `c` and `hub` closed.
BEZEL_TEST(expr_body_scoped_param_9201,
           expression_body_forward_to_borrow_field_add_rejected_spec_28_14_s1) {
    auto r = runAot(
        "module P;\n"
        "public class Counter2\n"
        "  private _n: int := 0;\n"
        "  public method Hit(t: double) => _n := _n + 1;\n"
        "  public property N: int => _n;\n"
        "end Counter2;\n"
        "public class Hub\n"
        "  private _src: EventSource<double>;\n"
        "  public constructor(src: EventSource<double>)\n"
        "    _src := src;\n"
        "  end constructor;\n"
        "  public method On(scoped handler: (double) -> ()) => _src.Add(handler);\n"
        "end Hub;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    using var src := new EventSource<double>();\n"
        "    do\n"
        "      using var c := new Counter2();\n"
        "      using var hub := new Hub(src);\n"
        "      hub.On(lambda(t) => c.Hit(t));\n"
        "      src.Raise(1.0);\n"
        "      Console.WriteLine(c.N);\n"
        "    end;\n"
        "    src.Raise(2.0);\n"
        "    Console.WriteLine(\"after\");\n"
        "  end Main;\n"
        "end Program;\n");
    expectReject(r,
                 "`handler` is a `scoped` parameter — it may be forwarded "
                 "only to another `scoped` parameter of a call on the same "
                 "bounding object (`this`) (§28.14, v0.175)");
}

// S2 — REJECT. A PARAMETER receiver (`=> tok.Register(cb)`): form 4 needs a
// chain "rooted at `this` or a local binding", so form 5 resolves the
// parameter `tok`, and "a parameter … admits no capture-bearing arguments"
// (§28.14, v0.359). Base: accepted, then `AddressSanitizer: SEGV … in
// bz$P.Box$Bump` at the `cts.Cancel()` after the `do` block that held `box`.
BEZEL_TEST(expr_body_scoped_param_9201,
           expression_body_forward_to_parameter_token_register_rejected_spec_28_14_s2) {
    auto r = runAot(
        "module P;\n"
        "import System.Threading;\n"
        "public class Box\n"
        "  private _v: int := 0;\n"
        "  public method Bump() => _v := _v + 1;\n"
        "  public property V: int => _v;\n"
        "end Box;\n"
        "public class Reg\n"
        "  public method OnCancel(tok: CancellationToken, scoped cb: () -> ()): "
        "owned Subscription => tok.Register(cb);\n"
        "end Reg;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    using var cts := new CancellationTokenSource();\n"
        "    var tok := cts.Token;\n"
        "    var keep: Subscription? := null;\n"
        "    do\n"
        "      using var box := new Box();\n"
        "      using var r := new Reg();\n"
        "      keep := r.OnCancel(tok, lambda() => box.Bump());\n"
        "    end;\n"
        "    cts.Cancel();\n"
        "    Console.WriteLine(\"after\");\n"
        "    keep?.Dispose();\n"
        "  end Main;\n"
        "end Program;\n");
    expectReject(r,
                 "`cb` is a `scoped` parameter — it may be forwarded only to "
                 "another `scoped` parameter of a call on the same bounding "
                 "object (`this`) (§28.14, v0.175)");
}
