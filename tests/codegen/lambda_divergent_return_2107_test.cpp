// Behavior tests for the §27.3 divergent-return rule and its suppression under
// a return-type ascription / target type — spec §27.3, v0.226 (impl #2107).
//
// §27.3: in a context with no target type, a value-returning block-bodied
// lambda's return type is inferred only when every value `return` expression
// has the SAME type; divergent returns are a compile error (the reject side is
// pinned in tests/lit/lambda_divergent_return_reject_2107.bzl). These ACCEPT
// cases pin the complementary behavior #2107 must NOT regress: consistent
// returns infer cleanly, and divergent returns UNDER a return-type ascription
// (the v0.217 on-site remedy) are admitted.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// §27.3 v0.226 (#2107): a no-target block body whose value returns are all the
// SAME type infers cleanly (first return's type) and runs — the divergent
// error does not fire. 7 + 35 = 42.
BEZEL_TEST(lambda_divergent_2107, spec_27_3_consistent_returns_no_target) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "var pick := lambda(b: bool) => do "
        "  if b then return 7; end; "
        "  return 35; "
        "end; "
        "return pick(true) + pick(false); "
        "end Main; end C;"), 42);
}

// §27.3 v0.226 (#2107): a no-target block body may nest its first `return`
// arbitrarily deep (here inside a `while`) — the recursive collector finds it
// instead of silently falling back to `int`. Pre-#2107 the flat scan missed
// this and typed the lambda `(int) -> int` by the `int` fallback; the body's
// real `string` return then miscompiles. With #2107 the inferred return is
// `string` and the lambda runs.
BEZEL_TEST(lambda_divergent_2107, spec_27_3_deep_first_return_infers_real_type) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "var first := lambda(s: string) => do "
        "  var i := 0; "
        "  while i < s.Length do "
        "    if s[i] = ' ' then return s.Substring(0, i); end; "
        "    i := i + 1; "
        "  end; "
        "  return s; "
        "end; "
        "Console.WriteLine(first(\"hello world\")); "
        "return 0; "
        "end Main; end C;"), std::string("hello"));
}

// §27.3 v0.226 (#2107): a return-type ascription is the on-site remedy for
// divergent returns — `: string?` admits the null-producing arm (§20) and each
// `return` checks against it under §5.1. The block body compiles and runs.
BEZEL_TEST(lambda_divergent_2107, spec_27_3_divergent_under_ascription) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "var classify := lambda(score: int): string? => do "
        "  if score < 0 then return null; end; "
        "  if score >= 90 then return \"A\"; end; "
        "  return \"pass\"; "
        "end; "
        "var a := classify(95); "
        "var b := classify(-1); "
        "if a is null then return 1; end; "
        "if b is not null then return 2; end; "
        "if a = \"A\" then return 0; end; "
        "return 3; "
        "end Main; end C;"), 0);
}

// §27.3 v0.226 (#2107): a block-bodied lambda RETURNED from a method whose
// declared return type is a function type has that type as its TARGET — the
// divergent-return inference error must NOT fire (the returns are checked
// against the target under §5.1, never unified by inference). This narrows the
// prior #2107 fix, which threaded the target only from a typed-local
// initializer and a call argument and so wrongly rejected this shape. The body
// returns Dog and Cat (sibling types — divergent without a target, §5.1-
// compatible with the Animal target), and g(…) dispatches to Dog.Kind (= 1).
// Fails without the ReturnStmt target-threading (divergent error); passes with
// it.
//
// ⚠ The Dog/Cat/bool bindings are the LAMBDA's OWN parameters, not the
// enclosing method's. They used to be `Make(d: Dog, c: Cat, b: bool)` with the
// lambda capturing all three; §28.3's closure bullet (v0.298, restated v0.307,
// implemented by `ac2c7c6e` / fix #4709) rejects a returned closure whose
// source set is non-empty, and a captured plain borrow parameter makes it
// non-empty — so that spelling is now a REJECT on a rule this file does not
// test. Moving the bindings INTO the lambda keeps this row's axis exactly (the
// method's `(…) -> Animal` return type is still the block-bodied lambda's
// target, and its two `return`s are still the divergent Dog/Cat pair) while
// the closure captures nothing. Measured: with the bindings inside, the
// identical body with NO target still reports "a block-bodied lambda with no
// target type has divergent `return` types … (§27.3)", so the suppression this
// row pins is still the thing under test. The capture rule itself is pinned in
// tests/ownership/plain_borrow_param_closure_escape_4709_test.cpp.
// // per §27.3 v0.226, §28.3 v0.307
BEZEL_TEST(lambda_divergent_2107, spec_27_3_divergent_under_method_return_target) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Animal public virtual method Kind(): int return 0; end Kind; end Animal; "
        "class Dog : Animal public override method Kind(): int return 1; end Kind; end Dog; "
        "class Cat : Animal public override method Kind(): int return 2; end Kind; end Cat; "
        "class C "
        "  public static method Make(): (Dog, Cat, bool) -> Animal "
        "    return lambda(d, c, b) => do "
        "      if b then return d; end; "
        "      return c; "
        "    end; "
        "  end Make; "
        "  public static method Main(): int "
        "    using var d := new Dog(); "
        "    using var c := new Cat(); "
        "    var g := Make(); "
        "    return g(d, c, true).Kind(); "
        "  end Main; "
        "end C;"), 1);
}

