// fix #6404 / §28.6 (spec v0.338): the laundering peel follows the returned
// binding through SEVERAL locals, not just one. A one-local-only rule would
// leave this shape admitted: each hop is a bare identifier initializer, so
// producerState() reads Borrowed at every step and the fresh `new` at the head
// is invisible. Measured ACCEPTED before the fix. per §28.6, #6404

// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class Cell end Cell; class Factory public property Item: Cell get => do var a := new Cell(); var b := a; var c := b; return c; end; end Item; end Factory;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a property or indexer getter may not return a freshly-produced owned value — a getter is a borrow-producing position (properties and indexers do not carry `owned`), so the value would escape with no owner and leak; expose the factory as a method returning `owned T`
// CHECK-SAME: (§28.6)