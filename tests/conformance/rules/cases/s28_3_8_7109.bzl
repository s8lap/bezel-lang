//! case: s28_3_8_7109
//! doc: spec
//! section: 28.3.8
//! title: an expression-bodied method passes a capturing closure to a call-bounded function parameter
//! rule: admitted: invoked, and passed to a call-bounded parameter
//! cites: 28.3, 22.2
//! expect: run
//! output: 15
//!   zed
module Conf;
public class Item(name: string)
  public property Name: string => name;
end Item;
public class Program
  public static method Apply(f: (int) -> int, v: int): int => f(v);
  public static method Run(p: int): int => Apply(lambda(x: int) => x + p, 10);
  public static method Use(g: () -> string): string => g();
  public static method Run2(i: Item): string => Use(lambda() => i.Name);
  public static method Main()
    Console.WriteLine(Run(5));
    using var it := new Item("zed");
    Console.WriteLine(Run2(it));
  end Main;
end Program;