// §27.3 v0.226 (#2107): the same narrowing for a DEFERRED reassignment to a
// function-typed local (`var f: (…) -> R; … f := lambda …`) — the local's
// declared type is the target, so the divergent-return error is suppressed.
// Mirrors the ascription test above, but the target arrives through the
// AssignmentStmt position instead of an inline ascription or initializer.
// Fails without the AssignmentStmt target-threading (divergent error on null vs
// string); passes with it (== 0).
BEZEL_TEST(lambda_divergent_2107, spec_27_3_divergent_under_reassign_target) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var classify: (int) -> string?; "
        "  classify := lambda(score: int) => do "
        "    if score < 0 then return null; end; "
        "    if score >= 90 then return \"A\"; end; "
        "    return \"pass\"; "
        "  end; "
        "  var a := classify(95); "
        "  var b := classify(-1); "
        "  if a is null then return 1; end; "
        "  if b is not null then return 2; end; "
        "  if a = \"A\" then return 0; end; "
        "  return 3; "
        "end Main; end C;"), 0);
}

// §27.3 / §28.9 (#2515, ASan regression): the same divergent-under-method-
// return-target shape as the cell above, but driven through runAotChecked so
// generated-code AddressSanitizer is forced ON for this cell regardless of the
// suite-wide flag.  Pre-#2515 the enclosing method's return-type node (`() ->
// Animal`, a Function kind) LEAKED into the block-bodied lambda's ReturnStmt
// lowering (currentReturnTypeNode_ was not saved/restored in lowerLambda), so
// emitReturn's closure-return-retain branch fired emitClosureRetain on the
// returned CLASS instance (a captured Dog) — writing 8 bytes at offset 16 of a
// 16-byte object, a heap-buffer-overflow that surfaced as a double-free under
// ASan.  The fix saves/restores currentReturnTypeNode_ in lowerLambda and nulls
// it for the lambda body (a lambda's `return` is an internal transfer to the
// trampoline, not a method-contract return that mints +1 against a declared
// return-type node).  Fails (sanitizer double-free → r.ran == false) without
// the fix; passes (ASan/LSan-clean AND the invocation's Kind() == 1) with it.
//
// ⚠ Same re-spelling as the cell above (#4709/#4757, §28.3 v0.307): the
// Dog/Cat/bool bindings moved from the enclosing method's parameter list into
// the LAMBDA's, so the returned closure captures nothing. The #2515 mechanism
// is untouched by that move — it is `currentReturnTypeNode_` (the enclosing
// method's `(…) -> Animal` Function-kind node) leaking into the block-bodied
// lambda's ReturnStmt lowering, so emitReturn's closure-return-retain branch
// fires emitClosureRetain on the returned CLASS instance. The body still
// returns a class instance (Dog / Cat) from a block-bodied lambda that is
// itself returned through a Function-kind declared return type, which is
// exactly the input that branch mis-classifies.
BEZEL_TEST(lambda_divergent_2107, spec_27_3_divergent_owned_capture_asan_2515) {
    auto r = runAotChecked(
        "module M; "
        "class Animal public virtual method Kind(): int return 0; end Kind; end Animal; "
        "class Dog : Animal public override method Kind(): int return 1; end Kind; end Dog; "
        "class Cat : Animal public override method Kind(): int return 2; end Kind; end Cat; "
        "class C "
        "  public static method Make(): (Dog, Cat, bool) -> Animal "
        "    return lambda(d, c, b) => do "
        "      if b then return d; end; "
        "      return c; "
        "    end; "
        "  end Make; "
        "  public static method Main(): int "
        "    using var d := new Dog(); "
        "    using var c := new Cat(); "
        "    var g := Make(); "
        "    return g(d, c, true).Kind(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);        // ASan/LSan-clean (no double-free, no leak)
    BEZEL_EXPECT_EQ(r.exitCode, 1);  // g(d, c, true).Kind() == Dog.Kind == 1
}

