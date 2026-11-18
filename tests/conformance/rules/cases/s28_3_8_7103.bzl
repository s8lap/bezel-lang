//! case: s28_3_8_7103
//! doc: spec
//! section: 28.3.8
//! title: an expression-bodied generic method may not return a closure over a plain class parameter
//! rule: may not be returned from the enclosing method
//! cites: 28.3, 22.2
//! expect: reject
//! diagnostic: returned closure captures
module Conf;
public class Item(name: string)
  public property Name: string => name;
end Item;
public class Program
  public static method Label<T>(i: Item, t: T): () -> string => lambda() => i.Name;
  public static method Main()
    using var it := new Item("x");
    var f := Label(it, 1);
    Console.WriteLine(f());
  end Main;
end Program;
