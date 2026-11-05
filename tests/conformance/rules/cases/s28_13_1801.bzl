//! case: s28_13_1801
//! doc: spec
//! section: 28.13
//! title: a TryUpgrade result stored in an owned field keeps the value alive until its holder is disposed
//! rule: **`TryUpgrade(): owned Shared<T>?`** atomically produces a new strong
//!   handle if the value is still alive, and `null` otherwise
//! cites: 2.16
//! expect: run
//! output: 9
//!   1
//!   0
//! issue: 7508
module Conf;

class Cell
  public static alive: int := 0;
  public V: int := 9;
  public constructor() Cell.alive := Cell.alive + 1; end constructor;
  destructor Cell.alive := Cell.alive - 1; end destructor;
end Cell;
class Holder
  private owned _s: Shared<Cell>? := null;
  public constructor(w: Weak<Cell>)
    _s := w.TryUpgrade();
  end constructor;
  public method Read(): int
    if _s <> null then return _s!.Get().V; end;
    return -1;
  end Read;
end Holder;
class Program
  public static method Main()
    var w: Weak<Cell>? := null;
    do using var sh := new Shared<Cell>(new Cell());
      w := sh.Downgrade();
      using var h := new Holder(w!);
      sh.Dispose();
      Console.WriteLine(h.Read());
      Console.WriteLine(Cell.alive);
    end;
    Console.WriteLine(Cell.alive);
    w!.Dispose();
  end Main;
end Program;
