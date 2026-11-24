// fix #8767 fix-forward 3 (§28.3, §28.3.10, v0.359): the confinement proof of
// a property getter's block body. GetterConfinement.hpp states what it is
// for; this file is the allow-list.
//
// VALUES. A value "carries" when its type can hold a borrow: anything but a
// non-`Object` primitive (`string` included: §28.1, runtime-managed), `()`,
// an enum, or a record / inline array whose every component, transitively, is
// one of those. A function type carries (§28.3's closure bullet: a closure
// whose source set holds the local). A carrying expression is one of
//   ROOTED    derived from a local or parameter of the body: the identifier,
//             a FIELD read through it, `!` or a conditional over those;
//   OWNED     a `new C(…)` (C verified, below) or `move <local>`;
//   LAMBDA    a lambda expression;
//   EXTERNAL  `this`, a field of `this`, a static field, a field path through
//             those, `null`: storage the body does not own.
// Any other carrying expression — a call, property or indexer RESULT, a user
// operator result, a collection literal — is denied: the body cannot tell
// whether such a result is a borrow or an owned value its producer also
// published. Two edges of that rule: a call that IS a statement is admitted
// whatever its type (its result is discarded; sema leaves a call of a method
// with no return type untyped), and a non-carrying value bound to a local of
// a carrying type (a `string` viewed as `Object`) is denied as a conversion
// the list does not classify.
//
// POSITIONS. A ROOTED value may appear only as
//   * the initializer of a `var` local (an alias, never the right-hand side
//     of an assignment: an alias cannot be re-pointed at a shorter-lived
//     source),
//   * the receiver of a FIELD read or of a field write of a non-carrying
//     value (a property, method, indexer or operator on it is a call that
//     receives it — r42-rev-s77's r01, r03, r04, r09, r17, r19),
//   * a `null` comparison the compiler did not resolve to a user operator,
//   * the operand of `move` or the receiver of the explicit `Dispose()` /
//     `?.Dispose()` discharge, when it names a local directly.
// It may not be an argument, a call / property / indexer / operator
// operand, a `return`, `throw` or `yield` value, an interpolation part, or a
// value stored anywhere but a local's own declaration — inside a lambda body
// as much as outside one (r11, r20). An OWNED value may only be bound to a
// local (declaration or assignment), thrown (`new`), or stored into an
// `owned` field of `this` (`move`). A LAMBDA may only initialize a local.
//
// FROZEN SOURCES. A local that an alias was initialized from, or that a
// lambda body mentions, may not be assigned, moved or explicitly disposed
// anywhere in the body; an alias (or lambda) local may not be either, and a
// local is never both an alias and an owner. So the object such a local
// names is freed at the local's own scope end and nowhere earlier, and every
// alias and closure of it is declared in that scope or inside it. The proof
// therefore does not lean on the dataflow's borrow invalidation.
//
// VERIFIED CLASSES. `new C(…)` is OWNED only when C — and every base class —
// is a source class whose instance constructors, instance field and
// auto-property initializers, invariants and destructor pass this same
// allow-list with `this` (and every instance field read) ROOTED: neither
// construction nor disposal hands `this` to other code. A class without
// source (builtin, imported), a type parameter (`new T()`), a constructor
// with a modifier-carrying parameter or a contract clause is unverified.
// `Dispose()` on a class is compiler-generated (§28.2: "Declaring a member
// named `Dispose` on a class … is a compile error"), so the explicit
// discharge runs only the verified destructor chain.
//
// WHAT THE PROOF LEAVES TO THE DATAFLOW: classifying each local as owned or
// borrowed at its binding, disposing an owned local's previous value on
// reassignment, and tracking its moved / disposed state to the scope end.
// When the dataflow would have REJECTED the body (a suppressed error),
// `runGetterBody` restores `GetterFlagSnapshot`.

#include <functional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/Node.hpp"
#include "ownership/GetterConfinement.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"

namespace bezel::ownership {

namespace {

using StmtList = std::vector<std::unique_ptr<ast::Stmt>>;

enum class Val {
    NonCarrying,
    Null,
    External,
    Rooted,
    OwnedNew,
    OwnedMove,
    Lambda,
    TypeName,
};

enum class Pos {
    Any,         // the caller checks the class itself
    Use,         // handed to code the proof does not see
    UseOrType,   // a call / member receiver: `Use`, or a type name
    FieldRecv,   // the receiver of a field read or write
    KeptInit,    // a `var` local's initializer
    KeptAssign,  // the value assigned to a local
    Throw,       // a `throw` operand
};

bool hasModifier(const std::vector<std::string_view>& mods,
                 std::string_view m) {
    for (std::string_view x : mods)
        if (x == m) return true;
    return false;
}

struct Shared {
    explicit Shared(sema::SemaContext& c) : ctx(c) {}

    sema::SemaContext& ctx;

    // --- types ---------------------------------------------------------
    std::unordered_set<const sema::TypeSymbol*> recordsOnPath;
    bool carries(const sema::TypeRef* t) {
        if (!t) return true;
        switch (t->kind) {
            case sema::TypeRef::Kind::Primitive:
                return t->primitive == sema::PrimitiveType::Object ||
                       t->primitive == sema::PrimitiveType::None;
            case sema::TypeRef::Kind::Unit:
                return false;
            case sema::TypeRef::Kind::InlineArray:
                return carries(t->arrayElement);
            case sema::TypeRef::Kind::Named: {
                const sema::TypeSymbol* ts = t->declaration;
                if (!ts) return true;  // bare type parameter, builtin head
                if (ts->kind == ast::TypeDeclKind::Enum) return false;
                if (ts->kind != ast::TypeDeclKind::Record) return true;
                // A generic record's components may be its type parameters,
                // which this walk does not substitute: such a component
                // reads as a bare type parameter and carries.
                if (!recordsOnPath.insert(ts).second) return true;
                const bool c = sema::anyRecordStorageField(
                    ts, ctx,
                    [&](const sema::Symbol*, const sema::TypeRef* ft) {
                        return carries(ft);
                    });
                recordsOnPath.erase(ts);
                return c;
            }
            default:
                return true;  // Function, Error
        }
    }

