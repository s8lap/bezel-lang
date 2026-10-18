// fix #6404 / §28.6 (spec v0.338): the ACCEPT control for the binding peel's
// §28.7 stop. `t := _cell;` is a drop-on-overwrite (§28.7): the displaced fresh
// value is disposed AT THE STORE, so by the `return` the local holds a BORROW of
// the field and returning it is legal. A peel that followed the declaration
// alone would reject this program — this row is what makes that stop
// non-vacuous. per §28.6, §28.7, #6404

// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Cell end Cell; class Factory private _cell: Cell?; public constructor(c: Cell?) _cell := c; end constructor; public property Item: Cell? get => do var t: Cell? := new Cell(); t := _cell; return t; end; end Item; end Factory;

// NODIAG-NOT: error
// NODIAG-NOT: §