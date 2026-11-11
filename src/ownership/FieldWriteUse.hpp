#pragma once

// fix #7976 (§28.7 *Use after a dominating field write*, v0.357): a local
// initialized from a direct read of an owned field (`var b := r.f;`) that is
// used after a disposing write of the same path (`r.f := e;`, an explicit
// `r.f.Dispose()`) which follows the borrow with no invalidating operation
// between them and dominates the use is a compile error at the USE; the
// write itself stays permitted (K5.6).
//
// `body` is one member's block body; a block-bodied lambda inside it is
// checked as a body of its own. `allowParamRoots` is false for a body whose
// parameters may bind the caller's storage (§7.4 *Stable locals*: an
// operator operand, an `Equals(T)` parameter). The implementation lives in
// src/ownership/Ownership_FieldWriteUse.cpp.
//
// A header of its own, rather than a declaration in Ownership.hpp, because
// only Ownership.cpp calls the check: Ownership.hpp is included by the test
// suite's translation units and an edit to it recompiles every one of them.

#include <memory>
#include <vector>

namespace bezel::ast {
struct Stmt;
}
namespace bezel::sema {
class SemaContext;
}

namespace bezel::ownership {

void checkUseAfterDominatingFieldWrite(
        const std::vector<std::unique_ptr<ast::Stmt>>& body,
        sema::SemaContext& ctx, bool allowParamRoots);

} // namespace bezel::ownership
