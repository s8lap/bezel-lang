// Spec-derived tests for async-readiness 3b — sema typing of the `await`
// expression and the async return-type rules.
//
// Derivation:
//   - §30.2 / §30.3 / §30.5: the result type of `await`.
//       `await Task`          -> unit   (no result value).
//       `await Task<T>`       -> T       (Pattern A; the borrow/ownership at
//                                          the await site is the ownership
//                                          pass's job — 3b types the result).
//       `await OwnedTask<T>`  -> T       (Pattern B; result type is the
//                                          element type T; `owned`-ness is
//                                          tracked by ownership, not TypeRef).
//   - §30.2 / Appendix C.6: `await` appears only inside an `async` method,
//       `async` lambda, or the synchronous prefix of `await for`. `await`
//       outside an async context is a compile error.
//   - §30.1 / §30.6 / Appendix C.6: an `async` method/lambda's declared
//       return type must be `Task` / `Task<T>` / `OwnedTask<T>` (or an
//       async-iterator interface). A non-async return type is a compile error.
//   - §17.1 (precedence list): `await await x` is permitted; typing composes.

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::sema::PrimitiveType;
using bezel::sema::TypeRef;

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// Find the first `await` UnaryExpr anywhere in the program's method bodies
// (block bodies and expression bodies, descending into lambdas), recursively.
const bezel::ast::UnaryExpr* findAwait(const bezel::ast::Expr* e);

const bezel::ast::UnaryExpr* findAwaitInStmts(
    const std::vector<std::unique_ptr<bezel::ast::Stmt>>& stmts);

const bezel::ast::UnaryExpr* findAwait(const bezel::ast::Expr* e) {
    if (!e) return nullptr;
    if (auto* ue = dynamic_cast<const bezel::ast::UnaryExpr*>(e)) {
        if (ue->op == "await") return ue;
        return findAwait(ue->operand.get());
    }
    if (auto* be = dynamic_cast<const bezel::ast::BinaryExpr*>(e)) {
        if (auto* l = findAwait(be->left.get())) return l;
        return findAwait(be->right.get());
    }
    if (auto* ma = dynamic_cast<const bezel::ast::MemberAccessExpr*>(e))
        return findAwait(ma->target.get());
    if (auto* ce = dynamic_cast<const bezel::ast::CallExpr*>(e)) {
        if (auto* c = findAwait(ce->callee.get())) return c;
        for (const auto& a : ce->args)
            if (auto* x = findAwait(a.value.get())) return x;
    }
    if (auto* lam = dynamic_cast<const bezel::ast::LambdaExpr*>(e)) {
        if (auto* x = findAwait(lam->exprBody.get())) return x;
        if (auto* x = findAwaitInStmts(lam->stmts)) return x;
    }
    return nullptr;
}

const bezel::ast::UnaryExpr* findAwaitInStmt(const bezel::ast::Stmt* s) {
    if (!s) return nullptr;
    if (auto* ls = dynamic_cast<const bezel::ast::LocalDecl*>(s))
        return findAwait(ls->initializer.get());
    if (auto* ds = dynamic_cast<const bezel::ast::DiscardStmt*>(s))
        return findAwait(ds->value.get());
    if (auto* rs = dynamic_cast<const bezel::ast::ReturnStmt*>(s))
        return findAwait(rs->value.get());
    if (auto* es = dynamic_cast<const bezel::ast::ExpressionStmt*>(s))
        return findAwait(es->expr.get());
    if (auto* as = dynamic_cast<const bezel::ast::AssignmentStmt*>(s))
        return findAwait(as->value.get());
    if (auto* db = dynamic_cast<const bezel::ast::DoEndBlock*>(s))
        return findAwaitInStmts(db->body);
    return nullptr;
}

const bezel::ast::UnaryExpr* findAwaitInStmts(
    const std::vector<std::unique_ptr<bezel::ast::Stmt>>& stmts) {
    for (const auto& s : stmts)
        if (auto* a = findAwaitInStmt(s.get())) return a;
    return nullptr;
}

const bezel::ast::UnaryExpr* firstAwait(const bezel::test::SemaResult& r) {
    if (!r.sourceFile) return nullptr;
    for (const auto& topD : r.sourceFile->topDecls) {
        const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(topD.get());
        if (!td) continue;
        for (const auto& m : td->members) {
            const auto* md =
                dynamic_cast<const bezel::ast::MethodDecl*>(m.get());
            if (!md) continue;
            if (md->bodyKind == bezel::ast::MethodBodyKind::Block) {
                if (auto* a = findAwaitInStmts(md->stmts)) return a;
            } else if (md->bodyKind ==
                       bezel::ast::MethodBodyKind::Expression) {
                if (auto* a = findAwait(md->exprBody.get())) return a;
            }
        }
    }
    return nullptr;
}