    // --- locals --------------------------------------------------------
    std::unordered_map<const ast::Node*, sema::LocalSymbol*> localByDecl;
    bool localsIndexed = false;
    sema::LocalSymbol* localOf(const ast::Node& decl) {
        if (!localsIndexed) {
            localsIndexed = true;
            for (const auto& s : ctx.symbols())
                if (auto* ls = dynamic_cast<sema::LocalSymbol*>(s.get()))
                    if (ls->decl) localByDecl.emplace(ls->decl, ls);
        }
        auto it = localByDecl.find(&decl);
        return it == localByDecl.end() ? nullptr : it->second;
    }

    // --- verified classes ----------------------------------------------
    enum class Verdict { Pending, Yes, No };
    std::unordered_map<const sema::TypeSymbol*, Verdict> classes;
    int classDepth = 0;
    bool cycleSeen = false;
    bool classIsConfining(const sema::TypeSymbol* ts);
    bool classMembersConfine(const ast::TypeDecl& td);
};

class Screen {
public:
    Screen(Shared& sh, bool thisMode, GetterFlagSnapshot* snap)
        : sh_(sh), ctx_(sh.ctx), thisMode_(thisMode), snap_(snap) {}

    // A whole body: the walk, then the frozen-source checks.
    bool body(const StmtList& stmts) {
        walkBody(stmts);
        if (denied_) return false;
        for (const sema::Symbol* s : mutated_)
            if (sources_.count(s) || borrowLocals_.count(s)) return false;
        for (const sema::Symbol* s : ownedLocals_)
            if (borrowLocals_.count(s)) return false;
        return true;
    }

    // An instance field / auto-property initializer of a class under
    // verification: the value is stored into a field of `this`.
    bool ownFieldInitializer(const ast::Expr& e) {
        const Val v = visit(e, Pos::Any);
        return !denied_ && storableIntoOwnField(v);
    }

    // A lone expression evaluated for its value by code outside the proof
    // (an invariant, a base-constructor argument).
    bool useExpression(const ast::Expr& e) {
        visit(e, Pos::Use);
        return !denied_;
    }

private:
    Shared& sh_;
    sema::SemaContext& ctx_;
    const bool thisMode_;
    GetterFlagSnapshot* snap_;
    bool denied_ = false;
    int lambdaDepth_ = 0;
    const ast::Expr* statementCall_ = nullptr;
    std::unordered_set<const sema::Symbol*> sources_;
    std::unordered_set<const sema::Symbol*> borrowLocals_;
    std::unordered_set<const sema::Symbol*> ownedLocals_;
    std::unordered_set<const sema::Symbol*> mutated_;

    Val deny() {
        denied_ = true;
        return Val::NonCarrying;
    }

    void keep(const bool& f) {
        if (!snap_) return;
        bool* p = const_cast<bool*>(&f);
        snap_->flags.emplace_back(p, *p);
    }
    void keepSymbol(const sema::Symbol* s) {
        if (!snap_ || !s) return;
        keep(s->borrowedByMethodGroup);
        if (auto* ls = dynamic_cast<const sema::LocalSymbol*>(s))
            keep(ls->autoDropAtScopeEnd);
    }

    static bool isStaticMember(const sema::MemberSymbol* ms) {
        return hasModifier(ms->modifiers, "static");
    }

    static bool allowed(Val v, Pos pos) {
        switch (pos) {
            case Pos::Any:
                return true;
            case Pos::Use:
                return v == Val::NonCarrying || v == Val::Null ||
                       v == Val::External;
            case Pos::UseOrType:
                return v == Val::NonCarrying || v == Val::Null ||
                       v == Val::External || v == Val::TypeName;
            case Pos::FieldRecv:
                return v == Val::NonCarrying || v == Val::External ||
                       v == Val::Rooted || v == Val::TypeName;
            case Pos::KeptInit:
                return v != Val::TypeName;
            case Pos::KeptAssign:
                return v == Val::Null || v == Val::OwnedNew ||
                       v == Val::OwnedMove;
            case Pos::Throw:
                return v == Val::NonCarrying || v == Val::Null ||
                       v == Val::External || v == Val::OwnedNew;
        }
        return false;
    }

    bool storableIntoOwnField(Val v) const {
        // Only a class under verification stores an owned value into its
        // own field here; the getter-mode `move` store is checked at the
        // assignment.
        return v == Val::NonCarrying || v == Val::Null ||
               v == Val::External || (thisMode_ && v == Val::OwnedNew) ||
               (thisMode_ && v == Val::OwnedMove);
    }

    Val combine(Val a, Val b) {
        if (a == b) return a;
        if (a == Val::Null) return b;
        if (b == Val::Null) return a;
        if ((a == Val::OwnedNew && b == Val::OwnedMove) ||
            (a == Val::OwnedMove && b == Val::OwnedNew))
            return Val::OwnedNew;
        return deny();
    }

    static const ast::Expr* stripBang(const ast::Expr* e) {
        while (auto* pb = dynamic_cast<const ast::PostfixBangExpr*>(e))
            e = pb->operand.get();
        return e;
    }

    const sema::TypeRef* localType(const sema::Symbol* sym,
                                   const ast::Expr& use) {
        if (auto* ls = dynamic_cast<const sema::LocalSymbol*>(sym))
            if (ls->declType) return ls->declType;
        return ctx_.exprType(use);
    }

    // ------------------------------------------------------------------
    // expressions
    // ------------------------------------------------------------------
    Val visit(const ast::Expr& e, Pos pos) {
        if (denied_) return Val::NonCarrying;
        const Val v = classify(e, pos);
        if (denied_) return Val::NonCarrying;
        if (!allowed(v, pos)) return deny();
        return v;
    }

