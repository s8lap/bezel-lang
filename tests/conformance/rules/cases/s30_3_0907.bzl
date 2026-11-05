//! case: s30_3_0907
//! doc: spec
//! section: 30.3
//! title: the await of an OwningTask parameter inside an async method binds the owned result, disposed once
//! rule: Awaiting an `OwningTask<T>` in an owned-consuming position remains the normal way to
//!   obtain the result
//! cites: 30.5, 28.3
//! expect: run
//! output: 20
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

  public static method TakeO(owned c: Cell): int
    return c.v;
  end TakeO;

  public static async method RA(t: OwningTask<Cell>): Task<int>
    var c := await t;
    return TakeO(move c);
  end RA;

  public static async method Main(): Task
    var i := 0;
    var s := 0;
    while i < 4 do
      using var t := Mk();
      s := s + await RA(t);
      i := i + 1;
    end while;
    Console.WriteLine(s);
    Console.WriteLine($"live {Cell.n}");
  end Main;
end Program;
