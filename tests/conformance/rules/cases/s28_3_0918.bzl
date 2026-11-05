//! case: s28_3_0918
//! doc: spec
//! section: 28.3
//! title: Result-bound locals in a loop with continue and break are each disposed exactly once
//! rule: the holder carries the disposal obligation: dispose exactly once, on every path
//! cites: 30.3, 28.3.10
//! expect: run
//! output: 240
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
    var s := 0;
    var j := 0;
    while j < 4 do
      using var t2 := Mk();
      var c := t2.Result;
      if j = 1 then
        s := s + TakeO(move c);
        j := j + 1;
        continue;
      end if;
      if k and j = 2 then
        Boom();
      end if;
      s := s + TakeO(move c);
      if j = 3 then
        break;
      end if;
      j := j + 1;
    end while;
    return s;
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
