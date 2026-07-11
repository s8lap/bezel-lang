// ConstFold robustness — folding must not deref a null Unary/Binary operand
// (#1631, §17.14 constant expressions).
//
// `foldIntConst` (src/sema/ConstFold.cpp) recurses into a UnaryExpr's
// `operand` and a BinaryExpr's `left`/`right` unconditionally:
//   auto v = foldIntConst(*u->operand, resolveIdent, depth + 1);
//   auto l = foldIntConst(*b->left,  resolveIdent, depth + 1);
//   auto r = foldIntConst(*b->right, resolveIdent, depth + 1);
// A parser error-recovery AST can leave any of these `unique_ptr<Expr>`
// fields null — e.g. `public const K: int := 1 + ;` recovers to a
// `BinaryExpr` whose `right` is null (the same shape #1622 pinned at the
// driver level); `public const K: int := * 1;` recovers to a `BinaryExpr`
// whose `left` is null (the first token, `*`, is not a valid expression
// start, so `parseUnaryExpression` returns null without consuming it, and
// the binary-operator loop then reads the still-current `*` as the
// operator); `public const K: int := -;` recovers to a `UnaryExpr` whose
// `operand` is null. Dereferencing any of these aborts the process
// (`unique_ptr::operator*()`), exactly as #1622 documented for the sibling
// unconditional-deref sites in TypeResolution.
//
// #1622 fixed the *driver* by gating sema on a clean parse
// (`!diagnostics.hasErrors()`) before it runs — which masks this class of
// call today when compilation goes through `Driver::compileFile` /
// `compileObject`. But `ConstFold` itself has no such guard, so any caller
// that reaches `resolveTypes` without going through that gate — such as
// this test's `parseSemaAll` helper below, which mirrors
// tests/sema/const_type_test.cpp and unconditionally calls
// `sema::resolveTypes` regardless of parse diagnostics — still drives the
// unguarded deref. This test pins ConstFold's OWN guard (independent of the
// driver gate) per the issue's defense-in-depth fix direction: an absent
// operand is trivially not one of §17.14's constant-expression forms, so
// the fold must decline (fall through to `std::nullopt`) rather than crash.
//
// Primary regression signal: these calls RETURN at all — before the fix
// each aborted the test process on the null-`unique_ptr` deref.

#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
    }
    return r;
}

} // namespace

// ── #1631: null-operand shapes must not crash the fold ───────────────────────

// BinaryExpr with a null RIGHT operand (`1 + ;` — the #1622 driver-level
// reproducer, replayed here past the driver's gate).
BEZEL_TEST(sema_const_fold_1631, binary_null_right_operand_no_crash) {
    auto r = parseSemaAll(
        "module M; class C public const K: int := 1 + ; end C;");
    // The malformed initializer is still a genuine parse error — just not
    // one that aborts the process while checkTypeConsts folds it.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// BinaryExpr with a null LEFT operand (`* 1` — the leading `*` is not a
// valid expression start; parseUnaryExpression returns null without
// consuming it, and the binary-operator loop then reads that unconsumed `*`
// as the operator, producing `BinaryExpr{op:"*", left:null, right:1}`).
BEZEL_TEST(sema_const_fold_1631, binary_null_left_operand_no_crash) {
    auto r = parseSemaAll(
        "module M; class C public const K: int := * 1; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// UnaryExpr with a null operand (`-;`).
BEZEL_TEST(sema_const_fold_1631, unary_null_operand_no_crash) {
    auto r = parseSemaAll(
        "module M; class C public const K: int := -; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Nested shape: the null operand is not the immediate child of the folded
// root but reached after one level of genuine recursion (`(1 + ;) * 2`
// parenthesizes the malformed `1 + ;` sub-expression so it survives as a
// BinaryExpr operand of the outer `*`), exercising the recursive call sites
// rather than only the top-level one.
BEZEL_TEST(sema_const_fold_1631, nested_null_operand_no_crash) {
    auto r = parseSemaAll(
        "module M; class C public const K: int := (1 + ) * 2; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ── Control: genuine constant folding still works ─────────────────────────────

// §17.14: binary arithmetic on constant operands folds — an in-range result
// is accepted with no diagnostic.
BEZEL_TEST(sema_const_fold_1631, genuine_binary_fold_still_accepted) {
    auto r = parseSemaAll(
        "module M; class C public const K: int := 1 + 2; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §17.14 / §6.1 (Appendix C.11, #958): a genuinely-constant fold whose value
// does not fit the declared type is still correctly range-checked (the guard
// must only decline null operands, not blunt the real fold-and-check path).
BEZEL_TEST(sema_const_fold_1631, genuine_binary_fold_overflow_still_rejected) {
    auto r = parseSemaAll(
        "module M; class C public const K: byte := 200 + 100; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("does not fit its declared type") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// §17.14: unary `-` on a constant operand still folds correctly.
BEZEL_TEST(sema_const_fold_1631, genuine_unary_fold_still_accepted) {
    auto r = parseSemaAll(
        "module M; class C public const K: int := -5; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
