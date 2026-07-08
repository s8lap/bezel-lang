// Spec-derived tests for nullability flow-narrowing and §7.4 enforcement of
// member access on nullable values (issue #418).
//
// §7.4 (Member access on nullable values): member access, method calls, and
// indexing on a nullable value are compile errors unless the compiler has
// narrowed it to non-null. Narrowing applies inside the then-block of
// `if x <> null` (and the matching `is not null` form, which the parser
// desugars to `<> null`), in the else-block of `if x = null` (resp.
// `is null` → `=`), after `x!` (§7.8 null-forgiving), and for `and`-joined
// null tests. Reassignment within the controlled block invalidates the
// narrowing (§7.4 / §12.7). Narrowing is block-scoped.
//
// §7.5 (Null-conditional access `?.`): `x?.M` is the safe form and is legal
// on a nullable value WITHOUT narrowing — it short-circuits to null.
//
// These tests inspect the resolved member type / symbol that sema attaches to
// a member-access expression (verifying narrowing directly), and the
// diagnostics it raises (verifying §7.4 enforcement). Codegen JIT coverage of
// the narrowed access producing the right value lives in
// tests/codegen/null_narrow_member_test.cpp.

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::sema::FieldSymbol;
using bezel::sema::MethodSymbol;
using bezel::sema::PrimitiveType;
using bezel::sema::TypeRef;
using bezel::sema::TypeSymbol;

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// Walk every expression in the tree and return the first MemberAccessExpr
// whose `.member` matches `member` and whose target is a bare identifier
// named `targetName` (mirrors flow_narrowing_test.cpp's helper).
const bezel::ast::MemberAccessExpr* findMemberAccessOn(
    const bezel::ast::Expr* e, std::string_view targetName,
    std::string_view member) {
    if (!e) return nullptr;
    if (auto* ma = dynamic_cast<const bezel::ast::MemberAccessExpr*>(e)) {
        if (ma->member == member) {
            if (auto* id = dynamic_cast<const bezel::ast::IdentifierExpr*>(
                    ma->target.get())) {
                if (id->name == targetName) return ma;
            }
        }
        return findMemberAccessOn(ma->target.get(), targetName, member);
    }
    if (auto* ce = dynamic_cast<const bezel::ast::CallExpr*>(e)) {
        if (auto* hit =
                findMemberAccessOn(ce->callee.get(), targetName, member))
            return hit;
        for (const auto& a : ce->args)
            if (a.value)
                if (auto* hit = findMemberAccessOn(a.value.get(), targetName,
                                                   member))
                    return hit;
        return nullptr;
    }
    if (auto* be = dynamic_cast<const bezel::ast::BinaryExpr*>(e)) {
        if (auto* hit =
                findMemberAccessOn(be->left.get(), targetName, member))
            return hit;
        return findMemberAccessOn(be->right.get(), targetName, member);
    }
    return nullptr;
}

const bezel::ast::MemberAccessExpr* findMemberAccessOnStmt(
    const bezel::ast::Stmt* s, std::string_view targetName,
    std::string_view member) {
    if (!s) return nullptr;
    if (auto* ld = dynamic_cast<const bezel::ast::LocalDecl*>(s)) {
        return findMemberAccessOn(ld->initializer.get(), targetName, member);
    }
    if (auto* rs = dynamic_cast<const bezel::ast::ReturnStmt*>(s)) {
        return findMemberAccessOn(rs->value.get(), targetName, member);
    }
    if (auto* ds = dynamic_cast<const bezel::ast::DiscardStmt*>(s)) {
        return findMemberAccessOn(ds->value.get(), targetName, member);
    }
    if (auto* as = dynamic_cast<const bezel::ast::AssignmentStmt*>(s)) {
        if (auto* hit =
                findMemberAccessOn(as->value.get(), targetName, member))
            return hit;
        return findMemberAccessOn(as->target.get(), targetName, member);
    }
    if (auto* es = dynamic_cast<const bezel::ast::ExpressionStmt*>(s)) {
        return findMemberAccessOn(es->expr.get(), targetName, member);
    }
    if (auto* ifs = dynamic_cast<const bezel::ast::IfStmt*>(s)) {
        for (const auto& bs : ifs->ifBranch.body)
            if (auto* hit =
                    findMemberAccessOnStmt(bs.get(), targetName, member))
                return hit;
        for (const auto& el : ifs->elifBranches)
            for (const auto& bs : el.body)
                if (auto* hit =
                        findMemberAccessOnStmt(bs.get(), targetName, member))
                    return hit;
        for (const auto& bs : ifs->elseBody)
            if (auto* hit =
                    findMemberAccessOnStmt(bs.get(), targetName, member))
                return hit;
        return nullptr;
    }
    return nullptr;
}

