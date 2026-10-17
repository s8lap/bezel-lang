// fix #6404 / §28.6 (spec v0.338): the returned EXPRESSION is not the whole
// §28.6 rule. The rule is about the VALUE a getter returns and names its
// producers — "a `new` expression, a call returning `owned T`, a `move`
// expression, or an owned operator result" — but checkGetterFreshOwnedReturn
// asked producerState() of the returned expression, and an `IdentifierExpr`
// reads Borrowed. So ONE local laundered a fresh owned value past the check:
//
//     get => do var t := new SelfA(3); return t; end;      // accepted before
//
// The fix follows a returned BINDING back through the chain of locals the
// getter body declared. Companion ACCEPT rows: the sibling `926` rows
// (`getter_bare_field_read_accepted`, `method_returning_owned_new_accepted`,
// `auto_property_accepted`) and the reassignment-discharged control
// `getter_returns_fresh_owned_6404__block_getter_reassigned_local_accepted`.
// per §28.6, #6404

// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class Cell end Cell; class Factory public property Item: Cell get => do var t := new Cell(); return t; end; end Item; end Factory;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a property or indexer getter may not return a freshly-produced owned value — a getter is a borrow-producing position (properties and indexers do not carry `owned`), so the value would escape with no owner and leak; expose the factory as a method returning `owned T`
// CHECK-SAME: (§28.6)