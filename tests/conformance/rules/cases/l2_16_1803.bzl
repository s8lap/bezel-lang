//! case: l2_16_1803
//! doc: stdlib
//! section: 2.16
//! title: TryUpgrade bound with using var is released on the exception path
//! rule: on non-null the caller owns a fresh handle, discharged by the `using var`;
//!   a null binding's scope-end disposal is a no-op
//! cites: 28.13, 28.10
//! expect: run
//! output: 100
//!   0
module Conf;

class Cell
  public static alive: int := 0;
  public constructor() Cell.alive := Cell.alive + 1; end constructor;
  destructor Cell.alive := Cell.alive - 1; end destructor;
end Cell;
class Program
  public static caught: int := 0;
  static method Inc() Program.caught := Program.caught + 1; end Inc;
  static method Boom(w: Weak<Cell>)
    using var s := w.TryUpgrade();
    if s <> null then
      throw new InvalidOperationException("x");
    end;
  end Boom;
  public static method Main()
    for var i in 0..99 do
      using var sh := new Shared<Cell>(new Cell());
      using var w := sh.Downgrade();
      try
        Program.Boom(w);
      catch
        InvalidOperationException => Program.Inc()
      end try;
    end;
    Console.WriteLine(Program.caught);
    Console.WriteLine(Cell.alive);
  end Main;
end Program;
