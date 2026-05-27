#pragma once

// Abstract syntax tree node definitions. The full node hierarchy derives from
// the grammar in Appendix A and the per-construct rules across §9–§27; it is
// built out by the AST/parser milestone. This stub establishes only the shared
// base (a source span) so other phases can name `Node` in their signatures.

#include "support/SourceLocation.hpp"

namespace bezel::ast {

// Base of every AST node. Concrete node types (declarations, expressions,
// statements, patterns) are added per the AST milestone, one set at a time,
// each citing the grammar production it implements.
struct Node {
    SourceSpan span;

    Node() = default;
    virtual ~Node() = default;
};

} // namespace bezel::ast
