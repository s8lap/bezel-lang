//! case: l4_5_0007
//! doc: stdlib
//! section: 4.5
//! title: TryPopLast reports false on an empty array and assigns null
//! rule: TryPopLast(out owned item: T?): bool; // v0.171: the false path
//!   assigns null.
//! cites: 4.5, 1
//! expect: run
//! output: False
//!   True
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    using var pool := new OwningArray<Widget>();
    var ok := pool.TryPopLast(out using var item);
    Console.WriteLine(ok);
    Console.WriteLine(item = null);
  end Main;
end Program;
