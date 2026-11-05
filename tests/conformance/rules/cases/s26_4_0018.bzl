//! case: s26_4_0018
//! doc: spec
//! section: 26.4
//! title: an extension block over Weak<T> where T: class is admitted and resolves on a Weak<Cell> receiver
//! rule: the extended type's declared constraints are decided there as at an instantiation site
//! cites: 14.2
//! expect: run
//! output: 2
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

extension Weak<T> where T: class
  public method Q(): int
    return 2;
  end Q;
end;

public class Program
  public static method Main()
    using var s := new Shared<Cell>(new Cell(3));
    using var w := s.Downgrade();
    Console.WriteLine(w.Q());
  end Main;
end Program;
