//! case: s28_3_8_7110
//! doc: spec
//! section: 28.3.8
//! title: an expression-bodied method returns a curried capture-free closure
//! rule: A capture-free function value is admitted at each of those positions
//! cites: 28.3, 22.2
//! expect: run
//! stdout: 7
module Conf;
public class Item(name: string)
  public property Name: string => name;
end Item;
public class Program
  public static method Adder(): (int) -> (int) -> int => lambda(a: int) => lambda(b: int) => a + b;
  public static method Main()
    Console.WriteLine(Adder()(3)(4));
  end Main;
end Program;
