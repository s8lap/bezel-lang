//! case: l4_10_0038
//! doc: stdlib
//! section: 4.10
//! title: OwningSet<T>.Remove on a bound element has a bool result
//! rule: method Remove(item: T): bool;              // disposes the removed element (stdlib §4.10 OwningSet<T>, IOwningCollection<T>)
//! cites: 4.10
//! expect: run
//! output: ok
module Conf;

public class Cell
  public constructor(x: int) end constructor;
end Cell;

public class Program
  public static method Main()
    using var s := new OwningSet<Cell>();
    using var q := new Cell(1);
    var b: bool := s.Remove(q);
    if b then Console.WriteLine("ok"); else Console.WriteLine("ok"); end if;
  end Main;
end Program;
