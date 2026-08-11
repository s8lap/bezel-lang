#include "driver/ScopedAwaitBind.hpp"

#include <deque>
#include <memory>
#include <string>
#include <utility>

namespace bezel::proto {

const char kPatternARejectPrefix[] =
    "awaiting a class-typed `Task<T>` (Pattern A) requires";

namespace {

bool sameLoc(const SourceLocation& a, const SourceLocation& b) {
    return a.line == b.line && a.column == b.column && a.offset == b.offset;
}

// Storage for the synthesized slot names. `ast::LocalDecl::name` is a
// `string_view` into source text; a desugaring has no source text, so the
// names live here for the process's lifetime (a handful of bytes per rewritten
// site — a prototype's cost, and the same shape `InterpPart::exprSource` uses
// for sub-parsed expression sources).
//
// The name is derived from the site's own line/column, so it is DETERMINISTIC:
// two compilations of the same source produce byte-identical names (and hence
// byte-identical emitted objects), which the AOT test harness's content-
// addressed object cache requires. (Not thread-safe; the compiler compiles one
// unit per process and the AOT test harness forks per compile.)
std::string_view slotName(const SourceLocation& at) {
    static std::deque<std::string>& pool = *new std::deque<std::string>();
    pool.push_back("$awaitTask$L" + std::to_string(at.line) + "C" +
                   std::to_string(at.column));
    return pool.back();
}

class Rewriter {
public:
    explicit Rewriter(const std::vector<SourceLocation>& locs) : locs_(locs) {}

    std::size_t count() const { return count_; }

    void unit(ast::SourceFile& src) {
        for (const auto& d : src.topDecls)
            if (d) topDecl(*d);
    }

private:
    // ---- the trigger -------------------------------------------------------

    // Returns the await node when `ld` is the proposal §3 shape AND the
    // ownership pass rejected that await; nullptr otherwise. Note what is NOT
    // tested here: the operand's type, its owned-ness, the enclosing method's
    // async-ness, the borrow's source, or the slot's lifetime. All of that is
    // already decided — the site list is the unmodified §30.5 rejection.
    ast::UnaryExpr* triggeredAwait(ast::LocalDecl& ld) const {
        // §2: `using var x := await E;` / `await using var x := await E;` stay
        // rejected — the rule extends a lifetime, it manufactures no ownership.
        if (ld.isUsing) return nullptr;
        // §17.15 binding-atom clauses have the ATOM (K8.5), not a block, as
        // their boundary; there is no enclosing statement list to splice into.
        if (ld.isBindAtomClause) return nullptr;
        if (!ld.initializer) return nullptr;
        // §1/§3: the initializer must be EXACTLY the await.
        auto* aw = dynamic_cast<ast::UnaryExpr*>(ld.initializer.get());
        if (!aw || aw->op != "await" || !aw->operand) return nullptr;
        // §1: the operand is an unbound owned temporary — the compiler's own
        // `isUnboundProducerExpr` shape (Ownership_Checks.cpp): a call or a
        // `new`. The §30.5 diagnostic does not distinguish it on its own.
        if (!dynamic_cast<const ast::CallExpr*>(aw->operand.get()) &&
            !dynamic_cast<const ast::NewExpr*>(aw->operand.get()))
            return nullptr;
        for (const SourceLocation& at : locs_)
            if (sameLoc(at, aw->span.begin)) return aw;
        return nullptr;
    }

    // ---- the rewrite -------------------------------------------------------

    // `var x := await E;`  ⇒  `using var $t := E;` + `var x := await $t;`
    // The slot declaration is spliced in IMMEDIATELY BEFORE the bind, so its
    // position on the §28.10 establishment-ordered cleanup stack is exactly the
    // declaration's — "exactly where the hand-written `using var task` would
    // sit" (proposal §3).
    std::unique_ptr<ast::Stmt> makeSlot(ast::UnaryExpr& aw,
                                        const ast::LocalDecl& ld) {
        const std::string_view name = slotName(aw.span.begin);

        auto slot = std::make_unique<ast::LocalDecl>();
        slot->span = ld.span;
        slot->isUsing = true;      // §28.10 `using var` — the hidden slot
        slot->name = name;
        slot->initializer = std::move(aw.operand);   // the producer expression

        auto id = std::make_unique<ast::IdentifierExpr>();
        id->span = slot->initializer->span;
        id->name = name;
        aw.operand = std::move(id);                  // `await $t`

        ++count_;
        return slot;
    }

