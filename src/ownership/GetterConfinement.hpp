#pragma once

// fix #8767 fix-forward 3 (§28.3, §28.3.10, v0.359): the CONFINEMENT PROOF a
// property getter's block body must pass before the method-body dataflow
// (`MethodOwnership::run`) is allowed to arm a scope-end free in it.
//
// §28.3 (v0.359): "A borrow cannot outlive its source." A method body
// enforces that sentence partly by REJECTION (the §28.3 escape rules) and
// partly by TRUST (field-store clause 3 admits a borrow of a scope-bound
// local passed to a plain parameter or receiver the callee stores). A getter
// body carries neither today: its rejections are an open ruling (#8850) and
// the trust admission has no checked backstop in the default build (#4602).
// So the getter path arms a free only for a body that provably hands NO
// borrow-carrying value derived from one of its locals to code outside the
// body, and creates its owned values only through constructors that provably
// do not publish `this`.
//
// The proof is an ALLOW-LIST: every statement and expression kind is denied
// unless it is one of the forms listed in Ownership_GetterConfinement.cpp,
// and an AST node kind this file does not know is denied. A body that is not
// proved confined is left exactly as it was before #8767 — the dataflow does
// not run in it and nothing is armed (the pre-fix bounded leak) — so a route
// the proof does not know costs a leak, never a use-after-free.
//
// A header of its own for the reason FieldWriteUse.hpp gives: only
// Ownership.cpp calls it, and Ownership.hpp is included by every test
// translation unit.

#include <memory>
#include <utility>
#include <vector>

namespace bezel::ast {
struct Stmt;
}
namespace bezel::sema {
class SemaContext;
struct Symbol;
}

namespace bezel::ownership {

// Every ownership-pass-written flag of every statement and expression node
// in a confined getter body, with its value before the dataflow ran. The
// proof's own walk collects it, so it covers exactly the nodes the proof
// accepted — lambda bodies, `case` arms and nested blocks included.
struct GetterFlagSnapshot {
    std::vector<std::pair<bool*, bool>> flags;
    void restore() const {
        for (const auto& [flag, before] : flags) *flag = before;
    }
};

// True iff `body` (a getter's block body; `indexerParams` are an indexer
// getter's parameters) is proved confined. `snapshot` is filled only then.
bool getterBodyIsConfined(const std::vector<std::unique_ptr<ast::Stmt>>& body,
                          const std::vector<sema::Symbol*>& indexerParams,
                          sema::SemaContext& ctx, GetterFlagSnapshot& snapshot);

// fix #8944 (§28.3.3 *State match on reassignment* item 2, §28.7, v0.359):
// for a getter body that does NOT get the method dataflow (not proved
// confined, or confined but restored after a suppressed error), set
// `AssignmentStmt::lhsMovedFromAtStore` on every `:=` store to a local that is
// the operand of a `move` anywhere in the body. Codegen's drop-on-overwrite
// pass (`disposeDisplacedOwnedClassLocal`) tests only the null half of §28.7's
// "non-null and not moved-from", and a plain `var` local's slot keeps the
// moved-out pointer, so without the flag the store frees the value the new
// owner holds. The mark is path-insensitive, so it errs toward the LEAK side
// (a store that in fact displaces a live value skips its disposal, the same
// bounded leak an unconfined getter body already has); it never adds a free.
void markGetterMovedFromReassignments(
    const std::vector<std::unique_ptr<ast::Stmt>>& body,
    sema::SemaContext& ctx);

} // namespace bezel::ownership
