// Spec-derived tests for §12.7 flow narrowing on `is` / `is not`.
//
// Per §12.7 (Type tests: is, is not, as): when `x is T` succeeds and the
// tested expression is a simple variable, the variable is flow-narrowed to
// `T` within the controlled (then) block. Member access on the narrowed
// variable then resolves against `T` — so a member declared only on the
// derived type `T` resolves there.  §12.7 further states:
//   * narrowing applies to the original variable;
//   * reassignment within the controlled block invalidates the narrowing;
//   * `is not T` narrows in the *else* branch (not the true branch);
//   * `is T` does not narrow in the else branch;
//   * two `is` tests joined with `and` are both in effect in the true block.
//
// These tests inspect the resolved member symbol / type the sema pass
// attaches to a member-access expression, so they verify the narrowing
// analysis directly (codegen JIT coverage lives in
// tests/codegen/is_narrow_receiver_test.cpp).

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
// named `targetName`. This lets a test pick out exactly `animal.Bark`
// (target `animal`, member `Bark`) regardless of where in the body it sits.
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

} // namespace

// §12.7: `if animal is Dog then animal.Bark(); end` — the canonical
// example. `Bark` is declared only on Dog; inside the controlled block
// `animal` is narrowed to Dog, so `animal.Bark` resolves to Dog's method.
BEZEL_TEST(sema_narrow, member_resolves_under_is_narrowing) {
    auto r = parseSemaAll(
        "module M; "
        "class Animal public virtual method Legs(): int => 0; end Animal; "
        "class Dog : Animal "
        "  public method Bark(): int => 100; "
        "end Dog; "
        "class C "
        "  public static method Run(animal: Animal): int "
        "    if animal is Dog then "
        "      return animal.Bark(); "
        "    end; "
        "    return -1; "
        "  end Run; "
        "end C;");
    auto* ma = findIn(r, "Run", "animal", "Bark");
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* sym = r.ctx->exprSymbol(*ma);
        auto* ms = dynamic_cast<MethodSymbol*>(sym);
        BEZEL_EXPECT_TRUE(ms != nullptr);
        if (ms) {
            BEZEL_EXPECT_TRUE(ms->parent != nullptr);
            if (ms->parent) BEZEL_EXPECT_TRUE(ms->parent->name == "Dog");
        }
    }
}

// §12.7: derived-only FIELD reads resolve under narrowing too.
BEZEL_TEST(sema_narrow, derived_field_resolves_under_is_narrowing) {
    auto r = parseSemaAll(
        "module M; "
        "class Animal public virtual method Legs(): int => 0; end Animal; "
        "class Dog : Animal public _tag: int := 77; end Dog; "
        "class C "
        "  public static method Run(animal: Animal): int "
        "    if animal is Dog then "
        "      return animal._tag; "
        "    end; "
        "    return -1; "
        "  end Run; "
        "end C;");
    auto* ma = findIn(r, "Run", "animal", "_tag");
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* fs = dynamic_cast<FieldSymbol*>(r.ctx->exprSymbol(*ma));
        BEZEL_EXPECT_TRUE(fs != nullptr);
        if (fs && fs->parent) BEZEL_EXPECT_TRUE(fs->parent->name == "Dog");
        auto* t = r.ctx->exprType(*ma);
        BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
    }
}

