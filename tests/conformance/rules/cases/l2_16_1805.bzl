//! case: l2_16_1805
//! doc: stdlib
//! section: 2.16
//! title: TryUpgrade on a Weak<T> held in a generic body is typed Shared<T>? and released at scope end
//! rule: on non-null the caller owns a fresh handle, discharged by the `using var`;
//!   a null binding's scope-end disposal is a no-op
//! cites: 28.13
//! expect: run
//! output: 5
//!   0
module Conf;

class Cell
  public static alive: int := 0;
  public V: int := 5;
  public constructor() Cell.alive := Cell.alive + 1; end constructor;
  destructor Cell.alive := Cell.alive - 1; end destructor;
end Cell;
class Cache<T> where T: class
  private owned _w: Weak<T>;
  public constructor(owned w: Weak<T>) _w := w; end constructor;
  public method Alive(): bool
    using var s := _w.TryUpgrade();
    return s <> null;
  end Alive;
end Cache;
class Program
  public static method Main()
    using var sh := new Shared<Cell>(new Cell());
    var n := 0;
    do using var c := new Cache<Cell>(sh.Downgrade());
      for var i in 0..4 do if c.Alive() then n := n + 1; end; end;
    end;
    Console.WriteLine(n);
    sh.Dispose();
    Console.WriteLine(Cell.alive);
  end Main;
end Program;
