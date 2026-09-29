// fix #4100 (§12.5, v0.300): an abstract indexer shorthand given an
// initializer (`abstract property [i: int]: T := expr;`) compiles clean
// but the initializer is dead code — an abstract member has no backing
// storage for it to write into.
//
// The named-property case was fixed in #4026; the indexer case was not
// covered because `isAbstractPropertyShorthand` (sema/Symbol.hpp) explicitly
// excludes indexers by design, and the initializers check keyed on that
// predicate alone. This adds the missing indexer branch.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M;
abstract class A public abstract property [i: int]: int := 42; end A;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: abstract indexer may not declare an initializer — an abstract member has no backing storage for it to write into, so the initializer could never run (§12.5)
