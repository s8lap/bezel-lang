//! case: s28_3_0216
//! doc: spec
//! section: 28.3
//! title: a self-assignment to an Object-typed local that adopted an owned value is rejected
//! rule: The adopted state is that of the first assignment that reaches the local
//!   on each path — owned from a fresh producer or a marked transfer; from that
//!   assignment onward the local is state-matched by the rule above, so an
//!   owned-state LHS requires an owned RHS.
//! cites: 28.3
//! expect: reject
//! diagnostic: owned-state
//! issue: 6609
module Conf;

public class Cell
  public value: int := 0;

  public constructor()
  end constructor;
end Cell;

public class Program
  public method Demo()
    var o: Object := new Cell();   // the local adopts the owned state
    o := o;                        // the unmarked read of `o` is a borrow, stored
                                   // into an owned-state LHS — REJECT
  end Demo;
end Program;
