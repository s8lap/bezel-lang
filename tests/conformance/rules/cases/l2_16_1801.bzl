//! case: l2_16_1801
//! doc: stdlib
//! section: 2.16
//! title: TryUpgrade bound with using var is released on an early return out of its scope
//! rule: on non-null the caller owns a fresh handle, discharged by the `using var`;
//!   a null binding's scope-end disposal is a no-op
//! cites: 28.13, 28.10
//! expect: run
//! output: 200
//!   0
//! issue: 7508
module Conf;

class Cell
  public static alive: int := 0;
  public V: int := 3;
  public constructor() Cell.alive := Cell.alive + 1; end constructor;
  destructor Cell.alive := Cell.alive - 1; end destructor;
end Cell;
class Program
  static method Probe(w: Weak<Cell>): int
    using var s := w.TryUpgrade();
    if s <> null then
      return s.Get().V;
    end;
    return -1;
  end Probe;
  public static method Main()
    var sum := 0;
    for var i in 0..199 do
      using var sh := new Shared<Cell>(new Cell());
      using var w := sh.Downgrade();
      if Program.Probe(w) = 3 then sum := sum + 1; end;
    end;
    Console.WriteLine(sum);
    Console.WriteLine(Cell.alive);
  end Main;
end Program;
