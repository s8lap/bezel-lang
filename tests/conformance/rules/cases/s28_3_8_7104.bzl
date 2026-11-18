//! case: s28_3_8_7104
//! doc: spec
//! section: 28.3.8
//! title: an expression-bodied method may not return an if-chosen closure over a plain class parameter
//! rule: may not be returned from the enclosing method
//! cites: 28.3, 22.2
//! expect: reject
//! diagnostic: returned closure captures
module Conf;
public class Item(name: string)
  public property Name: string => name;
end Item;
public class Program
  public static method Label(i: Item, b: bool): () -> string => (if b then lambda() => i.Name else lambda() => "k");
  public static method Main()
    using var it := new Item("x");
    Console.WriteLine(Label(it, true)());
  end Main;
end Program;
