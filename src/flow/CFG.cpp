#include "flow/CFG.hpp"

#include "ast/Node.hpp"

namespace bezel::flow {

CFG::CFG() {
    entry_ = makeBlock(CFGBlock::Kind::Entry);
    exit_  = makeBlock(CFGBlock::Kind::Exit);
}

CFGBlock* CFG::makeBlock(CFGBlock::Kind k) {
    auto owned = std::make_unique<CFGBlock>();
    owned->kind = k;
    owned->id = nextId_++;
    CFGBlock* raw = owned.get();
    blocks_.push_back(std::move(owned));
    return raw;
}

void CFG::addEdge(CFGBlock* from, CFGBlock* to) {
    from->succs.push_back(to);
    to->preds.push_back(from);
}

namespace {

// Loop context: where `break` and `continue` should branch to. The CFG
// builder maintains a stack of these as it enters / exits loop bodies.
struct LoopContext {
    CFGBlock* header;   // continue target
    CFGBlock* afterEnd; // break target
};

class Builder {
public:
    Builder(CFG& cfg) : cfg_(cfg) {}

    // Build the CFG. Returns the last "live" block on exit so the caller
    // can wire it to the function-level exit — null when control reaches
    // the end of the body via a terminator (return / throw / unbounded
    // break) and there is no live block to connect.
    CFGBlock* build(
        const std::vector<std::unique_ptr<ast::Stmt>>& body) {
        CFGBlock* current = cfg_.entry();
        for (const auto& s : body) {
            if (!s) continue;
            CFGBlock* after = visitStmt(*s, current);
            if (!after) {
                // Unreachable continuation; subsequent stmts would be
                // dead code (sema diagnoses; we just track CFG shape).
                current = cfg_.makeBlock(CFGBlock::Kind::Linear);
                // Don't connect from the previous block — the terminator
                // already routed past us. The fresh block has no preds
                // and exists only to absorb the dead stmts so we don't
                // crash the visitor.
            } else {
                current = after;
            }
        }
        // If the last block has no successor edge, it's the live exit
        // path; return it. If it does (e.g. a dead-code fresh block has
        // nothing wired in OR a fresh block that absorbed dead stmts has
        // no preds), check explicitly.
        if (current->preds.empty() && current->stmts.empty() &&
            current != cfg_.entry()) {
            return nullptr;
        }
        return current;
    }

private:
    CFG& cfg_;
    std::vector<LoopContext> loops_;

    // Visit one statement starting from `from`. Returns the block that
    // continues the linear thread after this statement (which may be
    // `from` for straight-line stmts, or a fresh merge block for
    // branches, or null for non-returning stmts like `return`/`throw`).
    CFGBlock* visitStmt(const ast::Stmt& s, CFGBlock* from) {
        if (auto* ifStmt = dynamic_cast<const ast::IfStmt*>(&s)) {
            return visitIf(*ifStmt, from);
        }
        if (auto* whileStmt = dynamic_cast<const ast::WhileStmt*>(&s)) {
            return visitWhile(*whileStmt, from);
        }
        if (auto* forStmt = dynamic_cast<const ast::ForStmt*>(&s)) {
            return visitFor(*forStmt, from);
        }
        if (auto* doBlock = dynamic_cast<const ast::DoEndBlock*>(&s)) {
            return visitDoEnd(*doBlock, from);
        }
        if (dynamic_cast<const ast::ReturnStmt*>(&s)) {
            from->stmts.push_back(&s);
            cfg_.addEdge(from, cfg_.exit());
            return nullptr;
        }
        if (dynamic_cast<const ast::ThrowStmt*>(&s)) {
            // F1: throws are terminators routed to exit. Exception edges
            // for try/catch handlers arrive in F2.
            from->stmts.push_back(&s);
            cfg_.addEdge(from, cfg_.exit());
            return nullptr;
        }
        if (dynamic_cast<const ast::BreakStmt*>(&s)) {
            from->stmts.push_back(&s);
            if (!loops_.empty()) {
                cfg_.addEdge(from, loops_.back().afterEnd);
            } else {
                cfg_.addEdge(from, cfg_.exit());
            }
            return nullptr;
        }
        if (dynamic_cast<const ast::ContinueStmt*>(&s)) {
            from->stmts.push_back(&s);
            if (!loops_.empty()) {
                cfg_.addEdge(from, loops_.back().header);
            } else {
                cfg_.addEdge(from, cfg_.exit());
            }
            return nullptr;
        }
        // All other statements (LocalDecl, AssignmentStmt, ExpressionStmt,
        // LocalConstDecl, DiscardStmt, IncrementStmt, YieldStmt, ...) are
        // linear: append to the current block.
        from->stmts.push_back(&s);
        return from;
    }

    CFGBlock* visitIf(const ast::IfStmt& s, CFGBlock* from) {
        // Branch block carries the `if` AST node so diagnostics can point
        // at the condition. Successors: [then-entry, else-or-elif-entry,
        // (...)] — chained elifs produce nested branch blocks.
        CFGBlock* merge = cfg_.makeBlock(CFGBlock::Kind::Merge);
        visitIfBranches(s, from, merge);
        return merge;
    }

