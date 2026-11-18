// Ownership_FieldWriteUse.cpp — §28.7 *Use after a dominating field write*.
//
// fix #7976 (§28.7, v0.357).
//
// §28.7 *Use after a dominating field write* says:
//
//   "A local initialized from a direct read of an **owned** field — `var b :=
//    r.f;` or `const b := r.f;`, `r` being `this` (written or implicit) or a
//    local variable or non-`ref` parameter, `f` an owned instance field
//    reachable there — borrows the field's current value (§28.3). A
//    **disposing write** of the same path — a reassignment `r.f := e;`
//    (drop-on-overwrite, K3.11; the store of `null` at a nullable `f`
//    included) or an explicit `r.f.Dispose()` or direct `r.f.DisposeAsync()`
//    where §28.6 admits it — that follows the borrow with **no invalidating
//    operation between them** on any path from the borrow to the write […]
//    kills that value with certainty: nothing in the window can have taken it
//    out. […] A use of the local — a read, a member access, an argument, a
//    re-borrow — that such a write **dominates** on every path, the local a
//    stable local (§7.4 *Stable locals*) not assigned between the borrow and
//    the use, is a compile error […] The write itself stays permitted (K5.6):
//    what is rejected is the use the write makes certain to fault."
//
// MEASURED on the base (issue #7976's own repro, `var a := h.B!; h.B := new
// Box(); a.V := 3;`): the program compiled clean and the store through `a`
// was a heap-use-after-free under AddressSanitizer in unchecked disposal
// mode (a `DisposedAccessException` in checked disposal mode, §28.2). The
// overwrite released the block `a` still pointed at (K3.11), and no rule
// fired because this paragraph of §28.7 was not implemented.
//
// THE SHAPE OF THE PASS. A forward MUST-dataflow over one member body,
// independent of the main ownership dataflow (so it can neither read nor
// perturb that pass's states). Per local `b` it carries one FACT:
//
//   * WINDOW — `b` borrows the value the path `r.f` held at the borrow, and
//     on EVERY path from the borrow to here nothing that could have taken
//     that value out has run; or
//   * KILLED — a disposing write of `r.f` ran inside such a window on every
//     path to here, so the value `b` borrows is disposed with certainty.
//
// A use of `b` under a KILLED fact is the compile error. Facts meet by
// EQUALITY at a join (same borrow, same status, same write), which is what
// "dominates on every path" asks: a write on only some paths, or two
// different writes on two branches, leaves no fact and no diagnostic.
//
// FAIL-CLOSED. The window survives only what this file can PROVE inert (see
// `evalExpr`): literals, reads of locals, parameters and plain fields, `!`,
// null tests, type tests, and built-in operators over primitive value
// operands. EVERYTHING else — a call, `new`, an accessor, an index, `move`,
// `await`, a lambda, an interpolation, a collection literal, `?.`, a `using`
// declaration, a discard, a `yield`, a `for`, any assignment that can drop a
// value — ends every window. So a construct this file does not know ends the
// window rather than extending it: the pass can under-reject, never
// over-reject on that axis. What it under-rejects stays under §28.7's
// runtime-checked posture, exactly as before this fix.
//
// `r.f!` IS READ AS THE DIRECT READ `r.f`. §17.8: "`x!` is purely a
// type-system assertion (with a runtime null check); it does not move, copy,
// or borrow beyond what reading `x` already does", and §28.3.4's
// announcement table gives `x!` "`x`'s own classification … a borrow keeps
// `x`'s source". fix #7963 reads the same `!` the same way for the same
// owned-field-read alias fact (`peelForceUnwraps`, Ownership.cpp). The issue's
// own repro spells the borrow `h.B!` — the only spelling a nullable owned
// field admits for a non-null local. The peel is the single call to
// `peelBang` in `fieldPathOfBorrow`.
//
// NOT IMPLEMENTED HERE (each stays runtime-checked, as on the base):
//   * the sentence's second error — "a read of `r.f` itself after an explicit
//     `r.f.Dispose()`" — which is #7436's remaining leg, not #7976's shape;
//   * a borrow, a write or a use that sits in a statement nested inside an
//     EXPRESSION of the walked body other than a block-bodied lambda (those
//     are walked as bodies of their own, as are method, constructor,
//     destructor, operator and property-accessor block bodies);
//   * a compound assignment `r.f op= e` (treated as an invalidating
//     operation, not as a disposing write).

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/Node.hpp"
#include "common/BuiltinRegistry.hpp"
#include "ownership/ExprWalker.hpp"
#include "ownership/FieldWriteUse.hpp"
#include "ownership/StmtWalker.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"
#include "support/Diagnostic.hpp"