    Val visitIdentifier(const ast::IdentifierExpr& id) {
        keep(id.owningMatchTransferBinding);
        keep(id.disposeReclaimsReassignedBorrow);
        if (id.name == "this") return thisMode_ ? Val::Rooted : Val::External;
        if (id.name == "base") return deny();
        sema::Symbol* sym = ctx_.exprSymbol(id);
        if (dynamic_cast<sema::TypeSymbol*>(sym)) return Val::TypeName;
        if (auto* ls = dynamic_cast<sema::LocalSymbol*>(sym)) {
            keepSymbol(ls);
            if (!sh_.carries(localType(ls, id))) return Val::NonCarrying;
            if (lambdaDepth_ > 0) sources_.insert(ls);
            return Val::Rooted;
        }
        const sema::TypeRef* type = ctx_.exprType(id);
        if (auto* ps = dynamic_cast<sema::ParameterSymbol*>(sym)) {
            if (!sh_.carries(type)) return Val::NonCarrying;
            // A constructor's parameter is the caller's value: storage the
            // object under verification does not own.
            if (thisMode_) return Val::External;
            if (lambdaDepth_ > 0) sources_.insert(ps);
            return Val::Rooted;
        }
        if (auto* fs = dynamic_cast<sema::FieldSymbol*>(sym)) {
            if (!sh_.carries(type)) return Val::NonCarrying;
            return thisMode_ && !isStaticMember(fs) ? Val::Rooted
                                                    : Val::External;
        }
        if (auto* prop = dynamic_cast<sema::PropertySymbol*>(sym)) {
            // An implicit-`this` property read is a call on `this`.
            if (thisMode_ && !isStaticMember(prop)) return deny();
            return sh_.carries(type) ? deny() : Val::NonCarrying;
        }
        if (dynamic_cast<sema::MethodSymbol*>(sym)) return deny();
        return sh_.carries(type) ? deny() : Val::NonCarrying;
    }

    Val visitMember(const ast::Expr& e, const ast::Expr* target) {
        sema::Symbol* sym = ctx_.exprSymbol(e);
        // A qualified type name: nothing is evaluated.
        if (dynamic_cast<sema::TypeSymbol*>(sym)) return Val::TypeName;
        if (!target) return deny();
        const sema::TypeRef* type = ctx_.exprType(e);
        if (dynamic_cast<sema::FieldSymbol*>(sym)) {
            const Val tv = visit(*target, Pos::FieldRecv);
            if (denied_) return Val::NonCarrying;
            if (!sh_.carries(type)) return Val::NonCarrying;
            if (tv == Val::Rooted) return Val::Rooted;
            if (tv == Val::External || tv == Val::TypeName)
                return Val::External;
            return deny();
        }
        // A property read, an enum constant, a builtin member: code that
        // receives the target.
        visit(*target, Pos::UseOrType);
        if (denied_) return Val::NonCarrying;
        return sh_.carries(type) ? deny() : Val::NonCarrying;
    }

    void visitArgs(const std::vector<ast::CallArgument>& args) {
        for (const auto& a : args) {
            if (denied_) return;
            if (!a.modifier.empty() || a.isDiscard || !a.value) {
                deny();
                return;
            }
            visit(*a.value, Pos::Use);
        }
    }

