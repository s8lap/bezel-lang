//! case: l2_16_1804
//! doc: stdlib
//! section: 2.16
//! title: a TryUpgrade result returned through an owned nullable result is discharged by the caller
//! rule: on non-null the caller owns a fresh handle, discharged by the `using var`;
//!   a null binding's scope-end disposal is a no-op
//! cites: 28.13
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
class Reg
  private owned _w: Weak<Cell>;
  public constructor(owned w: Weak<Cell>) _w := w; end constructor;
  public method Lookup(): owned Shared<Cell>? => _w.TryUpgrade();
end Reg;
class Program
  public static method Main()
    var n := 0;
    for var i in 0..99 do
      using var sh := new Shared<Cell>(new Cell());
      using var r := new Reg(sh.Downgrade());
      using var s := r.Lookup();
      if s <> null then n := n + 1; end;
    end;
    Console.WriteLine(n);
    Console.WriteLine(Cell.alive);
  end Main;
end Program;
