//! case: s28_3_3_7623
//! doc: spec
//! section: 28.3.3
//! title: a nullable owned local reassigned after a move displaces nothing; both values are disposed
//! rule: A **moved-from** LHS is an owned-state LHS holding no live value: it requires an owned RHS and displaces nothing
//! cites: 28.3.3, 28.3.10
//! expect: run
//! output: 600
//!   0
module Conf;
import System;
class Cell
  public static n: int := 0;
  public v: int;
  public constructor(x: int) this.v := x; Cell.n := Cell.n + 1; end constructor;
  destructor Cell.n := Cell.n - 1; end destructor;
end Cell;
class H
  public property V: int
    get => do
      var t: Cell? := new Cell(1);
      var u: Cell? := move t;
      t := new Cell(2);
      return u!.v + t!.v;
    end;
  end V;
end H;
public class Program
  public static method Main(): int
    var h := new H();
    var acc := 0;
    for var i in 0 ..< 200 do
      acc := acc + h.V;
    end;
    Console.WriteLine(acc);
    Console.WriteLine(Cell.n);
    return 0;
  end Main;
end Program;