    void visitIfBranches(const ast::IfStmt& s, CFGBlock* from,
                         CFGBlock* merge) {
        CFGBlock* current = from;
        // If-branch.
        CFGBlock* ifEnd = visitBranch(s.ifBranch, current, merge);
        (void)ifEnd;
        // Else / elif chain.
        for (const auto& elif : s.elifBranches) {
            CFGBlock* elifBranchHead =
                cfg_.makeBlock(CFGBlock::Kind::Branch);
            cfg_.addEdge(current, elifBranchHead);
            CFGBlock* elifEnd =
                visitBranch(elif, elifBranchHead, merge);
            (void)elifEnd;
            current = elifBranchHead;
        }
        if (s.hasElse) {
            CFGBlock* elseEntry = cfg_.makeBlock(CFGBlock::Kind::Linear);
            cfg_.addEdge(current, elseEntry);
            Builder inner(cfg_);
            inner.loops_ = loops_;
            CFGBlock* elseExit = inner.runInto(s.elseBody, elseEntry);
            if (elseExit) cfg_.addEdge(elseExit, merge);
        } else {
            // No `else`: the branch's false edge falls through to merge.
            cfg_.addEdge(current, merge);
        }
    }

    CFGBlock* visitBranch(const ast::IfStmt::Branch& b, CFGBlock* from,
                          CFGBlock* merge) {
        // Convert `from` into a branch block by leaving its kind alone
        // but adding successor edges. To keep the contract clean, we
        // promote `from` to Branch kind.
        from->kind = CFGBlock::Kind::Branch;
        CFGBlock* bodyEntry = cfg_.makeBlock(CFGBlock::Kind::Linear);
        cfg_.addEdge(from, bodyEntry);
        // Walk the branch body in a sub-Builder so loops_ are preserved.
        Builder inner(cfg_);
        inner.loops_ = loops_;
        CFGBlock* end = inner.runInto(b.body, bodyEntry);
        if (end) cfg_.addEdge(end, merge);
        return end;
    }

    CFGBlock* visitWhile(const ast::WhileStmt& s, CFGBlock* from) {
        CFGBlock* header = cfg_.makeBlock(CFGBlock::Kind::Branch);
        CFGBlock* bodyEntry = cfg_.makeBlock(CFGBlock::Kind::Linear);
        CFGBlock* after = cfg_.makeBlock(CFGBlock::Kind::Linear);
        cfg_.addEdge(from, header);
        cfg_.addEdge(header, bodyEntry); // true edge
        cfg_.addEdge(header, after);     // false edge
        Builder inner(cfg_);
        inner.loops_ = loops_;
        inner.loops_.push_back({header, after});
        CFGBlock* bodyEnd = inner.runInto(s.body, bodyEntry);
        if (bodyEnd) cfg_.addEdge(bodyEnd, header); // back-edge
        return after;
    }

    CFGBlock* visitFor(const ast::ForStmt& s, CFGBlock* from) {
        // For-loop CFG mirrors while-loop: header (the iteration check),
        // body, after.
        CFGBlock* header = cfg_.makeBlock(CFGBlock::Kind::Branch);
        CFGBlock* bodyEntry = cfg_.makeBlock(CFGBlock::Kind::Linear);
        CFGBlock* after = cfg_.makeBlock(CFGBlock::Kind::Linear);
        cfg_.addEdge(from, header);
        cfg_.addEdge(header, bodyEntry);
        cfg_.addEdge(header, after);
        Builder inner(cfg_);
        inner.loops_ = loops_;
        inner.loops_.push_back({header, after});
        CFGBlock* bodyEnd = inner.runInto(s.body, bodyEntry);
        if (bodyEnd) cfg_.addEdge(bodyEnd, header);
        return after;
    }

    CFGBlock* visitDoEnd(const ast::DoEndBlock& s, CFGBlock* from) {
        // do/end is pure sequencing — same as inlining the body.
        Builder inner(cfg_);
        inner.loops_ = loops_;
        CFGBlock* end = inner.runInto(s.body, from);
        return end;
    }

    CFGBlock* runInto(
        const std::vector<std::unique_ptr<ast::Stmt>>& body,
        CFGBlock* start) {
        CFGBlock* current = start;
        for (const auto& s : body) {
            if (!s) continue;
            current = visitStmt(*s, current);
            if (!current) {
                current = cfg_.makeBlock(CFGBlock::Kind::Linear);
            }
        }
        return current;
    }
};

} // namespace

std::unique_ptr<CFG> buildCFG(
    const std::vector<std::unique_ptr<ast::Stmt>>& body) {
    auto cfg = std::make_unique<CFG>();
    Builder builder(*cfg);
    CFGBlock* end = builder.build(body);
    if (end) {
        cfg->addEdge(end, cfg->exit());
    }
    return cfg;
}

} // namespace bezel::flow
