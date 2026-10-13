//! case: s28_11_0037
//! doc: spec
//! section: 28.11
//! title: move with at a computed OwningArray index exchanges and leaves the collection unfrozen
//! rule: The index is any expression, evaluated once before the exchange,
//!   and the receiver is any expression naming an owning slot's holder — a
//!   field, a local, a parameter, an element borrow; neither is tracked,
//!   and nothing about the collection is frozen afterwards.
//! cites: 28.11, 28.8.1
//! expect: run
//! output: a b
//!   0
//! issue: 5923
module Conf;

public class Res
  public readonly property Name: string;

  public constructor(name: string)
    Name := name;
  end constructor;
end Res;

public class Program
  public static method Last(xs: OwningArray<Res>): int => xs.Count - 1;

  public static method Main()
    using var xs := new OwningArray<Res>();
    xs.Add(new Res("a"));
    using var t := move xs[Last(xs)] with new Res("b");
    Console.WriteLine($"{t.Name} {xs[0].Name}");
    xs.RemoveAt(0);
    Console.WriteLine(xs.Count);
  end Main;
end Program;
