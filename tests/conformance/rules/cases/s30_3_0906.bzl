//! case: s30_3_0906
//! doc: spec
//! section: 30.3
//! title: Result on an OwningTask inside an async method is rejected
//! rule: The compiler rejects `Wait()` and `Result` lexically inside `async` methods and `async`
//!   lambdas
//! cites: 30.5
//! expect: reject
//! diagnostic: inside async methods
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
    var c := t.Result;
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
