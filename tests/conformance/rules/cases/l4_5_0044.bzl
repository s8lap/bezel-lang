//! case: l4_5_0044
//! doc: stdlib
//! section: 4.5
//! title: OwningArray<T>.Remove on a bound element has a bool result
//! rule: public method Remove(item: T): bool;                   // borrow-taking query; (stdlib §4.5 OwningArray<T>)
//! cites: 4.5
//! expect: run
//! output: ok
module Conf;

public class Cell
  public constructor(x: int) end constructor;
end Cell;

public class Program
  public static method Main()
    using var s := new OwningArray<Cell>();
    using var q := new Cell(1);
    var b: bool := s.Remove(q);
    if b then Console.WriteLine("ok"); else Console.WriteLine("ok"); end if;
  end Main;
end Program;
