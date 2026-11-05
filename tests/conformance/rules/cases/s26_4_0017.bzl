//! case: s26_4_0017
//! doc: spec
//! section: 26.4
//! title: an extension block over Shared<T> where T: class is admitted and resolves on a Shared<Cell> receiver
//! rule: the extended type's declared constraints are decided there as at an instantiation site
//! cites: 14.2
//! expect: run
//! output: 1
//!   3
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

extension Shared<T> where T: class
  public method P(): int
    return 1;
  end P;
end;

public class Program
  public static method Main()
    using var s := new Shared<Cell>(new Cell(3));
    Console.WriteLine(s.P());
    Console.WriteLine(s.Get().v);
  end Main;
end Program;
