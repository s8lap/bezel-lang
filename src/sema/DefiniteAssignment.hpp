#pragma once

// Definite-assignment analysis (§7 / §1.x — every read of a local must
// be preceded on every reaching path by at least one write).
//
// Slice DA1 is a syntactic walker over each method/ctor/dtor body —
// same shape as the slice-O1-O15 ownership analyzer. It tracks a per-
// scope set of definitely-assigned local *names* and reports reads of
// names not in the set. Branch joins take the intersection of branch
// exit sets (a local must be assigned on every reaching path).
//
// The pass is independent of the ownership analyzer; ownership has its
// own Uninitialized state that overlaps in spirit but applies only to
// Owned-local tracking. DA covers ALL locals (including borrowed and
// primitive ones).

namespace bezel::ast { struct SourceFile; }
namespace bezel::sema { class SemaContext; }

namespace bezel::sema {

// Run definite-assignment on `src`. Returns true if no DA errors were
// emitted (warnings don't count).
bool checkDefiniteAssignment(const ast::SourceFile& src, SemaContext& ctx);

} // namespace bezel::sema
