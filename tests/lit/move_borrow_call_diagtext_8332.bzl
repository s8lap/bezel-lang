// fix #8332 (spec §28.8.1 *The `move` expression*, Appendix A.7 `MoveExpr`,
// v0.355): the BORROW-CALL diagnostic — the one the ownership pass reports
// for `move` applied to a call whose result is a borrow — used to list "or a
// call that is itself a fresh owned producer (a call returning `owned T`)"
// among the things `move` transfers.
//
// §28.8.1 closes the operand set: "No other operand is admitted (A.7): a
// fresh producer (a `new` expression, a call returning `owned T`, an owned
// operator result) needs no marker anywhere (K3.13) and takes none, so
// `move (new Cell(1))` is a compile error at every position, an `owned`
// argument and an owned-slot store alike; the remedy is to drop the `move`."
// A call returning `owned T` is therefore NOT one of the things `move`
// transfers, at any position — and the clause contradicted the compiler's
// own sibling diagnostic (narrow #7926: "the `move` marker is not written on
// a fresh producer (a call returning `owned T`) ... remove `move`").
//
// The clause is DROPPED; the sibling clause ("a named binding (a local, an
// `owned` parameter, or an `owned` field/collection slot, §28.8.1)") stays,
// because it is §28.8.1's own enumeration of the admitted operand shapes.
//
// The positive CHECK below pins the text as it now stands (the clause list
// ends at the sibling clause); the CHECK-NOT pins the dropped clause's
// absence. The CHECK-NOT is NOT the oracle on its own — a CHECK-NOT on a
// vanished literal passes vacuously — the positive CHECK is.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
module P;
class Cell
  public Id: int;
  public constructor(id: int) Id := id; end constructor;
  destructor end destructor;
end Cell;
class W
  public static method N(): int return 3; end N;
  public static method Main(): int
    var a := move W.N();
    Console.WriteLine(a.ToString());
    return 0;
  end Main;
end W;

// CHECK: `move` is not admitted on this call
// CHECK-SAME: `move` transfers only a named binding (a local, an `owned` parameter, or an `owned` field/collection slot, §28.8.1).
// CHECK-SAME: This call returns a plain (non-`owned`) type
// CHECK-NOT: fresh owned producer
