//! case: l2_16_1802
//! doc: stdlib
//! section: 2.16
//! title: TryUpgrade bound with using var is released on break and continue edges
//! rule: on non-null the caller owns a fresh handle, discharged by the `using var`;
//!   a null binding's scope-end disposal is a no-op
//! cites: 28.13
//! expect: run
//! output: 66
//!   0
module Conf;

class Cell
  public static alive: int := 0;
  public constructor() Cell.alive := Cell.alive + 1; end constructor;
  destructor Cell.alive := Cell.alive - 1; end destructor;
end Cell;
class Program
  public static method Main()
    using var sh := new Shared<Cell>(new Cell());
    using var w := sh.Downgrade();
    var n := 0;
    for var i in 0..199 do
      using var s := w.TryUpgrade();
      if i mod 3 = 0 then continue; end;
      if i = 100 then break; end;
      if s <> null then n := n + 1; end;
    end;
    Console.WriteLine(n);
    sh.Dispose();
    Console.WriteLine(Cell.alive);
  end Main;
end Program;