// Find the body of method `methodName` (block form) and search every
// statement, in source order, for the requested member access.
const bezel::ast::MemberAccessExpr* findIn(
    const bezel::test::SemaResult& r, std::string_view methodName,
    std::string_view targetName, std::string_view member) {
    if (!r.sourceFile) return nullptr;
    for (const auto& topD : r.sourceFile->topDecls) {
        const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(topD.get());
        if (!td) continue;
        for (const auto& m : td->members) {
            const auto* md =
                dynamic_cast<const bezel::ast::MethodDecl*>(m.get());
            if (!md || md->name != methodName) continue;
            for (const auto& s : md->stmts)
                if (auto* hit =
                        findMemberAccessOnStmt(s.get(), targetName, member))
                    return hit;
        }
    }
    return nullptr;
}

}  // namespace

// ===========================================================================
// Null-narrowing: the value is narrowed to its non-nullable form.
// ===========================================================================

// §7.4: `if user <> null then ... user.Name` — the canonical example. Inside
// the then-block `user` is narrowed to its non-nullable form `User`, so the
// access is legal and `user.Name` resolves against `User`.
BEZEL_TEST(sema_null_narrow, ne_null_narrows_then_block_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class User public Name: int := 0; end User; "
        "class C "
        "  public static method Run(user: User?): int "
        "    if user <> null then "
        "      return user.Name; "
        "    end; "
        "    return -1; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* ma = findIn(r, "Run", "user", "Name");
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* fs = dynamic_cast<FieldSymbol*>(r.ctx->exprSymbol(*ma));
        BEZEL_EXPECT_TRUE(fs != nullptr);
        auto* t = r.ctx->exprType(*ma);
        BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
    }
}