// §12.7: narrowing is scoped to the controlled block. Inside the block,
// `animal.Bark` resolves to Dog; after the block `animal` keeps its
// declared static type Animal, on which `Bark` is not declared — so the
// trailing access does NOT resolve (it stays unresolved, matching the
// pre-narrowing behavior; member resolution does not diagnose unknown
// members per slice 3d).
BEZEL_TEST(sema_narrow, narrowing_does_not_leak_past_block) {
    auto r = parseSemaAll(
        "module M; "
        "class Animal public virtual method Legs(): int => 0; end Animal; "
        "class Dog : Animal "
        "  public method Bark(): int => 100; "
        "end Dog; "
        "class C "
        "  public static method Run(animal: Animal): int "
        "    if animal is Dog then "
        "      var inside: int := animal.Bark(); "
        "      return inside; "
        "    end; "
        "    return animal.Bark(); "
        "  end Run; "
        "end C;");
    // The access inside the block resolves to Dog.Bark...
    auto* inside = findIn(r, "Run", "animal", "Bark");
    BEZEL_EXPECT_TRUE(inside != nullptr);
    if (inside) {
        auto* ms = dynamic_cast<MethodSymbol*>(r.ctx->exprSymbol(*inside));
        BEZEL_EXPECT_TRUE(ms != nullptr);
        if (ms && ms->parent) BEZEL_EXPECT_TRUE(ms->parent->name == "Dog");
    }
    // ...but the access after the block (in the trailing `return`) does
    // not: `animal` is back to its declared Animal type, which has no
    // `Bark`, so no symbol is attached. Locate the trailing access from
    // the method's last statement directly.
    const bezel::ast::MemberAccessExpr* after = nullptr;
    if (r.sourceFile) {
        for (const auto& topD : r.sourceFile->topDecls) {
            auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(topD.get());
            if (!td) continue;
            for (const auto& m : td->members) {
                auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(m.get());
                if (!md || md->name != "Run" || md->stmts.empty()) continue;
                auto* rs = dynamic_cast<const bezel::ast::ReturnStmt*>(
                    md->stmts.back().get());
                if (!rs) continue;
                auto* ce =
                    dynamic_cast<const bezel::ast::CallExpr*>(rs->value.get());
                after = dynamic_cast<const bezel::ast::MemberAccessExpr*>(
                    ce ? ce->callee.get() : rs->value.get());
            }
        }
    }
    BEZEL_EXPECT_TRUE(after != nullptr);
    if (after) {
        // Unresolved: Animal has no Bark, narrowing does not apply here.
        BEZEL_EXPECT_TRUE(r.ctx->exprSymbol(*after) == nullptr);
    }
}

// §12.7: `is not T` does not narrow in the true branch; in the `else`
// branch the variable is narrowed to T. So `animal.Bark` resolves to
// Dog.Bark inside the else branch of `if animal is not Dog`.
BEZEL_TEST(sema_narrow, is_not_narrows_in_else_branch) {
    auto r = parseSemaAll(
        "module M; "
        "class Animal public virtual method Legs(): int => 0; end Animal; "
        "class Dog : Animal "
        "  public method Bark(): int => 100; "
        "end Dog; "
        "class C "
        "  public static method Run(animal: Animal): int "
        "    if animal is not Dog then "
        "      return -1; "
        "    else "
        "      return animal.Bark(); "
        "    end; "
        "  end Run; "
        "end C;");
    auto* ma = findIn(r, "Run", "animal", "Bark");
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* ms = dynamic_cast<MethodSymbol*>(r.ctx->exprSymbol(*ma));
        BEZEL_EXPECT_TRUE(ms != nullptr);
        if (ms && ms->parent) BEZEL_EXPECT_TRUE(ms->parent->name == "Dog");
    }
}

// §12.7: `is T` does NOT narrow in the else branch (the value could be
// many non-T types). So `animal.Bark` in the else branch of
// `if animal is Dog` stays unresolved.
BEZEL_TEST(sema_narrow, is_T_does_not_narrow_in_else) {
    auto r = parseSemaAll(
        "module M; "
        "class Animal public virtual method Legs(): int => 0; end Animal; "
        "class Dog : Animal "
        "  public method Bark(): int => 100; "
        "end Dog; "
        "class C "
        "  public static method Run(animal: Animal): int "
        "    if animal is Dog then "
        "      return 1; "
        "    else "
        "      return animal.Bark(); "
        "    end; "
        "  end Run; "
        "end C;");
    auto* ma = findIn(r, "Run", "animal", "Bark");
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        // No narrowing in the else of `is T`: stays unresolved.
        BEZEL_EXPECT_TRUE(r.ctx->exprSymbol(*ma) == nullptr);
    }
}

