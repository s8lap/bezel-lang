//! case: s28_3_3_3601
//! doc: spec
//! section: 28.3.3
//! title: a lambda's owned parameter self-assigned is rejected as the method form is
//! rule: A self-assignment is no exception: for an owned-state class-typed binding `t` — a local, or an
//!   `owned` parameter of a method, constructor or lambda — `t := t;` stores the unmarked read `t`, a
//!   borrow, into an owned-state LHS and is rejected at the assignment by this rule
//! cites: 28.3.3, 27.3
//! expect: reject
//! diagnostic: owned
//! issue: 8789
//! notes: The head now reaches a lambda's own `owned` parameter. Accepting twin s28_3_3_3602 (an owned right-hand side).
module Conf;

public class Cell
  public N: int;
  public constructor(n: int) N := n; end constructor;
  destructor end destructor;
end Cell;

public class Program
  public static method Main()
    var f: (owned Cell) -> int := lambda(owned p: Cell): int => do
      p := p;
      var n := p.N;
      p.Dispose();
      return n;
    end;
    Console.WriteLine(f(new Cell(1)));
  end Main;
end Program;
