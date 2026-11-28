//! case: s28_3_3_3602
//! doc: spec
//! section: 28.3.3
//! title: a lambda's owned parameter reassigned from an owned value disposes the displaced one
//! rule: Reassignment to an existing classified binding — a `var` local, or an `owned` or plain
//!   parameter of a method, constructor or lambda alike … — is state-matched: the RHS carries the LHS's
//!   classification
//! cites: 28.3.3, 27.3
//! expect: run
//! output: 400
//!   0
//! issue: 8789
//! notes: Accepting twin of s28_3_3_3601. Each call returns 2 and leaves no live Cell (the displaced
//!   Cell(1) is disposed at the reassignment, Cell(2) explicitly). 200 calls: 400, then 0 live.
module Conf;

public class Cell
  public static Live: int := 0;
  public N: int;
  public constructor(n: int) N := n; Cell.Live := Cell.Live + 1; end constructor;
  destructor Cell.Live := Cell.Live - 1; end destructor;
end Cell;

public class Program
  public static method Main()
    var f: (owned Cell) -> int := lambda(owned p: Cell): int => do
      p := new Cell(2);
      var n := p.N;
      p.Dispose();
      return n;
    end;
    var t := 0;
    for var i in 0 ..< 200 do
      t := t + f(new Cell(1));
    end for;
    Console.WriteLine(t);
    Console.WriteLine(Cell.Live);
  end Main;
end Program;