// §12.7: two `is` tests joined with `and` are both in effect throughout
// the combined true branch.
BEZEL_TEST(sema_narrow, and_joined_tests_both_narrow) {
    auto r = parseSemaAll(
        "module M; "
        "class Animal public virtual method Legs(): int => 0; end Animal; "
        "class Dog : Animal public method Bark(): int => 1; end Dog; "
        "class Plant public virtual method Roots(): int => 0; end Plant; "
        "class Tree : Plant public method Grow(): int => 2; end Tree; "
        "class C "
        "  public static method Run(a: Animal, p: Plant): int "
        "    if a is Dog and p is Tree then "
        "      return a.Bark() + p.Grow(); "
        "    end; "
        "    return -1; "
        "  end Run; "
        "end C;");
    auto* bark = findIn(r, "Run", "a", "Bark");
    auto* grow = findIn(r, "Run", "p", "Grow");
    BEZEL_EXPECT_TRUE(bark != nullptr);
    BEZEL_EXPECT_TRUE(grow != nullptr);
    if (bark) {
        auto* ms = dynamic_cast<MethodSymbol*>(r.ctx->exprSymbol(*bark));
        BEZEL_EXPECT_TRUE(ms != nullptr);
        if (ms && ms->parent) BEZEL_EXPECT_TRUE(ms->parent->name == "Dog");
    }
    if (grow) {
        auto* ms = dynamic_cast<MethodSymbol*>(r.ctx->exprSymbol(*grow));
        BEZEL_EXPECT_TRUE(ms != nullptr);
        if (ms && ms->parent) BEZEL_EXPECT_TRUE(ms->parent->name == "Tree");
    }
}

// §12.7: "Reassignment within the controlled block invalidates the
// narrowing." After `animal := other;` inside the block, a subsequent
// `animal.Bark` no longer resolves to Dog (the variable is re-widened to
// its declared Animal type).
BEZEL_TEST(sema_narrow, reassignment_invalidates_narrowing) {
    auto r = parseSemaAll(
        "module M; "
        "class Animal public virtual method Legs(): int => 0; end Animal; "
        "class Dog : Animal "
        "  public method Bark(): int => 100; "
        "end Dog; "
        "class C "
        "  public static method Run(animal: Animal, other: Animal): int "
        "    if animal is Dog then "
        "      animal := other; "
        "      return animal.Bark(); "
        "    end; "
        "    return -1; "
        "  end Run; "
        "end C;");
    auto* ma = findIn(r, "Run", "animal", "Bark");
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        // After reassignment the narrowing is dropped; `animal` is Animal
        // again, which has no `Bark`, so the access stays unresolved.
        BEZEL_EXPECT_TRUE(r.ctx->exprSymbol(*ma) == nullptr);
    }
}

// §12.7: a local variable (not just a parameter) is a "simple variable"
// and narrows the same way.
BEZEL_TEST(sema_narrow, local_variable_narrows) {
    auto r = parseSemaAll(
        "module M; "
        "class Animal public virtual method Legs(): int => 0; end Animal; "
        "class Dog : Animal "
        "  public method Bark(): int => 100; "
        "end Dog; "
        "class C "
        "  public static method Run(): int "
        "    using var d := new Dog(); "
        "    var animal: Animal := d; "
        "    if animal is Dog then "
        "      return animal.Bark(); "
        "    end; "
        "    return -1; "
        "  end Run; "
        "end C;");
    auto* ma = findIn(r, "Run", "animal", "Bark");
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* ms = dynamic_cast<MethodSymbol*>(r.ctx->exprSymbol(*ma));
        BEZEL_EXPECT_TRUE(ms != nullptr);
        if (ms && ms->parent) BEZEL_EXPECT_TRUE(ms->parent->name == "Dog");
    }
}

// §12.7: interface-base narrowing — `is T` where the variable's static
// type is an interface still narrows to the concrete class T.
BEZEL_TEST(sema_narrow, interface_base_narrows_to_class) {
    auto r = parseSemaAll(
        "module M; "
        "interface IAnimal method Legs(): int; end IAnimal; "
        "class Dog : IAnimal "
        "  public method Legs(): int => 4; "
        "  public method Bark(): int => 100; "
        "end Dog; "
        "class C "
        "  public static method Run(animal: IAnimal): int "
        "    if animal is Dog then "
        "      return animal.Bark(); "
        "    end; "
        "    return -1; "
        "  end Run; "
        "end C;");
    auto* ma = findIn(r, "Run", "animal", "Bark");
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* ms = dynamic_cast<MethodSymbol*>(r.ctx->exprSymbol(*ma));
        BEZEL_EXPECT_TRUE(ms != nullptr);
        if (ms && ms->parent) BEZEL_EXPECT_TRUE(ms->parent->name == "Dog");
    }
}
