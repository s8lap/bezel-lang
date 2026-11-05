//! case: s28_7_0901
//! doc: spec
//! section: 28.7
//! title: reassigning a Result-bound local to a fresh value disposes the displaced Result
//! rule: if the previous value is live (non-null — and, for a binding, not moved-from), it is
//!   disposed; then the new value is stored
//! cites: 28.3, 30.3
//! expect: run
//! output: 214
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
    c := new Cell(7);
    if k then
      Boom();
    end if;
    return TakeO(move c);
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
