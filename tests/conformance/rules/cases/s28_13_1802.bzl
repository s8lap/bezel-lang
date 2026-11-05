//! case: s28_13_1802
//! doc: spec
//! section: 28.13
//! title: a TryUpgrade result moved into an owned parameter is released exactly once by the callee
//! rule: **`TryUpgrade(): owned Shared<T>?`** atomically produces a new strong
//!   handle if the value is still alive, and `null` otherwise
//! cites: 2.16
//! expect: run
//! output: 100
//!   0
//! issue: 7508
module Conf;

class Cell
  public static alive: int := 0;
  public constructor() Cell.alive := Cell.alive + 1; end constructor;
  destructor Cell.alive := Cell.alive - 1; end destructor;
end Cell;
class Program
  static method Take(owned s: Shared<Cell>?): int
    using var t := move s;
    if t <> null then return 1; end;
    return 0;
  end Take;
  public static method Main()
    var n := 0;
    for var i in 0..99 do
      using var sh := new Shared<Cell>(new Cell());
      using var w := sh.Downgrade();
      var s := w.TryUpgrade();
      n := n + Program.Take(move s);
    end;
    Console.WriteLine(n);
    Console.WriteLine(Cell.alive);
  end Main;
end Program;