    Val visitCall(const ast::Expr& e, const ast::Expr* callee,
                  const std::vector<ast::CallArgument>& args) {
        if (!callee) return deny();
        const ast::Expr* recv = nullptr;
        std::string_view member;
        bool hasRecv = false;
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(callee)) {
            recv = ma->target.get();
            member = ma->member;
            hasRecv = true;
        } else if (auto* nm =
                       dynamic_cast<const ast::NullCondMemberExpr*>(callee)) {
            recv = nm->target.get();
            member = nm->member;
            hasRecv = true;
        }
        bool chain = false;
        if (hasRecv) {
            if (!recv) return deny();
            // The explicit discharge of a local (§28.6): the generated
            // `Dispose()` runs the verified destructor chain only.
            if (member == "Dispose" && args.empty())
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                        stripBang(recv)))
                    if (auto* ls = dynamic_cast<sema::LocalSymbol*>(
                            ctx_.exprSymbol(*id))) {
                        keep(id->owningMatchTransferBinding);
                        keep(id->disposeReclaimsReassignedBorrow);
                        keepSymbol(ls);
                        mutated_.insert(ls);
                        if (lambdaDepth_ > 0) sources_.insert(ls);
                        return Val::NonCarrying;
                    }
            visit(*recv, Pos::UseOrType);
        } else if (auto* id =
                       dynamic_cast<const ast::IdentifierExpr*>(callee)) {
            keep(id->owningMatchTransferBinding);
            keep(id->disposeReclaimsReassignedBorrow);
            if (id->name == "this" || id->name == "base") {
                // A constructor chain call (§22.10): the chained
                // constructor is verified with its class.
                if (!thisMode_) return deny();
                chain = true;
            } else {
                sema::Symbol* sym = ctx_.exprSymbol(*id);
                if (dynamic_cast<sema::LocalSymbol*>(sym) ||
                    dynamic_cast<sema::ParameterSymbol*>(sym)) {
                    // Calling a local closure: its body is screened where
                    // the lambda is written.
                    keepSymbol(sym);
                    if (lambdaDepth_ > 0) sources_.insert(sym);
                } else if (auto* ms =
                               dynamic_cast<sema::MethodSymbol*>(sym)) {
                    // An implicit-`this` call hands `this` to the callee.
                    if (thisMode_ && !isStaticMember(ms)) return deny();
                } else if (auto* fs =
                               dynamic_cast<sema::FieldSymbol*>(sym)) {
                    // A closure stored in a field: external code. In a
                    // class under verification no lambda is admitted, so
                    // the closure cannot hold `this`.
                    (void)fs;
                } else {
                    return deny();
                }
            }
        } else {
            return deny();
        }
        visitArgs(args);
        if (denied_) return Val::NonCarrying;
        if (chain) return Val::NonCarrying;
        // A call that IS a statement: its result — none at all for a call
        // of a method with no return type, which sema leaves untyped — is
        // discarded, so no local of the body can come to hold it.
        if (&e == statementCall_) return Val::NonCarrying;
        return sh_.carries(ctx_.exprType(e)) ? deny() : Val::NonCarrying;
    }

    Val visitNew(const ast::NewExpr& ne, Pos pos) {
        visitArgs(ne.args);
        if (denied_) return Val::NonCarrying;
        const sema::TypeRef* type = ctx_.exprType(ne);
        if (!sh_.carries(type)) return Val::NonCarrying;
        // A thrown object is not freed by this body.
        if (pos == Pos::Throw) return Val::OwnedNew;
        if (!type || type->kind != sema::TypeRef::Kind::Named ||
            !type->declaration || !sh_.classIsConfining(type->declaration))
            return deny();
        return Val::OwnedNew;
    }

    Val visitLambda(const ast::LambdaExpr& le, Pos pos) {
        if (thisMode_ || le.isAsync || pos != Pos::KeptInit) return deny();
        for (const auto& p : le.params)
            if (!p.modifier.empty()) return deny();
        ++lambdaDepth_;
        if (le.isBlockBody) {
            walkBody(le.stmts);
        } else if (le.exprBody) {
            visit(*le.exprBody, Pos::Use);
        } else {
            deny();
        }
        --lambdaDepth_;
        return Val::Lambda;
    }

    void walkArm(const ast::CaseArm& arm) {
        if (arm.guard) visit(*arm.guard, Pos::Use);
        if (arm.bodyKind == ast::CaseArmBodyKind::Block) {
            walkBody(arm.stmts);
        } else if (arm.exprBody) {
            visit(*arm.exprBody, Pos::Use);
        }
    }

    Val classify(const ast::Expr& e, Pos pos) {
        if (dynamic_cast<const ast::IntLiteralExpr*>(&e) ||
            dynamic_cast<const ast::FloatLiteralExpr*>(&e) ||
            dynamic_cast<const ast::CharLiteralExpr*>(&e) ||
            dynamic_cast<const ast::StringLiteralExpr*>(&e) ||
            dynamic_cast<const ast::VerbatimStringExpr*>(&e) ||
            dynamic_cast<const ast::BoolLiteralExpr*>(&e) ||
            dynamic_cast<const ast::UnitExpr*>(&e) ||
            dynamic_cast<const ast::TypeOfExpr*>(&e) ||
            dynamic_cast<const ast::NameOfExpr*>(&e))
            return Val::NonCarrying;
        if (dynamic_cast<const ast::NullLiteralExpr*>(&e)) return Val::Null;
        if (dynamic_cast<const ast::DefaultExpr*>(&e))
            return sh_.carries(ctx_.exprType(e)) ? Val::Null
                                                 : Val::NonCarrying;
        if (auto* is = dynamic_cast<const ast::InterpolatedStringExpr*>(&e)) {
            for (const auto& part : is->parts)
                if (part.expr) visit(*part.expr, Pos::Use);
            return Val::NonCarrying;
        }
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e))
            return visitIdentifier(*id);
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e))
            return visitMember(e, ma->target.get());
        if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(&e))
            return visitMember(e, nm->target.get());
        if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e))
            return visitCall(e, ce->callee.get(), ce->args);
        if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(&e))
            return visitCall(e, nc->callee.get(), nc->args);
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
            if (!ix->target) return deny();
            visit(*ix->target, Pos::Use);
            visitArgs(ix->indices);
            if (denied_) return Val::NonCarrying;
            return sh_.carries(ctx_.exprType(e)) ? deny() : Val::NonCarrying;
        }
        if (auto* pb = dynamic_cast<const ast::PostfixBangExpr*>(&e)) {
            if (!pb->operand) return deny();
            return visit(*pb->operand, pos);
        }
        if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e))
            return visitNew(*ne, pos);
        if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e)) {
            if (!be->left || !be->right) return deny();
            // A recorded symbol is the user operator the expression
            // resolved to (r42-rev-s77's r09, r19).
            const bool userOp = ctx_.exprSymbol(*be) != nullptr;
            const bool lNull =
                dynamic_cast<const ast::NullLiteralExpr*>(be->left.get());
            const bool rNull =
                dynamic_cast<const ast::NullLiteralExpr*>(be->right.get());
            if (!userOp && (be->op == "=" || be->op == "<>") &&
                (lNull || rNull)) {
                if (!lNull) visit(*be->left, Pos::FieldRecv);
                if (!rNull) visit(*be->right, Pos::FieldRecv);
                return Val::NonCarrying;
            }
            visit(*be->left, Pos::Use);
            visit(*be->right, Pos::Use);
            if (denied_) return Val::NonCarrying;
            if (!sh_.carries(ctx_.exprType(e))) return Val::NonCarrying;
            if (!userOp && be->op == "??") return Val::External;
            return deny();
        }
        if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e)) {
            if (!ue->operand) return deny();
            if (ue->op == "move") {
                if (ue->replacement) return deny();
                auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                    ue->operand.get());
                if (!id) return deny();
                auto* ls =
                    dynamic_cast<sema::LocalSymbol*>(ctx_.exprSymbol(*id));
                if (!ls) return deny();
                keep(id->owningMatchTransferBinding);
                keep(id->disposeReclaimsReassignedBorrow);
                keepSymbol(ls);
                if (!sh_.carries(localType(ls, *id)))
                    return Val::NonCarrying;
                mutated_.insert(ls);
                if (lambdaDepth_ > 0) sources_.insert(ls);
                return Val::OwnedMove;
            }
            visit(*ue->operand, Pos::Use);
            if (denied_) return Val::NonCarrying;
            return sh_.carries(ctx_.exprType(e)) ? deny() : Val::NonCarrying;
        }
        if (auto* tt = dynamic_cast<const ast::TypeTestExpr*>(&e)) {
            if (!tt->operand) return deny();
            visit(*tt->operand, Pos::Use);
            return sh_.carries(ctx_.exprType(e)) ? deny() : Val::NonCarrying;
        }
        if (auto* ce = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
            if (!ce->condition || !ce->thenValue || !ce->elseValue)
                return deny();
            visit(*ce->condition, Pos::Use);
            const Val a = visit(*ce->thenValue, pos);
            const Val b = visit(*ce->elseValue, pos);
            if (denied_) return Val::NonCarrying;
            return combine(a, b);
        }
        if (auto* te = dynamic_cast<const ast::ThrowExpr*>(&e)) {
            if (te->operand) visit(*te->operand, Pos::Throw);
            return Val::Null;  // never yields a value
        }
        if (auto* re = dynamic_cast<const ast::RangeExpr*>(&e)) {
            // Admitted as a `for` iterable (walkStmt) or where the range
            // type does not carry.
            visitRangeOperands(*re);
            return sh_.carries(ctx_.exprType(e)) ? deny() : Val::NonCarrying;
        }
        if (auto* we = dynamic_cast<const ast::WithExpr*>(&e)) {
            if (!we->source || sh_.carries(ctx_.exprType(e))) return deny();
            visit(*we->source, Pos::Use);
            for (const auto& a : we->assigns) {
                if (!a.value) return deny();
                visit(*a.value, Pos::Use);
            }
            return Val::NonCarrying;
        }
        if (auto* le = dynamic_cast<const ast::LambdaExpr*>(&e))
            return visitLambda(*le, pos);
        if (auto* cs = dynamic_cast<const ast::CaseExpr*>(&e)) {
            keep(cs->disposeOwnedScrutinee);
            if (cs->scrutinee) {
                visit(*cs->scrutinee, Pos::Use);
                if (sh_.carries(ctx_.exprType(*cs->scrutinee))) return deny();
            }
            for (const auto& arm : cs->arms) walkArm(arm);
            if (denied_) return Val::NonCarrying;
            return sh_.carries(ctx_.exprType(e)) ? deny() : Val::NonCarrying;
        }
        // CollectionLiteralExpr, MarkedTransferExpr, BindExpr and any node
        // kind this allow-list does not name.
        return deny();
    }

    void visitRangeOperands(const ast::RangeExpr& re) {
        if (re.lo) visit(*re.lo, Pos::Use);
        if (re.hi) visit(*re.hi, Pos::Use);
        if (re.step) visit(*re.step, Pos::Use);
    }

    // ------------------------------------------------------------------
    // statements
    // ------------------------------------------------------------------
    void walkBody(const StmtList& stmts) {
        for (const auto& s : stmts) {
            if (denied_) return;
            if (!s) {
                deny();
                return;
            }
            walkStmt(*s);
        }
    }

    // The root locals of a ROOTED alias initializer.
    void markSources(const ast::Expr* e) {
        if (!e) return;
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(e)) {
            sema::Symbol* sym = ctx_.exprSymbol(*id);
            if (dynamic_cast<sema::LocalSymbol*>(sym) ||
                dynamic_cast<sema::ParameterSymbol*>(sym))
                sources_.insert(sym);
        } else if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
            markSources(ma->target.get());
        } else if (auto* nm =
                       dynamic_cast<const ast::NullCondMemberExpr*>(e)) {
            markSources(nm->target.get());
        } else if (auto* pb = dynamic_cast<const ast::PostfixBangExpr*>(e)) {
            markSources(pb->operand.get());
        } else if (auto* ce = dynamic_cast<const ast::ConditionalExpr*>(e)) {
            markSources(ce->thenValue.get());
            markSources(ce->elseValue.get());
        }
    }

    void walkLocalDecl(const ast::LocalDecl& ld) {
        keep(ld.autoDropAtScopeEnd);
        keep(ld.usingVarSourceIsBorrow);
        keep(ld.becomesOwnedViaReassign);
        keep(ld.initializerCallIsBorrow);
        keep(ld.dischargedByDiscard);
        keep(ld.dischargedByArmTransfer);
        // `using var` disposes at scope end with or without the dataflow
        // (§28.10); its borrow / owner split is the dataflow's own, so the
        // proof does not vouch for it. A `bind` clause and the `owned`
        // transfer marker are likewise outside the allow-list.
        if (ld.isUsing || ld.isAwaitUsing || ld.isBindAtomClause ||
            ld.initializerHasOwnedMarker) {
            deny();
            return;
        }
        sema::LocalSymbol* ls = sh_.localOf(ld);
        if (!ls) {
            deny();
            return;
        }
        keepSymbol(ls);
        if (!ld.initializer) return;
        const Val v = visit(*ld.initializer, Pos::KeptInit);
        if (denied_) return;
        switch (v) {
            case Val::Rooted:
                markSources(ld.initializer.get());
                borrowLocals_.insert(ls);
                break;
            case Val::External:
            case Val::Lambda:
                borrowLocals_.insert(ls);
                break;
            case Val::OwnedNew:
            case Val::OwnedMove:
                ownedLocals_.insert(ls);
                break;
            case Val::NonCarrying: {
                // A value that carries nothing bound at a type that does
                // (a box, a `string` viewed as `Object`): a conversion the
                // allow-list does not classify.
                const sema::TypeRef* lt =
                    ls->declType ? ls->declType
                                 : ctx_.exprType(*ld.initializer);
                if (sh_.carries(lt)) deny();
                break;
            }
            default:
                break;
        }
    }

    // `value` stored into a field that is not reached through a ROOTED
    // receiver: a field of `this`, a static field, a field of external
    // storage.
    void storeIntoField(const sema::FieldSymbol& fs, bool ownField,
                        const ast::Expr& value) {
        const Val v = visit(value, Pos::Any);
        if (denied_) return;
        if (v == Val::NonCarrying || v == Val::Null || v == Val::External)
            return;
        if (thisMode_) {
            // The object under verification stores what it creates into
            // its own instance fields only.
            if (ownField && (v == Val::OwnedNew || v == Val::OwnedMove))
                return;
        } else if (v == Val::OwnedMove && ownField &&
                   hasModifier(fs.modifiers, "owned")) {
            // `_f := move t;` — the getter's own `owned` field takes the
            // local's value (§28.8.1); the local is moved-from.
            return;
        }
        deny();
    }

    void walkAssignment(const ast::AssignmentStmt& as) {
        keep(as.wholeValueLhsMovedFrom);
        keep(as.lhsMovedFromAtStore);
        if (!as.target || !as.value || as.valueHasOwnedMarker) {
            deny();
            return;
        }
        if (as.op != ":=") {
            // A compound assignment reads and writes a non-carrying place.
            if (sh_.carries(ctx_.exprType(*as.target))) {
                deny();
                return;
            }
            visit(*as.target, Pos::Use);
            visit(*as.value, Pos::Use);
            return;
        }
        if (auto* id =
                dynamic_cast<const ast::IdentifierExpr*>(as.target.get())) {
            keep(id->owningMatchTransferBinding);
            keep(id->disposeReclaimsReassignedBorrow);
            sema::Symbol* sym = ctx_.exprSymbol(*id);
            if (auto* ls = dynamic_cast<sema::LocalSymbol*>(sym)) {
                keepSymbol(ls);
                if (!sh_.carries(localType(ls, *id))) {
                    visit(*as.value, Pos::Use);
                    return;
                }
                const Val v = visit(*as.value, Pos::KeptAssign);
                if (denied_) return;
                mutated_.insert(ls);
                if (lambdaDepth_ > 0) sources_.insert(ls);
                if (v == Val::OwnedNew || v == Val::OwnedMove)
                    ownedLocals_.insert(ls);
                return;
            }
            if (dynamic_cast<sema::ParameterSymbol*>(sym)) {
                if (sh_.carries(ctx_.exprType(*id))) {
                    deny();
                    return;
                }
                visit(*as.value, Pos::Use);
                return;
            }
            if (auto* fs = dynamic_cast<sema::FieldSymbol*>(sym)) {
                storeIntoField(*fs, /*ownField=*/!isStaticMember(fs),
                               *as.value);
                return;
            }
            if (auto* prop = dynamic_cast<sema::PropertySymbol*>(sym)) {
                // An implicit-`this` setter call.
                if (thisMode_ && !isStaticMember(prop)) {
                    deny();
                    return;
                }
                visit(*as.value, Pos::Use);
                return;
            }
            deny();
            return;
        }
        const ast::Expr* recv = nullptr;
        if (auto* ma =
                dynamic_cast<const ast::MemberAccessExpr*>(as.target.get()))
            recv = ma->target.get();
        if (recv) {
            sema::Symbol* sym = ctx_.exprSymbol(*as.target);
            if (auto* fs = dynamic_cast<sema::FieldSymbol*>(sym)) {
                const Val tv = visit(*recv, Pos::FieldRecv);
                if (denied_) return;
                const auto* rid =
                    dynamic_cast<const ast::IdentifierExpr*>(recv);
                const bool recvIsThis = rid && rid->name == "this";
                if (tv == Val::Rooted && !(thisMode_ && recvIsThis)) {
                    // `t.f := v;` — a write through a local stores only a
                    // value that carries nothing.
                    const Val v = visit(*as.value, Pos::Use);
                    if (!denied_ && v != Val::NonCarrying) deny();
                    return;
                }
                storeIntoField(*fs,
                               /*ownField=*/recvIsThis && !isStaticMember(fs),
                               *as.value);
                return;
            }
            // A property setter: a call that receives the target.
            visit(*recv, Pos::UseOrType);
            visit(*as.value, Pos::Use);
            return;
        }
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(as.target.get())) {
            if (!ix->target) {
                deny();
                return;
            }
            visit(*ix->target, Pos::Use);
            visitArgs(ix->indices);
            visit(*as.value, Pos::Use);
            return;
        }
        deny();
    }

    void walkStmt(const ast::Stmt& st) {
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&st)) {
            walkLocalDecl(*ld);
        } else if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(&st)) {
            // §21.1: a `const` local never owns; its value is a use.
            if (sema::LocalSymbol* ls = sh_.localOf(*lc)) keepSymbol(ls);
            if (lc->value) visit(*lc->value, Pos::Use);
        } else if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&st)) {
            keep(rs->asyncResultIsOwnedObject);
            if (rs->value) visit(*rs->value, Pos::Use);
        } else if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&st)) {
            keep(es->disposeOwnedValue);
            if (!es->expr) {
                deny();
                return;
            }
            statementCall_ = es->expr.get();
            visit(*es->expr, Pos::Use);
            statementCall_ = nullptr;
        } else if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&st)) {
            walkAssignment(*as);
        } else if (auto* ts = dynamic_cast<const ast::ThrowStmt*>(&st)) {
            if (ts->value) visit(*ts->value, Pos::Throw);
        } else if (auto* is = dynamic_cast<const ast::IfStmt*>(&st)) {
            auto branch = [&](const ast::IfStmt::Branch& b) {
                if (!b.condition) {
                    deny();
                    return;
                }
                visit(*b.condition, Pos::Use);
                walkBody(b.body);
            };
            branch(is->ifBranch);
            for (const auto& b : is->elifBranches) branch(b);
            walkBody(is->elseBody);
        } else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&st)) {
            if (!ws->condition) {
                deny();
                return;
            }
            visit(*ws->condition, Pos::Use);
            walkBody(ws->body);
        } else if (auto* fs = dynamic_cast<const ast::ForStmt*>(&st)) {
            keep(fs->ownedIterVarAutoDropAtBodyEnd);
            // The iteration variable must not carry: a range, or an
            // iterable whose own type does not carry (a string).
            if (fs->isAwait || fs->withExpr || !fs->iterable) {
                deny();
                return;
            }
            if (auto* re = dynamic_cast<const ast::RangeExpr*>(
                    fs->iterable.get())) {
                visitRangeOperands(*re);
            } else {
                visit(*fs->iterable, Pos::Use);
                if (sh_.carries(ctx_.exprType(*fs->iterable))) {
                    deny();
                    return;
                }
            }
            walkBody(fs->body);
        } else if (auto* de = dynamic_cast<const ast::DoEndBlock*>(&st)) {
            walkBody(de->body);
        } else if (dynamic_cast<const ast::BreakStmt*>(&st) ||
                   dynamic_cast<const ast::ContinueStmt*>(&st)) {
            // no value
        } else if (auto* inc = dynamic_cast<const ast::IncrementStmt*>(&st)) {
            if (!inc->target || sh_.carries(ctx_.exprType(*inc->target))) {
                deny();
                return;
            }
            visit(*inc->target, Pos::Use);
        } else if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&st)) {
            keep(ds->disposeOwnedValue);
            keep(ds->disposeInlineOwnedArray);
            keep(ds->discardOutOwnedVar);
            keep(ds->disposeOwnedCollectionValue);
            keep(ds->disposeOwnedBuilderValue);
            if (!ds->value || sh_.carries(ctx_.exprType(*ds->value))) {
                deny();
                return;
            }
            visit(*ds->value, Pos::Use);
        } else if (auto* tr = dynamic_cast<const ast::TryStmt*>(&st)) {
            walkBody(tr->body);
            for (const auto& arm : tr->catches) {
                // A caught exception bound to a name is an object the
                // thrower created: outside the allow-list.
                if (arm.hasBinding || !arm.body) {
                    deny();
                    return;
                }
                if (arm.guard) visit(*arm.guard, Pos::Use);
                walkStmt(*arm.body);
            }
            walkBody(tr->finallyBody);
        } else if (auto* cs = dynamic_cast<const ast::CaseStmt*>(&st)) {
            keep(cs->disposeOwnedScrutinee);
            if (cs->scrutinee) {
                visit(*cs->scrutinee, Pos::Use);
                if (sh_.carries(ctx_.exprType(*cs->scrutinee))) {
                    deny();
                    return;
                }
            }
            for (const auto& arm : cs->arms) walkArm(arm);
        } else {
            // YieldStmt (an iterator getter), LocalTypeDecl and any
            // statement kind this allow-list does not name.
            deny();
        }
    }
};

