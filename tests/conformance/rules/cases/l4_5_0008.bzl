//! case: l4_5_0008
//! doc: stdlib
//! section: 4.5
//! title: Drain() yields owned elements and leaves the array empty
//! rule: Drain(): IOwningEnumerable<T>; // iteration drains the source.
//!   A for var x in arr.Drain() do ... end loop has owned iteration
//!   variables and consumes the array's elements.
//! cites: 4.5, 28.11, 29.2
//! expect: run
//! stdout: 0
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    using var pool := new OwningArray<Widget>();
    pool.Add(new Widget());
    for var w in pool.Drain() do
      using var taken := move w;
    end;
    Console.WriteLine(pool.Count);
  end Main;
end Program;