bool hasAwaitContextError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("`await`") != std::string::npos &&
            d.message.find("async") != std::string::npos)
            return true;
    return false;
}

bool hasAsyncReturnTypeError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("async") != std::string::npos &&
            d.message.find("return type") != std::string::npos)
            return true;
    return false;
}

} // namespace

// §30.3 / §30.5: `await Task<int>` types as `int`.
BEZEL_TEST(sema_await, await_task_int_types_as_int) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public async method F(t: Task<int>): Task "
        "    var n := await t; return; "
        "  end F; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* aw = firstAwait(r);
    BEZEL_EXPECT_TRUE(aw != nullptr);
    if (aw) {
        auto* ty = r.ctx->exprType(*aw);
        BEZEL_EXPECT_TRUE(ty && ty->kind == TypeRef::Kind::Primitive &&
                          ty->primitive == PrimitiveType::Int);
    }
}

// §30.5 Pattern B: `await OwnedTask<SomeClass>` types as the element type
// SomeClass (the `owned`-ness is the ownership pass's concern; 3b types T).
BEZEL_TEST(sema_await, await_ownedtask_class_types_as_element) {
    auto r = parseSemaAll(
        "module M; class R end R; class C "
        "  public async method F(t: OwnedTask<R>): Task "
        "    using var x := await t; return; "
        "  end F; "
        "end C;");
    auto* aw = firstAwait(r);
    BEZEL_EXPECT_TRUE(aw != nullptr);
    if (aw) {
        auto* ty = r.ctx->exprType(*aw);
        BEZEL_EXPECT_TRUE(ty && ty->kind == TypeRef::Kind::Named);
        // The element type is `R` (the user class).
        BEZEL_EXPECT_TRUE(ty && ty->astNode && ty->astNode->name &&
                          !ty->astNode->name->parts.empty() &&
                          ty->astNode->name->parts.front() == "R");
    }
}

// §30.2 / §30.3: `await Task` (no result value) types as unit.
BEZEL_TEST(sema_await, await_task_void_types_as_unit) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public async method F(t: Task): Task "
        "    await t; return; "
        "  end F; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* aw = firstAwait(r);
    BEZEL_EXPECT_TRUE(aw != nullptr);
    if (aw) {
        auto* ty = r.ctx->exprType(*aw);
        BEZEL_EXPECT_TRUE(ty && ty->kind == TypeRef::Kind::Unit);
    }
}

// §17.1 precedence: `await await x` is permitted; typing composes.
// `x: Task<Task<int>>` -> inner await -> Task<int> -> outer await -> int.
BEZEL_TEST(sema_await, await_of_await_composes) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public async method F(t: Task<Task<int>>): Task "
        "    var n := await await t; return; "
        "  end F; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* outer = firstAwait(r);
    BEZEL_EXPECT_TRUE(outer != nullptr);
    if (outer) {
        auto* ty = r.ctx->exprType(*outer);
        BEZEL_EXPECT_TRUE(ty && ty->kind == TypeRef::Kind::Primitive &&
                          ty->primitive == PrimitiveType::Int);
        // The inner await types as `Task<int>` (a Named TypeRef).
        auto* inner =
            dynamic_cast<const bezel::ast::UnaryExpr*>(outer->operand.get());
        BEZEL_EXPECT_TRUE(inner && inner->op == "await");
        if (inner) {
            auto* it = r.ctx->exprType(*inner);
            BEZEL_EXPECT_TRUE(it && it->kind == TypeRef::Kind::Named);
        }
    }
}

// §30.2 / Appendix C.6: `await` outside an async context is a compile error.
BEZEL_TEST(sema_await, await_outside_async_diagnosed) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F(t: Task<int>): int "
        "    var n := await t; return n; "
        "  end F; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAwaitContextError(r));
}

// §30.2: `await` inside an `async` lambda is permitted (not diagnosed).
BEZEL_TEST(sema_await, await_inside_async_lambda_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F(t: Task<int>) "
        "    var f := async () => await t; "
        "    _ := f; "
        "  end F; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasAwaitContextError(r));
}

// §30.2: `await` inside a NON-async lambda (even if the lambda sits in an
// async method) is a compile error — the lambda is its own async boundary.
BEZEL_TEST(sema_await, await_inside_sync_lambda_diagnosed) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public async method F(t: Task<int>): Task "
        "    var f := () => await t; "
        "    _ := f; return; "
        "  end F; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasAwaitContextError(r));
}