bool Shared::classMembersConfine(const ast::TypeDecl& td) {
    for (const auto& bt : td.baseTypes) {
        if (!bt) return false;
        const sema::TypeRef* base = ctx.resolvedType(*bt);
        if (!base || base->kind != sema::TypeRef::Kind::Named ||
            !base->declaration)
            return false;
        if (base->declaration->kind == ast::TypeDeclKind::Interface) continue;
        if (!classIsConfining(base->declaration)) return false;
    }
    for (const auto& p : td.positionalFields)
        if (!p.modifier.empty()) return false;
    for (const auto& a : td.primaryBaseCtorArgs) {
        if (!a.modifier.empty() || a.isDiscard || !a.value) return false;
        if (!Screen(*this, true, nullptr).useExpression(*a.value))
            return false;
    }
    for (const auto& m : td.members) {
        if (!m) return false;
        if (auto* fd = dynamic_cast<const ast::FieldDecl*>(m.get())) {
            if (hasModifier(fd->modifiers, "static")) continue;
            if (fd->initializer &&
                !Screen(*this, true, nullptr)
                     .ownFieldInitializer(*fd->initializer))
                return false;
        } else if (auto* cd =
                       dynamic_cast<const ast::ConstructorDecl*>(m.get())) {
            if (cd->isStatic) continue;
            if (!cd->contractClauses.empty()) return false;
            for (const auto& p : cd->params)
                if (!p.modifier.empty()) return false;
            if (!Screen(*this, true, nullptr).body(cd->body)) return false;
        } else if (auto* dd =
                       dynamic_cast<const ast::DestructorDecl*>(m.get())) {
            if (!Screen(*this, true, nullptr).body(dd->body)) return false;
        } else if (auto* inv =
                       dynamic_cast<const ast::InvariantDecl*>(m.get())) {
            if (!inv->expr ||
                !Screen(*this, true, nullptr).useExpression(*inv->expr))
                return false;
        } else if (auto* pd =
                       dynamic_cast<const ast::PropertyDecl*>(m.get())) {
            if (hasModifier(pd->modifiers, "static")) continue;
            if (pd->autoInitializer &&
                !Screen(*this, true, nullptr)
                     .ownFieldInitializer(*pd->autoInitializer))
                return false;
        } else if (dynamic_cast<const ast::MethodDecl*>(m.get()) ||
                   dynamic_cast<const ast::OperatorDecl*>(m.get()) ||
                   dynamic_cast<const ast::EventDecl*>(m.get()) ||
                   dynamic_cast<const ast::ConstDecl*>(m.get()) ||
                   dynamic_cast<const ast::NestedTypeMember*>(m.get())) {
            // Not run by construction or disposal.
        } else {
            return false;
        }
    }
    return true;
}

