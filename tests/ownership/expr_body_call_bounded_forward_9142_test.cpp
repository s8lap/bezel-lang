// Regression tests for issue #9142 — §28.14 *The call-bounded default*
// (v0.359) at an EXPRESSION-BODIED method (`=> E`).
//
// THE DEFECT.  §28.14's callee-side obligations bind every USER-AUTHORED
// CALLEE: "Within the callee the callback may be invoked, bound to locals,
// and passed on as an argument to another call-bounded parameter … It may not
// be stored in a field of any object …, stored into any collection …,
// returned, captured by a closure that itself escapes the callee …, or
// forwarded into a `scoped` parameter position: the callee cannot see its
// argument's capture set, so it cannot vouch for the §28.14 admission …
// These obligations are compiler-verified for user-authored callees".
//
// A BLOCK body runs them: `MethodOwnership::run` seeds the body's own plain
// function-typed formals as CALL-BOUNDED and then runs `callBoundedScan` +
// `scopedPreScan`.  An EXPRESSION body ran only the RETURN sink — the
// expression-body driver (`MethodOwnership::runExprBodyOwningReturnCheck`)
// never ran the call-bounded walk, so every OTHER sink was invisible on
// `=> E`: the callback was silently accepted where the block-bodied twin is
// rejected.  The body form is not an exception in the spec.
//
// MEASURED on r30-c1 (#9142): the issue's own program compiles with ZERO
// diagnostics, while its block-bodied twin is rejected with §28.14's
// forward diagnostic.
//
// THE REPRO (issue #9142, verbatim):
//
//   module M;
//   import System;
//   public class Sink
//     public constructor() end constructor;
//     public method Take(scoped cb: () -> int): int return 0; end Take;
//   end Sink;
//   public class Fl
//     public constructor() end constructor;
//     public method M(q: () -> int): int => (new Sink()).Take(q);
//   end Fl;
//   public class Program
//     public static method Main(): int
//       Console.WriteLine("ok");
//       return 0;
//     end Main;
//   end Program;
//
// ROWS.  One reject row per sink the `=> E` body silently accepted, each
// asserting the BLOCK twin's own diagnostic text.  ORACLE.  `runAot`.

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
    "parameter `q` is call-bounded and may not be forwarded into a `scoped` "
    "parameter position — the callee cannot vouch for captures it cannot "
    "see; declare `q` itself `scoped` to store it (§28.14, v0.177)";

// §28.14's collection-store diagnostic, verbatim.
const char* kCollectionStore =
    "parameter `q` is call-bounded; declare it `scoped` to store it "
    "(§28.14, v0.177) — a plain function-typed parameter may be invoked, "
    "bound to locals, and passed to another plain function-typed parameter, "
    "but may not be stored into a collection reachable beyond the call";

// §28.14's return diagnostic, verbatim (the lambda's OWN call-bounded
// parameter, returned through the lambda's `=> E` body).
const char* kReturned =
    "parameter `cb` is call-bounded; declare it `scoped` to store it "
    "(§28.14, v0.177) — a plain function-typed parameter may be invoked, "
    "bound to locals, and passed to another plain function-typed parameter, "
    "but may not be returned";

void expectReject(const JitResult& r, const char* diagnostic) {
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, diagnostic));
}

// The issue's program, verbatim.
const char* kIssueRepro =
    "module M;\n"
    "import System;\n"
    "public class Sink\n"
    "  public constructor() end constructor;\n"
    "  public method Take(scoped cb: () -> int): int return 0; end Take;\n"
    "end Sink;\n"
    "public class Fl\n"
    "  public constructor() end constructor;\n"
    "  public method M(q: () -> int): int => (new Sink()).Take(q);\n"
    "end Fl;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    Console.WriteLine(\"ok\");\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

}  // namespace

// ---- SCOPED-FORWARD: "or forwarded into a `scoped` parameter position" -------

// REJECT — #9142's repro: the expression-bodied method forwards its own
// call-bounded parameter into a `scoped` parameter position.  RED on the base
// tree (the program compiles there); the diagnostic is the BLOCK twin's.
BEZEL_TEST(expr_body_call_bounded_9142,
           expression_body_forwards_call_bounded_param_to_scoped_rejected_spec_28_14) {
    auto r = runAot(kIssueRepro);
    expectReject(r, kScopedForward);
}

// ---- COLLECTION STORE: "stored into any collection, a local one included" ---

// REJECT — the collection-store twin of the same gap: the expression-bodied
// method stores its own call-bounded parameter into a collection reachable
// beyond the call.  RED on the base tree; the diagnostic is the BLOCK twin's.
BEZEL_TEST(expr_body_call_bounded_9142,
           expression_body_stores_call_bounded_param_into_collection_rejected_spec_28_14) {
    auto r = runAot(
        "module M;\n"
        "import System;\n"
        "public class Fl\n"
        "  private _l: Array<() -> int> := [];\n"
        "  public constructor() end constructor;\n"
        "  public method M(q: () -> int): int => this._l.Add(q);\n"
        "end Fl;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(\"ok\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    expectReject(r, kCollectionStore);
}

// ---- RETURN: "returned" ---------------------------------------------------------

// REJECT — a LAMBDA in the expression body returns its OWN call-bounded
// parameter through the lambda's `=> E` body (§28.14's return sink, the
// #2343 leg).  RED on the base tree; the diagnostic is the BLOCK twin's.
BEZEL_TEST(expr_body_call_bounded_9142,
           expression_body_returns_lambda_own_call_bounded_param_rejected_spec_28_14) {
    auto r = runAot(
        "module M;\n"
        "import System;\n"
        "public class Fl\n"
        "  public constructor() end constructor;\n"
        "  public method M(q: () -> int): () -> int => "
        "(lambda(cb: () -> int) => cb)(q);\n"
        "end Fl;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(\"ok\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    expectReject(r, kReturned);
}
