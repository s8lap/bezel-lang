//! case: s28_3_0220
//! doc: spec
//! section: 28.3
//! title: a self-assignment inside a try body is rejected
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
    try
      t := t;          // the same rule applies inside the protected block
    catch
      InvalidOperationException => Console.WriteLine("caught")
    end try;
    t.Dispose();
  end Demo;
end Program;