bool Shared::classIsConfining(const sema::TypeSymbol* ts) {
    if (!ts || ts->kind != ast::TypeDeclKind::Class) return false;
    // SEAM-ACCESSOR: local-only by design. The question is "does this
    // class's SOURCE body pass the proof", and a builtin or imported class
    // has no body here: the null `decl` is the intended "unverified" answer,
    // which DENIES (its getter locals stay leaked, never freed).
    auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl);
    if (!td) return false;
    auto it = classes.find(ts);
    if (it != classes.end()) {
        if (it->second == Verdict::Pending) {
            // A class reached again while it is being verified: assumed
            // confining; the outermost verification decides.
            cycleSeen = true;
            return true;
        }
        return it->second == Verdict::Yes;
    }
    classes[ts] = Verdict::Pending;
    if (classDepth == 0) cycleSeen = false;
    ++classDepth;
    const bool ok = classMembersConfine(*td);
    --classDepth;
    if (!ok) {
        classes[ts] = Verdict::No;
    } else if (classDepth == 0 || !cycleSeen) {
        classes[ts] = Verdict::Yes;
    } else {
        // Verified only under an assumption about an enclosing class: not
        // cached, so a later failure of that class is not masked.
        classes.erase(ts);
    }
    return ok;
}

} // namespace

