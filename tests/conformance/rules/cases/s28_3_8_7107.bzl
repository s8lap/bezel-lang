//! case: s28_3_8_7107
//! doc: spec
//! section: 28.3.8
//! title: an expression-bodied method returns a capture-free closure beside a class parameter
//! rule: A capture-free function value is admitted at each of those positions
//! cites: 28.3, 22.2
//! expect: run
//! stdout: const
module Conf;
public class Item(name: string)
  public property Name: string => name;
end Item;
public class Program
  public static method Make(i: Item): () -> string => lambda() => "const";
  public static method Main()
    using var it := new Item("x");
    var f := Make(it);
    Console.WriteLine(f());
  end Main;
end Program;