namespace bezel::ownership {

namespace {

using StmtList = std::vector<std::unique_ptr<ast::Stmt>>;

// ---------------------------------------------------------------------------
// The dataflow state.
// ---------------------------------------------------------------------------

// A one-segment field path `r.f`. `root == nullptr` is `this` (written or
// implicit); otherwise the root is a local's or a parameter's symbol.
struct FieldPath {
    const sema::Symbol* root = nullptr;
    const sema::FieldSymbol* field = nullptr;
    bool operator==(const FieldPath& o) const {
        return root == o.root && field == o.field;
    }
};

struct Fact {
    FieldPath path;
    std::string_view local;      // `b`
    std::string pathText;        // `r.f`, as the borrow spelled it
    SourceLocation borrowLoc;
    bool killed = false;
    SourceLocation killLoc;      // meaningful when `killed`
    bool killIsDispose = false;  // an explicit `r.f.Dispose()`, not a store
    bool operator==(const Fact& o) const {
        return path == o.path && borrowLoc.offset == o.borrowLoc.offset &&
               killed == o.killed &&
               (!killed || killLoc.offset == o.killLoc.offset);
    }
};

// Keyed by the local's DECLARATION node (`LocalSymbol::decl`), which every
// reference to the local resolves to through `SemaContext::exprSymbol`.
struct State {
    bool reachable = true;
    std::map<const ast::Node*, Fact> facts;
    bool operator==(const State& o) const {
        return reachable == o.reachable && facts == o.facts;
    }
};

// The meet at a control-flow join: a fact survives only when BOTH paths
// carry the identical fact — the same borrow, the same status and, for a
// killed fact, the same write. An unreachable side contributes nothing.
void joinInto(State& into, const State& other) {
    if (!other.reachable) return;
    if (!into.reachable) {
        into = other;
        return;
    }
    for (auto it = into.facts.begin(); it != into.facts.end();) {
        auto o = other.facts.find(it->first);
        if (o == other.facts.end() || !(o->second == it->second))
            it = into.facts.erase(it);
        else
            ++it;
    }
}

void clearWindows(State& st) {
    for (auto it = st.facts.begin(); it != st.facts.end();) {
        if (!it->second.killed)
            it = st.facts.erase(it);
        else
            ++it;
    }
}

const ast::Expr* peelBang(const ast::Expr* e) {
    while (auto* pb = dynamic_cast<const ast::PostfixBangExpr*>(e))
        e = pb->operand.get();
    return e;
}

bool hasModifier(const sema::MemberSymbol& m, std::string_view want) {
    for (std::string_view mod : m.modifiers)
        if (mod == want) return true;
    return false;
}

// A primitive VALUE type: a store of one drops nothing and a built-in
// operator over one runs no user code.
bool isPrimitiveValue(const sema::TypeRef* t) {
    if (!t || t->kind != sema::TypeRef::Kind::Primitive) return false;
    switch (t->primitive) {
        case sema::PrimitiveType::Bool:
        case sema::PrimitiveType::Char:
        case sema::PrimitiveType::Byte:
        case sema::PrimitiveType::SByte:
        case sema::PrimitiveType::Short:
        case sema::PrimitiveType::UShort:
        case sema::PrimitiveType::Int:
        case sema::PrimitiveType::UInt:
        case sema::PrimitiveType::Long:
        case sema::PrimitiveType::ULong:
        case sema::PrimitiveType::Float:
        case sema::PrimitiveType::Double:
        case sema::PrimitiveType::Decimal:
            return true;
        default:
            return false;
    }
}

// A class-, interface- or `Object`-typed value: the local holds a reference
// to the block the field owns. (The same classification
// Ownership_Extension.cpp's `receiverIsReference` makes.)
bool isReferenceTyped(const sema::TypeRef* t) {
    if (!t) return false;
    if (t->kind == sema::TypeRef::Kind::Primitive)
        return t->primitive == sema::PrimitiveType::Object;
    if (t->kind != sema::TypeRef::Kind::Named) return false;
    if (t->declaration) {
        if (t->declaration->isValueBuiltin) return false;
        return t->declaration->kind == ast::TypeDeclKind::Class ||
               t->declaration->kind == ast::TypeDeclKind::Interface;
    }
    // A builtin head has no parsed declaration.
    std::string_view n = t->headName();
    if (n.empty() || sema::isValueBuiltinByName(n)) return false;
    return common::isReservedBuiltinTypeName(n);
}

struct PendingDiag {
    const ast::Node* local;  // the fact's key — one diagnostic per local
    SourceLocation loc;
    std::string msg;
};

class FieldWriteUse {
public:
    FieldWriteUse(sema::SemaContext& ctx, bool allowParamRoots)
        : ctx_(ctx), allowParamRoots_(allowParamRoots) {}

    void run(const StmtList& body) {
        if (!bodyHasBorrowOfOwnedField(body)) return;
        collectCapturedNames(body);
        State st;
        execBlock(body, st);
        // One diagnostic per local, at its first rejected use in source
        // order. `pending_` is filled in walk order, which is not source
        // order across a loop's re-walks, so sort before deduplicating.
        std::stable_sort(pending_.begin(), pending_.end(),
                         [](const PendingDiag& a, const PendingDiag& b) {
                             return a.loc.offset < b.loc.offset;
                         });
        std::unordered_set<const ast::Node*> seen;
        for (const auto& d : pending_) {
            if (!seen.insert(d.local).second) continue;
            ctx_.diagnostics().report(Severity::Error, d.loc, d.msg);
        }
    }

private:
    // ---- recognizers -------------------------------------------------------