bool getterBodyIsConfined(const StmtList& body,
                          const std::vector<sema::Symbol*>& indexerParams,
                          sema::SemaContext& ctx,
                          GetterFlagSnapshot& snapshot) {
    for (const sema::Symbol* sym : indexerParams) {
        auto* ps = dynamic_cast<const sema::ParameterSymbol*>(sym);
        if (!ps || !ps->modifier.empty()) return false;
    }
    Shared sh(ctx);
    Screen screen(sh, /*thisMode=*/false, &snapshot);
    if (!screen.body(body)) {
        snapshot.flags.clear();
        return false;
    }
    return true;
}


namespace {

// fix #8944 (§28.3.3 *State match on reassignment* item 2, §28.7, v0.359): the
// syntactic walk behind `markGetterMovedFromReassignments`. It visits every
// statement and every sub-expression of a getter body, a nested lambda's
// body and a `case` arm included, and records (a) the locals that are the
// operand of a `move` anywhere in the body and (b) the plain `:=` stores to a
// bare local. The expression arms below name every `ast::Expr` kind; a leaf
// kind (a literal, `typeof`, `nameof`, `default`, `()`, `owned x`) holds no
// sub-expression.
class MovedLocalScan {
public:
    explicit MovedLocalScan(sema::SemaContext& ctx) : ctx_(ctx) {}

    void body(const StmtList& stmts) {
        for (const auto& s : stmts)
            if (s) stmt(*s);
    }

    // Set the §28.7 moved-from flag on every `:=` store to a local that is
    // moved from somewhere in the body.
    void apply() const {
        for (const auto& [as, sym] : stores_)
            if (moved_.count(sym)) as->lhsMovedFromAtStore = true;
    }

private:
    sema::SemaContext& ctx_;
    std::unordered_set<const sema::Symbol*> moved_;
    std::vector<std::pair<ast::AssignmentStmt*, const sema::Symbol*>> stores_;

    sema::LocalSymbol* localOfIdent(const ast::Expr* e) {
        while (auto* pb = dynamic_cast<const ast::PostfixBangExpr*>(e))
            e = pb->operand.get();
        auto* id = dynamic_cast<const ast::IdentifierExpr*>(e);
        if (!id) return nullptr;
        return dynamic_cast<sema::LocalSymbol*>(ctx_.exprSymbol(*id));
    }

