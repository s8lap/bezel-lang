// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/ownership/getter_returns_fresh_owned_926_test.cpp
//   (BEZEL_TEST getter_returns_fresh_owned_926, block_getter_new_rejected).
//
// The #926 repro: a block-form `get => new Cell();`.

module M; class Cell end Cell; class Factory private _cell: Cell?; public static method MakeCell(): owned Cell return new Cell(); end MakeCell; public property Item: Cell get => new Cell(); end Item; end Factory;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a property or indexer getter may not return a freshly-produced owned value — a getter is a borrow-producing position (properties and indexers do not carry `owned`), so the value would escape with no owner and leak; expose the factory as a method returning `owned T`
// CHECK-SAME: (§28.6)
