//! case: s28_13_1803
//! doc: spec
//! section: 28.13
//! title: TryUpgrade keeps the value alive past the original handle and answers null once it is gone
//! rule: **`TryUpgrade(): owned Shared<T>?`** atomically produces a new strong
//!   handle if the value is still alive, and `null` otherwise
//! cites: 2.16
//! expect: run
//! output: 1
//!   7
//!   0
//!   gone
//! issue: 7508
module Conf;

class Cell
  public static alive: int := 0;
  public V: int := 7;
  public constructor() Cell.alive := Cell.alive + 1; end constructor;
  destructor Cell.alive := Cell.alive - 1; end destructor;
end Cell;
class Program
  public static method Main()
    var sh := new Shared<Cell>(new Cell());
    using var w := sh.Downgrade();
    do using var up := w.TryUpgrade();
      sh.Dispose();
      Console.WriteLine(Cell.alive);
      if up <> null then Console.WriteLine(up.Get().V); end;
    end;
    Console.WriteLine(Cell.alive);
    using var again := w.TryUpgrade();
    if again = null then Console.WriteLine("gone"); end;
  end Main;
end Program;
