// Spec-derived regression test for issue #7450 — the §28.3.8 sink rule
// (v0.359) reached from an EXPRESSION-BODIED member.
//
// §28.3.8 (v0.348's example, unchanged in v0.359) reads:
//
//   *Example.* The closure bullet: a closure that captures a borrow may be
//   invoked or passed on, but not returned:
//   ```bezel
//   public static method Label(i: Item): () -> string => lambda() => i.Name;
//     // error (sink rule): a return slot announces no source set
//   ```
//
// The example IS the issue's repro, and the spec marks that exact line `error`.
// §28.3.8's sink rule says: "While its source set is non-empty, a function
// value may not be stored in a field, in any collection, in a property or
// indexer, or through an `out` or `ref` parameter, may not be returned from
// the enclosing method …". §22.2 says "When the body is a single expression,
// `=> expression` replaces the block" — so an expression body IS the
// enclosing method's return value, and the sink rule applies to it exactly as
// it does to a block body's `return`.
//
// The gap: the block-bodied twin (`return lambda() => i.Name;`) is rejected by
// the §28.3 closure-bullet plain-parameter capture leg (#4709), but the
// expression body never reached that check — `runExprBodyOwningReturnCheck`
// drove the other `=>`-sibling return checks and not this one. MEASURED on the
// base (b279e8daf): the program below COMPILED with zero diagnostics, and at
// run time it is a heap-use-after-free (ASan: heap-use-after-free, READ of
// size 8 in `lam$0`, freed by `main`) — the returned closure reads the `Item`
// the caller's `using var` disposed at scope end.
//
// The BLOCK-bodied twin is base-GREEN (it is rejected on the base too), so it
// is not a gtest row — it travels to the conformance corpus with the spec
// sentence that decides it (see the producer's corpus-candidates/).
//
// per §28.3.8 (v0.359; example added v0.348), §22.2, §27.5

#include <string>

#include "support/aot_result_asserts.hpp"   // diagnosticMessageContains
#include "support/aot_support.hpp"          // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// The §28.3 closure-bullet diagnostic the block-bodied twin already carries
// (#4709) — the expression body now carries it too.
static const char* kPlainParamCapture =
    "returned closure captures `i`, a plain (borrowing) parameter";

// The same diagnostic over the operator's own parameter name.
static const char* kPlainParamCaptureA =
    "returned closure captures `a`, a plain (borrowing) parameter";

// ===========================================================================
// REJECT — the issue's own repro (the spec's v0.348 example line).
// ===========================================================================

// MEASURED before the fix: rc 0 (compiled clean), and a heap-use-after-free
// at run time.
BEZEL_TEST(expr_body_returned_closure_sink_7450,
           static_method_returning_closure_over_plain_param_rejected_28_3_8) {
    std::string src =
        "module M; "
        "class Item(name: string) "
        "  public property Name: string => name; "
        "end Item; "
        "class Program "
        "  public static method Label(i: Item): () -> string => "
        "lambda() => i.Name; "
        "  public static method Main() "
        "    var f: () -> string := lambda() => \"none\"; "
        "    do "
        "      using var it := new Item(\"x\"); "
        "      f := Label(it); "
        "    end; "
        "    Console.WriteLine(f()); "
        "  end Main; "
        "end Program;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kPlainParamCapture));
    // The sink rule's own reasoning: a function type announces no source set.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a function type announces no source set"));
}

// The INSTANCE-method spelling of the same body — the issue names it, and the
// sink rule is not a static-only rule ("may not be returned from the
// enclosing method").
BEZEL_TEST(expr_body_returned_closure_sink_7450,
           instance_method_returning_closure_over_plain_param_rejected_28_3_8) {
    std::string src =
        "module M; "
        "class Item(name: string) "
        "  public property Name: string => name; "
        "end Item; "
        "class Program "
        "  public method Label(i: Item): () -> string => "
        "lambda() => i.Name; "
        "  public static method Main() "
        "    using var it := new Item(\"x\"); "
        "    var f := new Program().Label(it); "
        "    Console.WriteLine(f()); "
        "  end Main; "
        "end Program;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kPlainParamCapture));
}

// The OPERATOR spelling (§25: an operator is a `public static method
// operator`) — the same expression body on the shared `=>` driver.
BEZEL_TEST(expr_body_returned_closure_sink_7450,
           operator_returning_closure_over_plain_param_rejected_28_3_8) {
    std::string src =
        "module M; "
        "class Item(name: string) "
        "  public property Name: string => name; "
        "  public static method operator +(a: Item, b: Item): () -> string => "
        "lambda() => a.Name; "
        "end Item; "
        "class Program "
        "  public static method Main() "
        "    using var it := new Item(\"x\"); "
        "    var f := it + it; "
        "    Console.WriteLine(f()); "
        "  end Main; "
        "end Program;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kPlainParamCaptureA));
}
