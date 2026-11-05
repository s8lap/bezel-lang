//! case: l2_16_1806
//! doc: stdlib
//! section: 2.16
//! title: a null test on a TryUpgrade result narrows it so the handle is read without a null-forgiving postfix
//! rule: on non-null the caller owns a fresh handle, discharged by the `using var`;
//!   a null binding's scope-end disposal is a no-op
//! cites: 28.13, 7.5
//! expect: run
//! output: 4
//!   -1
//!   0
//! issue: 7508
module Conf;

class Cell
  public static alive: int := 0;
  public V: int := 4;
  public constructor() Cell.alive := Cell.alive + 1; end constructor;
  destructor Cell.alive := Cell.alive - 1; end destructor;
end Cell;
class Program
  static method Read(w: Weak<Cell>): int
    using var s := w.TryUpgrade();
    if s = null then return -1; end;
    return s.Get().V;
  end Read;
  public static method Main()
    using var sh := new Shared<Cell>(new Cell());
    using var w := sh.Downgrade();
    Console.WriteLine(Program.Read(w));
    sh.Dispose();
    Console.WriteLine(Program.Read(w));
    Console.WriteLine(Cell.alive);
  end Main;
end Program;