    // ---- the structural walk ----------------------------------------------

    void topDecl(ast::TopDecl& d) {
        if (auto* td = dynamic_cast<ast::TypeDecl*>(&d)) {
            for (auto& m : td->members)
                if (m) member(*m);
        } else if (auto* ext = dynamic_cast<ast::ExtensionDecl*>(&d)) {
            for (auto& m : ext->members)
                if (m) member(*m);
        }
    }

    void member(ast::Member& m) {
        // Only an `async` MethodDecl body can hold an await, and only a
        // MethodDecl can be `async` (§30.1: constructors, destructors, and
        // operators cannot). The other bodies are walked anyway — the site list
        // gates every rewrite, so extra reach costs nothing and cannot fire.
        if (auto* md = dynamic_cast<ast::MethodDecl*>(&m)) {
            stmts(md->stmts);
        } else if (auto* cd = dynamic_cast<ast::ConstructorDecl*>(&m)) {
            stmts(cd->body);
        } else if (auto* dd = dynamic_cast<ast::DestructorDecl*>(&m)) {
            stmts(dd->body);
        } else if (auto* od = dynamic_cast<ast::OperatorDecl*>(&m)) {
            stmts(od->stmts);
        } else if (auto* pd = dynamic_cast<ast::PropertyDecl*>(&m)) {
            for (auto& acc : pd->accessors) stmts(acc.stmts);
        } else if (auto* nt = dynamic_cast<ast::NestedTypeMember*>(&m)) {
            if (nt->decl)
                for (auto& im : nt->decl->members)
                    if (im) member(*im);
        }
    }

    void stmts(std::vector<std::unique_ptr<ast::Stmt>>& list) {
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (!list[i]) continue;
            if (auto* ld = dynamic_cast<ast::LocalDecl*>(list[i].get())) {
                if (ast::UnaryExpr* aw = triggeredAwait(*ld)) {
                    auto slot = makeSlot(*aw, *ld);
                    list.insert(list.begin() + static_cast<std::ptrdiff_t>(i),
                                std::move(slot));
                    ++i;   // step over the bind we just rewrote
                    continue;
                }
            }
            stmt(*list[i]);
        }
    }

    void stmt(ast::Stmt& s) {
        if (auto* is = dynamic_cast<ast::IfStmt*>(&s)) {
            stmts(is->ifBranch.body);
            for (auto& el : is->elifBranches) stmts(el.body);
            stmts(is->elseBody);
            return;
        }
        if (auto* ws = dynamic_cast<ast::WhileStmt*>(&s)) { stmts(ws->body); return; }
        if (auto* fs = dynamic_cast<ast::ForStmt*>(&s))   { stmts(fs->body); return; }
        if (auto* db = dynamic_cast<ast::DoEndBlock*>(&s)) { stmts(db->body); return; }
        if (auto* ts = dynamic_cast<ast::TryStmt*>(&s)) {
            stmts(ts->body);
            for (auto& arm : ts->catches)
                if (arm.body) stmt(*arm.body);
            stmts(ts->finallyBody);
            return;
        }
        if (auto* cs = dynamic_cast<ast::CaseStmt*>(&s)) {
            for (auto& arm : cs->arms) stmts(arm.stmts);
            return;
        }
    }

    const std::vector<SourceLocation>& locs_;
    std::size_t count_ = 0;
};

} // namespace

std::size_t rewriteScopedAwaitBinds(
    ast::SourceFile& unit, const std::vector<SourceLocation>& awaitLocs) {
    if (awaitLocs.empty()) return 0;
    Rewriter r(awaitLocs);
    r.unit(unit);
    return r.count();
}

} // namespace bezel::proto