// §30.1 / Appendix C.6: an `async` method with a non-async return type is a
// compile error.
BEZEL_TEST(sema_await, async_method_non_async_return_diagnosed) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public async method F(): int return 1; end F; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAsyncReturnTypeError(r));
}

// §30.1: an `async` method with NO declared return type is a compile error
// (the implicit unit return is not an admitted async return type).
BEZEL_TEST(sema_await, async_method_missing_return_diagnosed) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public async method F() return; end F; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAsyncReturnTypeError(r));
}

// §30.1: `Task` / `Task<T>` / `OwnedTask<T>` are all admitted async return
// types (no return-type error).
BEZEL_TEST(sema_await, async_method_task_returns_ok) {
    auto r = parseSemaAll(
        "module M; class R end R; class C "
        "  public async method A(): Task return; end A; "
        "  public async method B(): Task<int> return 1; end B; "
        "  public async method D(t: OwnedTask<R>): OwnedTask<R> "
        "    return await t; "
        "  end D; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasAsyncReturnTypeError(r));
}

// §30.7: an async-iterator interface return type is admitted (no return-type
// error). The async-iterator-specific `await for` result typing is deferred;
// here we verify only that the declared return type is accepted.
BEZEL_TEST(sema_await, async_method_iterator_return_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public async method G(): IAsyncEnumerable<int> "
        "    yield return 1; "
        "  end G; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasAsyncReturnTypeError(r));
}

// §30.1: a fully-typed async method body that uses the awaited result at its
// correct type compiles. `await t` is `int`, so `n + 1` (int arithmetic)
// type-checks and is returned as `int` in `Task<int>`.
BEZEL_TEST(sema_await, async_body_uses_awaited_result_compiles) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public async method F(t: Task<int>): Task<int> "
        "    var n := await t; "
        "    return n + 1; "
        "  end F; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A non-async method does NOT trip the async-return check (the rule is gated
// on the `async` modifier).
BEZEL_TEST(sema_await, non_async_int_method_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F(): int => 1; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasAsyncReturnTypeError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §30.2 / Appendix C.7 (#444): `await` of a non-Task primitive (e.g. `await 5`)
// is a compile error. The operand type `int` is not Task / Task<T> / OwnedTask<T>.
// This is the general guard that turns any await-of-non-task into a clean diagnostic.
BEZEL_TEST(sema_await_444, await_non_task_int_literal_diagnosed) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public async method F(): Task "
        "    await 5; return; "
        "  end F; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("`await` requires") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// §30.2 / Appendix C.7 (#444): `await` of a bool (the type produced by the
// chained-comparison misparse of `Task.FromResult<int>(42)`, per §A.7 — no
// call-site generic type argument syntax) is a compile error. This is the
// core #444 repro at sema level: the misparse makes `await bool`, which is
// now rejected cleanly instead of crashing codegen with a SIGSEGV.
BEZEL_TEST(sema_await_444, await_bool_literal_diagnosed) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public async method F(): Task "
        "    await true; return; "
        "  end F; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("`await` requires") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// §A.7 / §17.1 (#444): a genuine chained comparison `a < b` produces bool and
// must still compile (not over-rejected). The comparison itself is valid; only
// `await`-ing the result is the error.
BEZEL_TEST(sema_await_444, comparison_bool_result_compiles) {
    // `3 < 5` is a valid bool expression; assigning to a bool local must work.
    auto r = parseSemaAll(
        "module M; class C "
        "  public method F(): bool "
        "    var b := 3 < 5; return b; "
        "  end F; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §30.3 (#444): `await Task<int>` (inference form — the correct idiom matching
// `Task.FromResult(42)`) still works and types the result as `int`. This
// verifies the guard does NOT over-reject the valid Task typed form.
BEZEL_TEST(sema_await_444, await_task_int_inference_form_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static async method Producer(): Task<int> "
        "    return 42; "
        "  end Producer; "
        "  public async method F(): Task<int> "
        "    var n := await Producer(); "
        "    return n; "
        "  end F; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // The await must type as int.
    auto* aw = firstAwait(r);
    BEZEL_EXPECT_TRUE(aw != nullptr);
    if (aw) {
        auto* ty = r.ctx->exprType(*aw);
        BEZEL_EXPECT_TRUE(ty && ty->kind == TypeRef::Kind::Primitive &&
                          ty->primitive == PrimitiveType::Int);
    }
}
