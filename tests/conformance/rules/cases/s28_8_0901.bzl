//! case: s28_8_0901
//! doc: spec
//! section: 28.8
//! title: a Result-bound local moved into an owned parameter whose callee then throws is disposed exactly once
//! rule: A raise from the consuming call does not hand the obligation back.
//! cites: 28.3, 30.3
//! expect: run
//! output: 210
//!   live 0
//! issue: 8137
module Conf;

public class Cell
  public static n: int := 0;
  public v: int;

  public constructor(x: int)
    v := x;
    Cell.n := Cell.n + 1;
  end constructor;

  destructor
    Cell.n := Cell.n - 1;
  end destructor;
end Cell;

public class Program
  public static async method Mk(): OwningTask<Cell>
    return new Cell(5);
  end Mk;

  public static method Boom()
    throw new InvalidOperationException("boom");
  end Boom;

  public static method TakeO(owned c: Cell): int
    return c.v;
  end TakeO;

  public static method TakeOB(owned c: Cell, k: bool): int
    if k then
      Boom();
    end if;
    return c.v;
  end TakeOB;

  public static method Take(c: Cell): int
    return c.v;
  end Take;

  static method R(t: OwningTask<Cell>, k: bool): int
    var c := t.Result;
    return TakeOB(move c, k);
  end R;

  public static method Main()
    var i := 0;
    var s := 0;
    while i < 4 do
      using var t := Mk();
      try
        s := s + R(t, i mod 2 = 0);
      catch
        InvalidOperationException => s := s + 100;
      end try;
      i := i + 1;
    end while;
    Console.WriteLine(s);
    Console.WriteLine($"live {Cell.n}");
  end Main;
end Program;
