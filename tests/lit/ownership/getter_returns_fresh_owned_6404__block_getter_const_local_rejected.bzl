// fix #6404 / §28.6 + §21.1 (spec v0.338): a `const` local is a DIFFERENT AST
// node from `var` — A.6 `LocalConstDecl ::= "const" Identifier (":" Type)? ":="
// ConstExpr ";"`, §21.1 *Runtime `const` locals* — and it holds its initializer
// in `value`, not `initializer`. A peel that collected only `LocalDecl` never
// saw it, so one `const` laundered a fresh owned value exactly as one `var`
// used to. Measured ACCEPTED before the fix.
//
// §21.1 makes the discharge that rescues the `var` rows impossible here: "A
// `const` local of either classification is not an assignable location:
// assignment, compound assignment, `++`/`--`, and binding to a `ref` or `out`
// parameter are compile errors naming the declaration." No store can retire the
// fresh value, so the escape is unconditional. Companion ACCEPT rows:
// `getter_returns_fresh_owned_6404__block_getter_reassigned_local_accepted` and
// the gtest twins. per §28.6, §21.1, A.6, #6404

// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class Cell end Cell; class Factory public property Item: Cell get => do const t := new Cell(); return t; end; end Item; end Factory;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a property or indexer getter may not return a freshly-produced owned value — a getter is a borrow-producing position (properties and indexers do not carry `owned`), so the value would escape with no owner and leak; expose the factory as a method returning `owned T`
// CHECK-SAME: (§28.6)