// §17.4 / §7.4: `is not null` desugars to `<> null` (parser), so it narrows
// the then-block identically.
BEZEL_TEST(sema_null_narrow, is_not_null_narrows_then_block_spec_17_4) {
    auto r = parseSemaAll(
        "module M; "
        "class User public Name: int := 0; end User; "
        "class C "
        "  public static method Run(user: User?): int "
        "    if user is not null then "
        "      return user.Name; "
        "    end; "
        "    return -1; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §8.4 / §7.4: `if user = null then ... else user.Name` — the else-block of
// `= null` narrows `user` to non-null.
BEZEL_TEST(sema_null_narrow, eq_null_narrows_else_block_spec_8_4) {
    auto r = parseSemaAll(
        "module M; "
        "class User public Name: int := 0; end User; "
        "class C "
        "  public static method Run(user: User?): int "
        "    if user = null then "
        "      return -1; "
        "    else "
        "      return user.Name; "
        "    end; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* ma = findIn(r, "Run", "user", "Name");
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) BEZEL_EXPECT_TRUE(r.ctx->exprSymbol(*ma) != nullptr);
}

// §7.4: a nullable VALUE type also narrows. Here a nullable record value read
// under a `<> null` guard.
BEZEL_TEST(sema_null_narrow, nullable_record_narrows_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "record Box(V: int); "
        "class C "
        "  public static method Run(b: Box?): int "
        "    if b <> null then "
        "      return b.V; "
        "    end; "
        "    return -1; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* ma = findIn(r, "Run", "b", "V");
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* t = r.ctx->exprType(*ma);
        BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
    }
}

// §7.4 / §12.7: two null tests joined with `and` both narrow throughout the
// combined true branch.
BEZEL_TEST(sema_null_narrow, and_joined_null_tests_both_narrow_spec_12_7) {
    auto r = parseSemaAll(
        "module M; "
        "class A public X: int := 0; end A; "
        "class B public Y: int := 0; end B; "
        "class C "
        "  public static method Run(a: A?, b: B?): int "
        "    if a <> null and b <> null then "
        "      return a.X + b.Y; "
        "    end; "
        "    return -1; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(findIn(r, "Run", "a", "X") != nullptr);
    BEZEL_EXPECT_TRUE(findIn(r, "Run", "b", "Y") != nullptr);
}

// §7.4: a local variable (not just a parameter) narrows the same way — this is
// the §7.4 "copy a nullable field to a local before narrowing" pattern.
BEZEL_TEST(sema_null_narrow, local_variable_narrows_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class User public Name: int := 0; end User; "
        "class C "
        "  public static method Run(): int "
        "    var u: User? := new User(); "
        "    if u <> null then "
        "      return u.Name; "
        "    end; "
        "    return -1; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// §7.8: `!` force-unwrap narrows to non-null, so `x!.field` is legal.
// ===========================================================================

// §7.8 / §7.4: `x!.field` — the force-unwrap yields the non-nullable form, so
// member access is legal without an `if` guard.
BEZEL_TEST(sema_null_narrow, bang_unwrap_member_legal_spec_7_8) {
    auto r = parseSemaAll(
        "module M; "
        "class User public Name: int := 0; end User; "
        "class C "
        "  public static method Run(user: User?): int "
        "    return user!.Name; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// §7.5: `?.` (null-conditional) is legal without narrowing.
// ===========================================================================

// §7.5: `x?.field` is the safe form and needs no guard. The result type is
// always nullable, so this must NOT raise the §7.4 member-access error.
BEZEL_TEST(sema_null_narrow, null_conditional_member_legal_unguarded_spec_7_5) {
    auto r = parseSemaAll(
        "module M; "
        "class User public Name: int := 0; end User; "
        "class C "
        "  public static method Run(user: User?): int? "
        "    return user?.Name; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// §7.4 ENFORCEMENT: bare member access on a nullable value is an error.
// ===========================================================================

// §7.4: a direct `user.Name` on a nullable `User?` with NO narrowing is a
// compile error.
BEZEL_TEST(sema_null_narrow, bare_member_on_nullable_class_rejected_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class User public Name: int := 0; end User; "
        "class C "
        "  public static method Run(user: User?): int "
        "    return user.Name; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §7.4: a direct member read on a nullable RECORD value (`Box?`) with no guard
// is likewise rejected.
BEZEL_TEST(sema_null_narrow, bare_member_on_nullable_record_rejected_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "record Box(V: int); "
        "class C "
        "  public static method Run(b: Box?): int "
        "    return b.V; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §7.4: a method CALL on a nullable value with no narrowing is rejected too
// (member access covers method calls per §7.4).
BEZEL_TEST(sema_null_narrow, bare_method_call_on_nullable_rejected_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class User public method Greet(): int => 0; end User; "
        "class C "
        "  public static method Run(user: User?): int "
        "    return user.Greet(); "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §7.4: a NON-nullable value's member access is of course still legal (the
// enforcement must not over-reject the common case).
BEZEL_TEST(sema_null_narrow, member_on_nonnull_legal_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class User public Name: int := 0; end User; "
        "class C "
        "  public static method Run(user: User): int "
        "    return user.Name; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// Invalidation and scoping.
// ===========================================================================

// §7.4 / §12.7: reassignment within the controlled block invalidates the
// narrowing — a subsequent bare member access becomes an error again.
BEZEL_TEST(sema_null_narrow, reassignment_invalidates_narrowing_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class User public Name: int := 0; end User; "
        "class C "
        "  public static method Run(user: User?, other: User?): int "
        "    if user <> null then "
        "      user := other; "
        "      return user.Name; "
        "    end; "
        "    return -1; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §7.4 / §12.7: narrowing is block-scoped — after the controlled block the
// variable is nullable again, so a trailing bare access is an error.
BEZEL_TEST(sema_null_narrow, narrowing_does_not_leak_past_block_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class User public Name: int := 0; end User; "
        "class C "
        "  public static method Run(user: User?): int "
        "    if user <> null then "
        "      var inside: int := user.Name; "
        "      return inside; "
        "    end; "
        "    return user.Name; "
        "  end Run; "
        "end C;");
    // The access inside the block is legal; the trailing one (outside) is not.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §7.4: the then-block of `if user = null` does NOT narrow (the value is known
// to be null there) — a member access there is an error.
BEZEL_TEST(sema_null_narrow, eq_null_then_block_does_not_narrow_spec_8_4) {
    auto r = parseSemaAll(
        "module M; "
        "class User public Name: int := 0; end User; "
        "class C "
        "  public static method Run(user: User?): int "
        "    if user = null then "
        "      return user.Name; "
        "    end; "
        "    return -1; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §7.4 (#1188): a `ref` argument can be reassigned by the callee (including to
// null), so a prior null-narrowing on the passed-in local no longer holds after
// the call — a subsequent bare member access becomes an error again, exactly
// like a direct reassignment. (Before the fix this compiled and dereferenced
// the nulled value at run time → SIGSEGV.)
BEZEL_TEST(sema_null_narrow, ref_arg_invalidates_null_narrowing_1188) {
    auto r = parseSemaAll(
        "module M; "
        "class User public Name: int := 0; end User; "
        "class C "
        "  public static method Clear(ref u: User?) u := null; end Clear; "
        "  public static method Run(): int "
        "    var u: User? := new User(); "
        "    if u <> null then "
        "      Clear(ref u); "
        "      return u.Name; "
        "    end; "
        "    return -1; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §7.4 (#1188): an `out` argument is likewise (re)written by the callee, so it
// invalidates the passed-in local's null-narrowing.
BEZEL_TEST(sema_null_narrow, out_arg_invalidates_null_narrowing_1188) {
    auto r = parseSemaAll(
        "module M; "
        "class User public Name: int := 0; end User; "
        "class C "
        "  public static method Reset(out u: User?) u := null; end Reset; "
        "  public static method Run(): int "
        "    var u: User? := new User(); "
        "    if u <> null then "
        "      Reset(out u); "
        "      return u.Name; "
        "    end; "
        "    return -1; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §7.4 (#1188): a BY-VALUE argument cannot mutate the caller's local, so the
// narrowing must SURVIVE the call — the trailing member access stays legal.
// (The invalidation must not over-reject plain by-value calls.)
BEZEL_TEST(sema_null_narrow, byvalue_arg_keeps_narrowing_1188) {
    auto r = parseSemaAll(
        "module M; "
        "class User public Name: int := 0; end User; "
        "class C "
        "  public static method Look(u: User?) end Look; "
        "  public static method Run(): int "
        "    var u: User? := new User(); "
        "    if u <> null then "
        "      Look(u); "
        "      return u.Name; "
        "    end; "
        "    return -1; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// §7.4 (v0.182, #1566): closure-capture narrowing invalidation.
//
// A local narrowed non-null, then written to `null` by a lambda invoked
// (directly or indirectly) inside the controlled block, is no longer non-null.
// The spec rule is deliberately syntactic and conservative: a local is "mutably
// captured" when it is assigned, or passed `ref`/`out`, ANYWHERE inside ANY
// lambda of the enclosing function body; while such a local is narrowed, any
// operation in the controlled block that can transfer control to user code — a
// call, a function-value invocation, a constructor, or an `await` — invalidates
// the narrowing (a closure invoked in between may have nulled it). Straight-line
// check-then-use (no such operation BETWEEN the test and the use) stays accepted,
// and a local that is NOT mutably captured keeps its narrowing across a call.
// ===========================================================================

// §7.4 (#1566): the probe. `s` is narrowed non-null, then nulled by a lambda
// invoked inside the block, then dereferenced. Before the fix this compiled
// clean and SIGSEGV'd at run time; it must now be rejected (re-narrow required).
BEZEL_TEST(sema_null_narrow, closure_capture_mutation_invalidates_across_call_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Run(): int "
        "    var s: string? := \"hi\"; "
        "    var f := () => do s := null; end; "
        "    if s <> null then "
        "      f(); "
        "      return s.Length; "
        "    end; "
        "    return -1; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §7.4 (#1566): the invalidation is not limited to invoking the capturing closure
// itself — ANY control-transfer operation counts, because the closure could be
// reached indirectly. A call to an unrelated method between the test and the use
// invalidates a mutably-captured local just the same.
BEZEL_TEST(sema_null_narrow, captured_local_invalidated_by_unrelated_call_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Touch() end Touch; "
        "  public static method Run(): int "
        "    var s: string? := \"hi\"; "
        "    var f := () => do s := null; end; "
        "    if s <> null then "
        "      Touch(); "
        "      return s.Length; "
        "    end; "
        "    return -1; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §7.4 (#1566): a constructor (`new`) is likewise a control transfer, so it
// invalidates a mutably-captured local's narrowing.
BEZEL_TEST(sema_null_narrow, captured_local_invalidated_by_constructor_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class Thing public constructor() end; end Thing; "
        "class C "
        "  public static method Run(): int "
        "    var s: string? := \"hi\"; "
        "    var f := () => do s := null; end; "
        "    if s <> null then "
        "      _ := new Thing(); "
        "      return s.Length; "
        "    end; "
        "    return -1; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §7.4 (#1566): a lambda that passes the local as a `ref` argument mutates it
// (the callee may null it), so it is mutably captured — a call in the block
// invalidates the narrowing exactly as a direct assignment inside the lambda.
BEZEL_TEST(sema_null_narrow, ref_capture_in_lambda_invalidates_across_call_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Zap(ref u: string?) u := null; end Zap; "
        "  public static method Run(): int "
        "    var s: string? := \"hi\"; "
        "    var f := () => do Zap(ref s); end; "
        "    if s <> null then "
        "      f(); "
        "      return s.Length; "
        "    end; "
        "    return -1; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §7.4 (#1566): the invalidation also applies to a narrowing established by an
// early-exit guard at method-body scope, not only inside an `if` block. Here the
// surviving path is narrowed by `if s = null then return; end;`, then a call
// invalidates the mutably-captured local before the use.
BEZEL_TEST(sema_null_narrow, early_exit_narrowing_invalidated_by_call_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Touch() end Touch; "
        "  public static method Run(): int "
        "    var s: string? := \"hi\"; "
        "    var f := () => do s := null; end; "
        "    if s = null then return -1; end; "
        "    Touch(); "
        "    return s.Length; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §7.4 (#1566): NO over-invalidation. A closure that CAPTURES the local but only
// READS it (never assigns / ref/out-passes it) does not make the local mutably
// captured, so its narrowing survives a call — this must be ACCEPTED.
BEZEL_TEST(sema_null_narrow, non_mutating_closure_keeps_narrowing_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Run(): int "
        "    var s: string? := \"hi\"; "
        "    var f := () => do var z := s; end; "
        "    if s <> null then "
        "      f(); "
        "      return s.Length; "
        "    end; "
        "    return -1; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.4 (#1566): NO over-invalidation. A local that is NOT captured by any lambda
// keeps its narrowing across a call (the by-value #1188 guarantee), even though
// the body contains a lambda that mutates a DIFFERENT local.
BEZEL_TEST(sema_null_narrow, uncaptured_local_keeps_narrowing_across_call_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Touch() end Touch; "
        "  public static method Run(): int "
        "    var s: string? := \"hi\"; "
        "    var other: string? := \"x\"; "
        "    var f := () => do other := null; end; "
        "    if s <> null then "
        "      Touch(); "
        "      return s.Length; "
        "    end; "
        "    return -1; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.4 (#1566): straight-line check-then-use is preserved. Even for a mutably-
// captured local, if there is no control-transfer operation BETWEEN the test and
// the use, the narrowing holds — this must be ACCEPTED.
BEZEL_TEST(sema_null_narrow, straight_line_use_of_captured_local_accepted_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Run(): int "
        "    var s: string? := \"hi\"; "
        "    var f := () => do s := null; end; "
        "    if s <> null then "
        "      return s.Length; "
        "    end; "
        "    return -1; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.4 (#1566): a use that PRECEDES the call is accepted — a call's arguments are
// evaluated before control transfers, so a read before the invocation is under
// the still-valid narrowing.
BEZEL_TEST(sema_null_narrow, use_before_call_of_captured_local_accepted_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Sink(n: int) end Sink; "
        "  public static method Run(): int "
        "    var s: string? := \"hi\"; "
        "    var f := () => do s := null; end; "
        "    if s <> null then "
        "      Sink(s.Length); "
        "      f(); "
        "    end; "
        "    return 0; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.4 (#1566): NO over-invalidation from shadowing. A lambda whose OWN parameter
// shadows the outer name assigns only its parameter, not the outer local — so the
// outer local is not mutably captured and its narrowing survives a call.
BEZEL_TEST(sema_null_narrow, lambda_param_shadow_does_not_invalidate_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Run(): int "
        "    var s: string? := \"hi\"; "
        "    var f := (s: string?) => do s := null; end; "
        "    if s <> null then "
        "      f(s); "
        "      return s.Length; "
        "    end; "
        "    return -1; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// §7.4 (v0.182, #1593): the "accessor call" trigger of the mutably-captured-
// narrowing invalidation rule (§7.4 line 631: "any operation that can transfer
// control to user code: a method, constructor, ACCESSOR, or user-defined-
// operator call…"). #1566 wired the method/constructor/operator/await triggers
// but deferred the user-defined property/indexer getter — a call to a user
// accessor whose body could reach the capturing closure and null the local.
// These cases extend the #1566 tests to that trigger: a user accessor between
// the null test and the use invalidates a mutably-captured local (REJECT); a
// non-virtual auto-property (a pure backing-field load) and a builtin property
// do NOT, and an uncaptured local keeps its narrowing (no over-rejection).
// ===========================================================================

// §7.4 (#1593): a user-defined expression-body property getter (`=> e`) between
// the test and the use is an accessor call — it can transfer control to user
// code, so it invalidates a mutably-captured local's narrowing. Before the fix
// this compiled clean (the getter read was not a recognised control transfer).
BEZEL_TEST(sema_null_narrow, expr_body_property_getter_invalidates_capture_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class Box public property Val: int => 42; end Box; "
        "class C "
        "  public static method Run(box: Box): int "
        "    var s: string? := \"hi\"; "
        "    var f := () => do s := null; end; "
        "    if s <> null then "
        "      _ := box.Val; "
        "      return s.Length; "
        "    end; "
        "    return -1; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §7.4 (#1593): a block-body property with an explicit `get` (and `set`) arm is
// likewise a user accessor — reading it invalidates a mutably-captured local.
BEZEL_TEST(sema_null_narrow, block_body_property_getter_invalidates_capture_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class Box "
        "  private _v: int := 0; "
        "  public property Val: int "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end; "
        "end Box; "
        "class C "
        "  public static method Run(box: Box): int "
        "    var s: string? := \"hi\"; "
        "    var f := () => do s := null; end; "
        "    if s <> null then "
        "      _ := box.Val; "
        "      return s.Length; "
        "    end; "
        "    return -1; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §7.4 (#1593): a user-defined indexer access `g[i]` invokes the indexer getter
// (also an "accessor" per §7.4) — it invalidates a mutably-captured local too.
BEZEL_TEST(sema_null_narrow, user_indexer_getter_invalidates_capture_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class Grid public property [i: int]: int get => i; end; end Grid; "
        "class C "
        "  public static method Run(g: Grid): int "
        "    var s: string? := \"hi\"; "
        "    var f := () => do s := null; end; "
        "    if s <> null then "
        "      _ := g[3]; "
        "      return s.Length; "
        "    end; "
        "    return -1; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §7.4 (#1593): NO over-invalidation. A NON-virtual auto-property (`Val: int :=
// 0;`) getter is a compiler-generated backing-field load — it cannot transfer
// control to user code, so reading it does NOT drop a captured local's
// narrowing. This is the "exclude common auto reads" boundary the #1566 change
// reserved for #1593. Must be ACCEPTED.
BEZEL_TEST(sema_null_narrow, auto_property_read_keeps_capture_narrowing_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class Box public property Val: int := 42; end Box; "
        "class C "
        "  public static method Run(box: Box): int "
        "    var s: string? := \"hi\"; "
        "    var f := () => do s := null; end; "
        "    if s <> null then "
        "      _ := box.Val; "
        "      return s.Length; "
        "    end; "
        "    return -1; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.4 (#1593): NO over-invalidation. A local that is NOT mutably captured keeps
// its narrowing across a user accessor call, exactly as across an ordinary call
// (#1188). The lambda here mutates a DIFFERENT local. Must be ACCEPTED.
BEZEL_TEST(sema_null_narrow, uncaptured_local_keeps_narrowing_across_accessor_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class Box public property Val: int => 42; end Box; "
        "class C "
        "  public static method Run(box: Box): int "
        "    var s: string? := \"hi\"; "
        "    var other: string? := \"x\"; "
        "    var f := () => do other := null; end; "
        "    if s <> null then "
        "      _ := box.Val; "
        "      return s.Length; "
        "    end; "
        "    return -1; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
