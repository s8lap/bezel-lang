//! case: s28_3_0222
//! doc: spec
//! section: 28.3
//! title: a self-assignment inside a while body is rejected
//! rule: A self-assignment is no exception: for an owned-state class-typed local
//!   `t`, `t := t;` stores the unmarked read `t`, a borrow, into an owned-state
//!   LHS and is rejected at the assignment by this rule, not a §28.7 discharge.
//! cites: 28.3, 28.7
//! expect: reject
//! diagnostic: self-assign
//! issue: 6609
module Conf;

public class Cell
  public value: int := 0;

  public constructor()
  end constructor;
end Cell;

public class Program
  public method Demo()
    var t := new Cell();
    var i := 0;
    while i < 3 do
      t := t;          // the same rule applies inside the loop body — REJECT
      t := new Cell();  // a fresh producer, so the back edge joins equal
      i := i + 1;
    end;
  end Demo;
end Program;