    void args(const std::vector<ast::CallArgument>& as) {
        for (const auto& a : as) expr(a.value.get());
    }

    void arm(const ast::CaseArm& a) {
        expr(a.guard.get());
        expr(a.exprBody.get());
        body(a.stmts);
    }

    void expr(const ast::Expr* e) {
        if (!e) return;
        if (auto* x = dynamic_cast<const ast::UnaryExpr*>(e)) {
            if (x->op == "move")
                if (sema::LocalSymbol* ls = localOfIdent(x->operand.get()))
                    moved_.insert(ls);
            expr(x->operand.get());
            expr(x->replacement.get());
        } else if (auto* x = dynamic_cast<const ast::BinaryExpr*>(e)) {
            expr(x->left.get());
            expr(x->right.get());
        } else if (auto* x = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
            expr(x->target.get());
        } else if (auto* x = dynamic_cast<const ast::NullCondMemberExpr*>(e)) {
            expr(x->target.get());
        } else if (auto* x = dynamic_cast<const ast::CallExpr*>(e)) {
            expr(x->callee.get());
            args(x->args);
        } else if (auto* x = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
            expr(x->callee.get());
            args(x->args);
        } else if (auto* x = dynamic_cast<const ast::IndexExpr*>(e)) {
            expr(x->target.get());
            args(x->indices);
        } else if (auto* x = dynamic_cast<const ast::PostfixBangExpr*>(e)) {
            expr(x->operand.get());
        } else if (auto* x = dynamic_cast<const ast::NewExpr*>(e)) {
            args(x->args);
        } else if (auto* x = dynamic_cast<const ast::TypeTestExpr*>(e)) {
            expr(x->operand.get());
        } else if (auto* x = dynamic_cast<const ast::ConditionalExpr*>(e)) {
            expr(x->thenValue.get());
            expr(x->condition.get());
            expr(x->elseValue.get());
        } else if (auto* x = dynamic_cast<const ast::ThrowExpr*>(e)) {
            expr(x->operand.get());
        } else if (auto* x = dynamic_cast<const ast::RangeExpr*>(e)) {
            expr(x->lo.get());
            expr(x->hi.get());
            expr(x->step.get());
        } else if (auto* x = dynamic_cast<const ast::WithExpr*>(e)) {
            expr(x->source.get());
            for (const auto& ia : x->assigns) expr(ia.value.get());
        } else if (auto* x =
                       dynamic_cast<const ast::CollectionLiteralExpr*>(e)) {
            for (const auto& k : x->keys) expr(k.get());
            for (const auto& v : x->elements) expr(v.get());
        } else if (auto* x = dynamic_cast<const ast::LambdaExpr*>(e)) {
            for (const auto& p : x->params) expr(p.defaultValue.get());
            expr(x->exprBody.get());
            body(x->stmts);
        } else if (auto* x = dynamic_cast<const ast::CaseExpr*>(e)) {
            expr(x->scrutinee.get());
            for (const auto& a : x->arms) arm(a);
        } else if (auto* x = dynamic_cast<const ast::BindExpr*>(e)) {
            for (const auto& c : x->clauses)
                if (c) stmt(*c);
            expr(x->body.get());
        } else if (auto* x =
                       dynamic_cast<const ast::InterpolatedStringExpr*>(e)) {
            for (const auto& p : x->parts) expr(p.expr.get());
        }
    }

    void stmt(const ast::Stmt& st) {
        if (auto* s = dynamic_cast<const ast::LocalDecl*>(&st)) {
            expr(s->initializer.get());
        } else if (auto* s = dynamic_cast<const ast::LocalConstDecl*>(&st)) {
            expr(s->value.get());
        } else if (auto* s = dynamic_cast<const ast::ReturnStmt*>(&st)) {
            expr(s->value.get());
        } else if (auto* s = dynamic_cast<const ast::ExpressionStmt*>(&st)) {
            expr(s->expr.get());
        } else if (auto* s = dynamic_cast<const ast::AssignmentStmt*>(&st)) {
            if (s->op == ":=")
                if (sema::LocalSymbol* ls = localOfIdent(s->target.get()))
                    stores_.emplace_back(
                        const_cast<ast::AssignmentStmt*>(s), ls);
            expr(s->target.get());
            expr(s->value.get());
        } else if (auto* s = dynamic_cast<const ast::ThrowStmt*>(&st)) {
            expr(s->value.get());
        } else if (auto* s = dynamic_cast<const ast::IfStmt*>(&st)) {
            expr(s->ifBranch.condition.get());
            body(s->ifBranch.body);
            for (const auto& b : s->elifBranches) {
                expr(b.condition.get());
                body(b.body);
            }
            body(s->elseBody);
        } else if (auto* s = dynamic_cast<const ast::WhileStmt*>(&st)) {
            expr(s->condition.get());
            body(s->body);
        } else if (auto* s = dynamic_cast<const ast::ForStmt*>(&st)) {
            expr(s->iterable.get());
            expr(s->withExpr.get());
            body(s->body);
        } else if (auto* s = dynamic_cast<const ast::DoEndBlock*>(&st)) {
            body(s->body);
        } else if (auto* s = dynamic_cast<const ast::IncrementStmt*>(&st)) {
            expr(s->target.get());
        } else if (auto* s = dynamic_cast<const ast::DiscardStmt*>(&st)) {
            expr(s->value.get());
        } else if (auto* s = dynamic_cast<const ast::YieldStmt*>(&st)) {
            expr(s->value.get());
        } else if (auto* s = dynamic_cast<const ast::TryStmt*>(&st)) {
            body(s->body);
            for (const auto& c : s->catches) {
                expr(c.guard.get());
                if (c.body) stmt(*c.body);
            }
            body(s->finallyBody);
        } else if (auto* s = dynamic_cast<const ast::CaseStmt*>(&st)) {
            expr(s->scrutinee.get());
            for (const auto& a : s->arms) arm(a);
        }
    }
};

} // namespace

void markGetterMovedFromReassignments(const StmtList& body,
                                      sema::SemaContext& ctx) {
    MovedLocalScan scan(ctx);
    scan.body(body);
    scan.apply();
}

} // namespace bezel::ownership
