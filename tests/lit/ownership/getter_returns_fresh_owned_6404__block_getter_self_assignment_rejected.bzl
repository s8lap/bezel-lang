// fix #6404 / §28.6 + §28.7 (spec v0.338): `t := t;` is a NO-OP, not a §28.7
// discharge. §28.7's drop-on-overwrite disposes "the previous value" — here the
// right-hand side IS the previous value, so nothing is retired and the local
// still holds the fresh `new` when the `return` reads it. The self-assignment
// no-op the spec states is §15.3's, and it is scoped there to whole-value
// reassignment of a MOVE-ONLY value type ("Whole-value reassignment `arr:=
// newValue;` on a move-only local or instance field … self-assignment is
// therefore a no-op") — not to a plain-`var` owning-classified local, which is
// the spelling here. The compiler already implements it as a no-op for such a
// local (ownership's `recollectAliasBorrowSourceOnReassign`; codegen's
// retain-before-release at every drop-on-overwrite site), so accepting this
// would be accepting a fresh-value escape. Measured ACCEPTED before the fix.
//
// The discharge this row is NOT: `getter_returns_fresh_owned_6404__block_getter
// _reassigned_local_accepted` stores a FIELD read, which genuinely retires the
// displaced value. per §28.6, §28.7, §15.3, #6404

// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class Cell end Cell; class Factory public property Item: Cell? get => do var t: Cell? := new Cell(); t := t; return t; end; end Item; end Factory;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a property or indexer getter may not return a freshly-produced owned value — a getter is a borrow-producing position (properties and indexers do not carry `owned`), so the value would escape with no owner and leak; expose the factory as a method returning `owned T`
// CHECK-SAME: (§28.6)