    // `e` as a one-segment path `r.f` over a FIELD (owned or not; the callers
    // ask). `r` is `this` (written or implicit) or a local or a parameter
    // that does not bind the caller's storage (§7.4 *Stable locals*).
    bool fieldPathOf(const ast::Expr& e, FieldPath& out, std::string& text) {
        auto* fs = dynamic_cast<const sema::FieldSymbol*>(ctx_.exprSymbol(e));
        if (!fs) return false;
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
            if (id->name == "this") return false;
            out = FieldPath{nullptr, fs};
            text = std::string(id->name);
            return true;
        }
        auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e);
        if (!ma || !ma->target) return false;
        auto* rid = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get());
        if (!rid) return false;
        if (rid->name == "this") {
            out = FieldPath{nullptr, fs};
            text = "this." + std::string(ma->member);
            return true;
        }
        const sema::Symbol* rs = ctx_.exprSymbol(*rid);
        if (!rs) return false;
        if (dynamic_cast<const sema::LocalSymbol*>(rs)) {
            // a local root
        } else if (auto* ps = dynamic_cast<const sema::ParameterSymbol*>(rs)) {
            // §7.4 *Stable locals*: "is not a parameter that binds the
            // caller's storage: a `ref` parameter […] or a move-only operator
            // operand or `Equals(T)` parameter (§25.2)".
            if (!allowParamRoots_) return false;
            if (ps->modifier == "ref" || ps->modifier == "out" ||
                ps->modifier == "out owned")
                return false;
        } else {
            return false;
        }
        // §7.4 *Stable locals*: "not mutably captured". ANY capture of the
        // root disqualifies it here (fail-closed).
        if (captured_.count(rid->name)) return false;
        out = FieldPath{rs, fs};
        text = std::string(rid->name) + "." + std::string(ma->member);
        return true;
    }

    // §28.7: "`f` an owned instance field". A class's own field only: a
    // primary-constructor capture slot and a value kind's field are out.
    static bool isOwnedInstanceField(const sema::FieldSymbol& fs) {
        if (fs.posParam) return false;
        if (!fs.parent || fs.parent->kind != ast::TypeDeclKind::Class)
            return false;
        return hasModifier(fs, "owned") && !hasModifier(fs, "static");
    }

    // The initializer of `var b := r.f;` / `const b := r.f;` as the path it
    // borrows. `r.f!` is the direct read `r.f` (§17.8; see the file header).
    bool fieldPathOfBorrow(const ast::Expr* init, FieldPath& out,
                           std::string& text) {
        const ast::Expr* core = peelBang(init);
        if (!core) return false;
        if (!fieldPathOf(*core, out, text)) return false;
        // A BORROW of the value: the field holds a reference. A value-kind
        // field (an owning inline array, an owning choice, a record) is
        // copied or moved by a read, never aliased.
        return isOwnedInstanceField(*out.field) &&
               isReferenceTyped(ctx_.exprType(*core));
    }

    // Cheap pre-filter: a body with no `var b := <owned field read>`
    // declaration can hold no fact, so the walk is skipped.
    bool bodyHasBorrowOfOwnedField(const StmtList& body) {
        bool found = false;
        auto onStmt = [&](const ast::Stmt& s) {
            if (found) return;
            const ast::Expr* init = nullptr;
            if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s))
                init = ld->initializer.get();
            else if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(&s))
                init = lc->value.get();
            const ast::Expr* core = peelBang(init);
            if (!core) return;
            auto* fs =
                dynamic_cast<const sema::FieldSymbol*>(ctx_.exprSymbol(*core));
            if (fs && isOwnedInstanceField(*fs)) found = true;
        };
        for (const auto& s : body)
            if (s) forEachNestedStmt(*s, onStmt);
        return found;
    }

    // Every identifier a lambda body names. A local or a root named in one
    // is not a stable local for this pass (§7.4 says "mutably captured"; any
    // capture is treated as one — fail-closed).
    void collectCapturedNames(const StmtList& body) {
        auto noStmt = [](const ast::Stmt&) {};
        auto inLambda = [&](const ast::Expr& e) {
            if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e))
                captured_.insert(id->name);
        };
        auto onExpr = [&](const ast::Expr& e) {
            if (dynamic_cast<const ast::LambdaExpr*>(&e))
                walkExprDeep(e, inLambda, noStmt);
        };
        for (const auto& s : body)
            if (s) walkStmtDeep(*s, onExpr, noStmt);
    }

    // The names a statement tree assigns: an assignment or `++`/`--` target
    // that is a bare identifier, and a bare identifier passed as a `ref` /
    // `out` argument. By NAME — a superset of the bindings actually written,
    // which is the fail-closed direction for the callers (they drop facts).
    void collectAssignedNames(const ast::Stmt& s,
                              std::unordered_set<std::string_view>& out) {
        auto bare = [&](const ast::Expr* e) {
            if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(e))
                out.insert(id->name);
        };
        auto argsOf = [&](const std::vector<ast::CallArgument>& as) {
            for (const auto& a : as)
                if (!a.modifier.empty() && a.modifier != "scoped")
                    bare(a.value.get());
        };
        auto onExpr = [&](const ast::Expr& e) {
            if (auto* x = dynamic_cast<const ast::CallExpr*>(&e))
                argsOf(x->args);
            else if (auto* x = dynamic_cast<const ast::NullCondCallExpr*>(&e))
                argsOf(x->args);
            else if (auto* x = dynamic_cast<const ast::NewExpr*>(&e))
                argsOf(x->args);
            else if (auto* x = dynamic_cast<const ast::IndexExpr*>(&e))
                argsOf(x->indices);
        };
        auto onStmt = [&](const ast::Stmt& st) {
            if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&st))
                bare(as->target.get());
            else if (auto* inc = dynamic_cast<const ast::IncrementStmt*>(&st))
                bare(inc->target.get());
        };
        walkStmtDeep(s, onExpr, onStmt);
    }

    static void dropByName(State& st,
                           const std::unordered_set<std::string_view>& names) {
        if (names.empty()) return;
        for (auto it = st.facts.begin(); it != st.facts.end();) {
            if (names.count(it->second.local))
                it = st.facts.erase(it);
            else
                ++it;
        }
    }

    // A bare identifier that is WRITTEN (an assignment target, a `ref`/`out`
    // argument): the local it names no longer holds the borrow. Resolved by
    // symbol; an unresolved name falls back to a by-name drop (fail-closed).
    void dropAssigned(const ast::IdentifierExpr& id, State& st) {
        if (const sema::Symbol* sym = ctx_.exprSymbol(id)) {
            if (dynamic_cast<const sema::LocalSymbol*>(sym) && sym->decl)
                st.facts.erase(sym->decl);
            return;
        }
        for (auto it = st.facts.begin(); it != st.facts.end();) {
            if (it->second.local == id.name)
                it = st.facts.erase(it);
            else
                ++it;
        }
    }

    // ---- uses --------------------------------------------------------------

    void noteUse(const ast::IdentifierExpr& id, State& st) {
        if (!st.reachable) return;
        const sema::Symbol* sym = ctx_.exprSymbol(id);
        if (!sym || !dynamic_cast<const sema::LocalSymbol*>(sym) || !sym->decl)
            return;
        auto it = st.facts.find(sym->decl);
        if (it == st.facts.end() || !it->second.killed) return;
        const Fact& f = it->second;
        // The wording is the code-generation reference's §4.6 row for this
        // rule; §28.7: "the diagnostic names the use, the borrow and the
        // write."
        std::string msg = "use of `" + std::string(f.local) + "` after ";
        msg += f.killIsDispose ? "the explicit disposal of `" : "the write to `";
        msg += f.pathText + "` at line " + std::to_string(f.killLoc.line) +
               ", which precedes this use on every path with nothing between "
               "the borrow and the write that could have moved the value "
               "out; `" +
               std::string(f.local) + "` borrows the value `" + f.pathText +
               "` held at line " + std::to_string(f.borrowLoc.line) +
               ", and that value is disposed (§28.7 *Use after a dominating "
               "field write*). Use `" +
               std::string(f.local) +
               "` first, or take the value out with `move " + f.pathText +
               " with …`";
        pending_.push_back(PendingDiag{it->first, id.span.begin, std::move(msg)});
    }

    // ---- expressions -------------------------------------------------------

    // Walks `e` in `st`: reports every use of a killed local, drops the fact
    // of a local written through a `ref`/`out` argument, and answers whether
    // the expression is PROVABLY INERT — runs no user code, disposes nothing
    // and writes nothing (§28.7's and §7.4's invalidating forms). The caller
    // ends every window when it is not.
    bool evalExpr(const ast::Expr* e, State& st) {
        if (!e) return true;
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(e)) {
            if (id->name == "this") return true;
            noteUse(*id, st);
            const sema::Symbol* sym = ctx_.exprSymbol(*id);
            // A local, a parameter, or a plain field read (implicit `this`).
            // A property, a method group, a type name or an unresolved name
            // is not provably inert.
            return sym && (dynamic_cast<const sema::LocalSymbol*>(sym) ||
                           dynamic_cast<const sema::ParameterSymbol*>(sym) ||
                           dynamic_cast<const sema::FieldSymbol*>(sym));
        }
        if (dynamic_cast<const ast::IntLiteralExpr*>(e) ||
            dynamic_cast<const ast::FloatLiteralExpr*>(e) ||
            dynamic_cast<const ast::CharLiteralExpr*>(e) ||
            dynamic_cast<const ast::StringLiteralExpr*>(e) ||
            dynamic_cast<const ast::BoolLiteralExpr*>(e) ||
            dynamic_cast<const ast::NullLiteralExpr*>(e) ||
            dynamic_cast<const ast::NameOfExpr*>(e))
            return true;
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
            const bool inner = evalExpr(ma->target.get(), st);
            // A plain FIELD read; an accessor read is §7.4's invalidating
            // form, and anything unresolved is not provably inert.
            return inner && dynamic_cast<const sema::FieldSymbol*>(
                                ctx_.exprSymbol(*e)) != nullptr;
        }
        if (auto* pb = dynamic_cast<const ast::PostfixBangExpr*>(e))
            return evalExpr(pb->operand.get(), st);
        if (auto* tt = dynamic_cast<const ast::TypeTestExpr*>(e)) {
            const bool inner = evalExpr(tt->operand.get(), st);
            return inner && (tt->op == "is" || tt->op == "is not");
        }
        if (auto* ce = dynamic_cast<const ast::ConditionalExpr*>(e)) {
            const bool a = evalExpr(ce->condition.get(), st);
            const bool b = evalExpr(ce->thenValue.get(), st);
            const bool c = evalExpr(ce->elseValue.get(), st);
            return a && b && c;
        }
        if (auto* be = dynamic_cast<const ast::BinaryExpr*>(e)) {
            const bool a = evalExpr(be->left.get(), st);
            const bool b = evalExpr(be->right.get(), st);
            if (!a || !b || !be->left || !be->right) return false;
            // A null test compares references; it runs no operator.
            const bool nullTest =
                (be->op == "==" || be->op == "!=") &&
                (dynamic_cast<const ast::NullLiteralExpr*>(be->left.get()) ||
                 dynamic_cast<const ast::NullLiteralExpr*>(be->right.get()));
            if (nullTest) return true;
            // A built-in operator over primitive value operands. A
            // user-defined operator is §7.4's invalidating form, and a
            // string operator allocates.
            return isPrimitiveValue(ctx_.exprType(*be->left)) &&
                   isPrimitiveValue(ctx_.exprType(*be->right));
        }
        if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(e)) {
            const bool a = evalExpr(ue->operand.get(), st);
            const bool b = evalExpr(ue->replacement.get(), st);
            // `move` and `await` are invalidating; so is a user-defined
            // unary operator.
            const bool builtinOp =
                ue->op == "not" || ue->op == "-" || ue->op == "+";
            return a && b && builtinOp && !ue->replacement && ue->operand &&
                   isPrimitiveValue(ctx_.exprType(*ue->operand));
        }
        if (dynamic_cast<const ast::LambdaExpr*>(e)) {
            // A lambda body is not walked: every name it mentions is in
            // `captured_`, so no fact exists for a local it could use.
            return false;
        }
        // Everything else is an invalidating operation or is not provably
        // inert: a call, `?.`, an index, `new`, a `throw` expression, a
        // range, `with`, a collection literal, an interpolation, a `case`
        // expression, a binding atom, a marked transfer.
        auto argsOf = [&](const std::vector<ast::CallArgument>& as) {
            for (const auto& a : as) {
                if (!a.value) continue;
                if (!a.modifier.empty() && a.modifier != "scoped") {
                    // A `ref` / `out` argument WRITES the binding it names.
                    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                            a.value.get())) {
                        dropAssigned(*id, st);
                        continue;
                    }
                }
                evalExpr(a.value.get(), st);
            }
        };
        if (auto* x = dynamic_cast<const ast::CallExpr*>(e)) {
            evalExpr(x->callee.get(), st);
            argsOf(x->args);
            return false;
        }
        if (auto* x = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
            evalExpr(x->callee.get(), st);
            argsOf(x->args);
            return false;
        }
        if (auto* x = dynamic_cast<const ast::NewExpr*>(e)) {
            argsOf(x->args);
            return false;
        }
        if (auto* x = dynamic_cast<const ast::IndexExpr*>(e)) {
            evalExpr(x->target.get(), st);
            argsOf(x->indices);
            return false;
        }
        forEachChild(
            *e, [&](const ast::Expr& c) { evalExpr(&c, st); },
            [&](const ast::Stmt& s) {
                // A statement nested in an expression (a block-bodied `case`
                // arm, a binding atom's clause): a local it assigns no
                // longer holds its borrow, so its fact is dropped before the
                // uses are read.
                std::unordered_set<std::string_view> assigned;
                collectAssignedNames(s, assigned);
                dropByName(st, assigned);
                auto onExpr = [&](const ast::Expr& n) {
                    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&n))
                        noteUse(*id, st);
                };
                auto onStmt = [](const ast::Stmt&) {};
                walkStmtDeep(s, onExpr, onStmt);
            });
        return false;
    }

    // Evaluate `e` and end every window when it is not provably inert.
    void evalAndInvalidate(const ast::Expr* e, State& st) {
        if (!evalExpr(e, st)) clearWindows(st);
    }

    // ---- statements --------------------------------------------------------

    void execBlock(const StmtList& body, State& st) {
        for (const auto& s : body)
            if (s) execStmt(*s, st);
    }

    void declare(const ast::Node& decl, std::string_view name,
                 const ast::Expr* init, bool isUsing, State& st) {
        bool inert = evalExpr(init, st);
        // A `using` declaration owns (or pins) its value and disposes it at
        // scope end; an inline-array-typed binding may be a move-only value.
        if (isUsing) inert = false;
        if (init) {
            const sema::TypeRef* t = ctx_.exprType(*init);
            if (t && t->kind == sema::TypeRef::Kind::InlineArray) inert = false;
        }
        if (!inert) clearWindows(st);
        if (isUsing || !st.reachable) return;
        FieldPath path;
        std::string text;
        if (!fieldPathOfBorrow(init, path, text)) return;
        if (captured_.count(name)) return;
        Fact f;
        f.path = path;
        f.local = name;
        f.pathText = std::move(text);
        f.borrowLoc = decl.span.begin;
        st.facts[&decl] = std::move(f);
    }

    // Mark every WINDOW fact over `path` killed by the disposing write at
    // `loc`, and end every other window (the write disposes a value: for
    // another path that is "a disposal of another value").
    void kill(const std::vector<const ast::Node*>& victims, SourceLocation loc,
              bool isDispose, State& st) {
        clearWindowsExcept(victims, st);
        for (const ast::Node* key : victims) {
            auto it = st.facts.find(key);
            if (it == st.facts.end() || it->second.killed) continue;
            it->second.killed = true;
            it->second.killLoc = loc;
            it->second.killIsDispose = isDispose;
        }
    }

    static void clearWindowsExcept(const std::vector<const ast::Node*>& keep,
                                   State& st) {
        for (auto it = st.facts.begin(); it != st.facts.end();) {
            const bool kept =
                std::find(keep.begin(), keep.end(), it->first) != keep.end();
            if (!it->second.killed && !kept)
                it = st.facts.erase(it);
            else
                ++it;
        }
    }

    static std::vector<const ast::Node*> windowsOver(const FieldPath& path,
                                                     const State& st) {
        std::vector<const ast::Node*> out;
        for (const auto& [key, f] : st.facts)
            if (!f.killed && f.path == path) out.push_back(key);
        return out;
    }

    // `r.f.Dispose();` / `r.f.DisposeAsync();` (optionally awaited, the
    // receiver optionally `r.f!`) over an owned instance field path.
    bool explicitFieldDispose(const ast::Expr* e, FieldPath& path) {
        if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(e))
            if (ue->op == "await" && !ue->replacement) e = ue->operand.get();
        auto* call = dynamic_cast<const ast::CallExpr*>(e);
        if (!call || !call->args.empty() || !call->callee) return false;
        auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call->callee.get());
        if (!ma || !ma->target) return false;
        if (ma->member != "Dispose" && ma->member != "DisposeAsync")
            return false;
        const ast::Expr* recv = peelBang(ma->target.get());
        std::string text;
        if (!recv || !fieldPathOf(*recv, path, text)) return false;
        return isOwnedInstanceField(*path.field);
    }

    // The receiver expression of a statement `explicitFieldDispose` accepted.
    static const ast::Expr* disposeReceiver(const ast::Expr* e) {
        if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(e))
            if (ue->op == "await") e = ue->operand.get();
        auto* call = dynamic_cast<const ast::CallExpr*>(e);
        auto* ma = call ? dynamic_cast<const ast::MemberAccessExpr*>(
                              call->callee.get())
                        : nullptr;
        return ma ? ma->target.get() : nullptr;
    }

    void execAssignment(const ast::AssignmentStmt& as, State& st) {
        const ast::Expr* target = as.target.get();
        if (!target) {
            evalAndInvalidate(as.value.get(), st);
            return;
        }
        FieldPath path;
        std::string text;
        if (fieldPathOf(*target, path, text)) {
            // The root of `b.x := e` is a USE of `b`.
            if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(target))
                evalExpr(ma->target.get(), st);
            if (as.op == ":=" && isOwnedInstanceField(*path.field)) {
                // §28.7: the disposing write. "The window ends where the
                // write's right-hand side begins": the victims are the
                // windows open BEFORE the operand is evaluated, the operand
                // is read under the pre-write facts (a use of `b` in it is
                // before the write), and whatever the operand does cannot
                // rescue them.
                const std::vector<const ast::Node*> victims =
                    windowsOver(path, st);
                evalExpr(as.value.get(), st);
                kill(victims, as.span.begin, /*isDispose=*/false, st);
                return;
            }
            // Another field store. It is inert only when it stores a
            // primitive value into a plain field: nothing is dropped and no
            // accessor runs. A compound store of an owned field, `??=`, and
            // a store of a reference end every window (drop-on-overwrite is
            // a disposal the body does not show).
            const bool inertValue = evalExpr(as.value.get(), st);
            if (!inertValue || !isPrimitiveValue(ctx_.exprType(*target)))
                clearWindows(st);
            return;
        }
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(target)) {
            // A local (or parameter, or unresolved) target: a WRITE of the
            // binding, not a use of it.
            const bool inertValue = evalExpr(as.value.get(), st);
            dropAssigned(*id, st);
            if (!inertValue || !isPrimitiveValue(ctx_.exprType(*target)))
                clearWindows(st);
            return;
        }
        // An index, a property, a deeper path: accessor / indexer writes and
        // drop-on-overwrite of something this pass does not model.
        evalExpr(target, st);
        evalExpr(as.value.get(), st);
        clearWindows(st);
    }

    template <class Body>
    void execLoop(State& st, Body&& iteration) {
        // Fixpoint over the loop-head state. `iteration(head, exitState)`
        // walks one trip from `head`, leaving the back-edge state in `head`
        // and the state on the loop's normal exit in `exitState`.
        // Diagnostics of a trip whose head turned out not to be stable are
        // discarded. The head only ever loses facts (the meet with the entry
        // state), so this terminates; the cap is a belt-and-braces bound.
        const State entry = st;
        State head = entry;
        for (int round = 0;; ++round) {
            const std::size_t mark = pending_.size();
            loops_.emplace_back();
            State back = head;
            State exitState;
            iteration(back, exitState);
            LoopFrame fr = std::move(loops_.back());
            loops_.pop_back();
            State next = entry;
            joinInto(next, back);
            for (const State& c : fr.jumps) joinInto(next, c);
            const bool capped = round >= 16;
            if (next == head || capped) {
                State out = exitState;
                for (const State& b : fr.jumps) joinInto(out, b);
                if (capped) out.facts.clear();
                st = std::move(out);
                return;
            }
            pending_.resize(mark);
            head = std::move(next);
        }
    }

    void execStmt(const ast::Stmt& s, State& st) {
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
            declare(*ld, ld->name, ld->initializer.get(), ld->isUsing, st);
            return;
        }
        if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(&s)) {
            declare(*lc, lc->name, lc->value.get(), /*isUsing=*/false, st);
            return;
        }
        if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
            FieldPath path;
            if (explicitFieldDispose(es->expr.get(), path)) {
                // §28.7: "an explicit `r.f.Dispose()` or direct
                // `r.f.DisposeAsync()`" is a disposing write too.
                evalExpr(disposeReceiver(es->expr.get()), st);
                const std::vector<const ast::Node*> victims =
                    windowsOver(path, st);
                kill(victims, es->span.begin, /*isDispose=*/true, st);
                return;
            }
            evalAndInvalidate(es->expr.get(), st);
            return;
        }
        if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
            execAssignment(*as, st);
            return;
        }
        if (auto* inc = dynamic_cast<const ast::IncrementStmt*>(&s)) {
            const ast::Expr* t = inc->target.get();
            bool inert = false;
            if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(t)) {
                dropAssigned(*id, st);
                inert = true;
            } else {
                inert = evalExpr(t, st);
            }
            if (!inert || !t || !isPrimitiveValue(ctx_.exprType(*t)))
                clearWindows(st);
            return;
        }
        if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&s)) {
            // §28.7: "no disposal of another value (a discard, …)".
            evalExpr(ds->value.get(), st);
            clearWindows(st);
            return;
        }
        if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
            evalExpr(rs->value.get(), st);
            st.reachable = false;
            return;
        }
        if (auto* ts = dynamic_cast<const ast::ThrowStmt*>(&s)) {
            evalExpr(ts->value.get(), st);
            st.reachable = false;
            return;
        }
        if (auto* ys = dynamic_cast<const ast::YieldStmt*>(&s)) {
            // §28.7: "no `yield return` (the consumer runs at the
            // suspension)".
            evalExpr(ys->value.get(), st);
            clearWindows(st);
            if (ys->isBreak) st.reachable = false;
            return;
        }
        if (dynamic_cast<const ast::BreakStmt*>(&s) ||
            dynamic_cast<const ast::ContinueStmt*>(&s)) {
            // The jump's state reaches a loop head or a loop exit. It is
            // offered to EVERY enclosing loop, as both — more joins than the
            // program has, which can only lose facts.
            for (LoopFrame& fr : loops_) fr.jumps.push_back(st);
            st.reachable = false;
            return;
        }
        if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
            std::vector<State> ends;
            evalAndInvalidate(ifs->ifBranch.condition.get(), st);
            {
                State t = st;
                execBlock(ifs->ifBranch.body, t);
                ends.push_back(std::move(t));
            }
            for (const auto& el : ifs->elifBranches) {
                evalAndInvalidate(el.condition.get(), st);
                State t = st;
                execBlock(el.body, t);
                ends.push_back(std::move(t));
            }
            if (ifs->hasElse) execBlock(ifs->elseBody, st);
            for (const State& t : ends) joinInto(st, t);
            return;
        }
        if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
            execLoop(st, [&](State& cur, State& exitState) {
                evalAndInvalidate(ws->condition.get(), cur);
                exitState = cur;
                execBlock(ws->body, cur);
            });
            return;
        }
        if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
            // The iteration protocol (`GetEnumerator`, `MoveNext`, the
            // enumerator's disposal) is a sequence of calls.
            evalExpr(fs->iterable.get(), st);
            evalExpr(fs->withExpr.get(), st);
            clearWindows(st);
            execLoop(st, [&](State& cur, State& exitState) {
                clearWindows(cur);
                exitState = cur;
                execBlock(fs->body, cur);
                clearWindows(cur);
            });
            return;
        }
        if (auto* de = dynamic_cast<const ast::DoEndBlock*>(&s)) {
            execBlock(de->body, st);
            return;
        }
        if (auto* tr = dynamic_cast<const ast::TryStmt*>(&s)) {
            execTry(*tr, st);
            return;
        }
        if (auto* cs = dynamic_cast<const ast::CaseStmt*>(&s)) {
            // Fail-closed: pattern matching may bind, copy and test through
            // forms this pass does not model, so every window ends at the
            // scrutinee. A killed fact still flows into each arm (the write
            // dominates the arm).
            evalExpr(cs->scrutinee.get(), st);
            clearWindows(st);
            std::vector<State> ends;
            for (const auto& arm : cs->arms) {
                State t = st;
                evalAndInvalidate(arm.guard.get(), t);
                if (arm.bodyKind == ast::CaseArmBodyKind::Block)
                    execBlock(arm.stmts, t);
                else
                    evalAndInvalidate(arm.exprBody.get(), t);
                ends.push_back(std::move(t));
            }
            // `st` itself stands for the no-arm-matched edge.
            for (const State& t : ends) joinInto(st, t);
            return;
        }
        // `LocalTypeDecl` declares a type; nothing runs.
    }

    void execTry(const ast::TryStmt& tr, State& st) {
        // What holds wherever an exception may leave the body: only the
        // facts KILLED before the `try`, less every local the body assigns
        // (an exception can be raised between any two operations of the
        // body, so nothing established inside it is known at a handler).
        State atThrow = st;
        clearWindows(atThrow);
        std::unordered_set<std::string_view> assigned;
        for (const auto& b : tr.body)
            if (b) collectAssignedNames(*b, assigned);
        dropByName(atThrow, assigned);

        State normal = st;
        execBlock(tr.body, normal);
        for (const auto& arm : tr.catches) {
            State t = atThrow;
            evalAndInvalidate(arm.guard.get(), t);
            if (arm.body) execStmt(*arm.body, t);
            joinInto(normal, t);
            if (arm.body) collectAssignedNames(*arm.body, assigned);
        }
        if (tr.hasFinally) {
            // The `finally` body runs on the normal path AND on every
            // exceptional one. Its diagnostics are those that hold on all of
            // them; its effect on the code after the `try` is the normal
            // path's, so it is walked a second time, silently, for that.
            State onAnyPath = atThrow;
            dropByName(onAnyPath, assigned);
            joinInto(onAnyPath, normal);
            execBlock(tr.finallyBody, onAnyPath);
            const std::size_t mark = pending_.size();
            execBlock(tr.finallyBody, normal);
            pending_.resize(mark);
        }
        st = std::move(normal);
    }

    struct LoopFrame {
        std::vector<State> jumps;  // `break` / `continue` states
    };

    sema::SemaContext& ctx_;
    bool allowParamRoots_;
    std::unordered_set<std::string_view> captured_;
    std::vector<LoopFrame> loops_;
    std::vector<PendingDiag> pending_;
};

} // namespace

// fix #7976 (§28.7 *Use after a dominating field write*, v0.357). See the
// file header for the rule and the shape of the pass.
void checkUseAfterDominatingFieldWrite(
        const std::vector<std::unique_ptr<ast::Stmt>>& body,
        sema::SemaContext& ctx, bool allowParamRoots) {
    FieldWriteUse(ctx, allowParamRoots).run(body);
    // A block-bodied lambda is a body of its own: the borrow, the write and
    // the use the rule relates are all inside one invocation of it. The
    // outer pass does not walk a lambda's statements (a lambda expression
    // only closes the open windows), so each one — nested ones included,
    // the deep walk reaches them — is checked here exactly once. A lambda
    // parameter is a root unless it is `ref` / `out`; a local of the
    // enclosing body named as a root is still the same binding for the
    // whole window, because the window admits no operation that could
    // rebind it.
    auto noStmt = [](const ast::Stmt&) {};
    auto onExpr = [&](const ast::Expr& e) {
        if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(&e))
            if (lam->isBlockBody)
                FieldWriteUse(ctx, /*allowParamRoots=*/true).run(lam->stmts);
    };
    for (const auto& s : body)
        if (s) walkStmtDeep(*s, onExpr, noStmt);
}

} // namespace bezel::ownership
