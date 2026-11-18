//! case: s28_3_8_7106
//! doc: spec
//! section: 28.3.8
//! title: an expression-bodied method returns a closure over a value-typed parameter (empty source set)
//! rule: While its source set is non-empty, a function value may not be stored in a field
//! cites: 28.3, 22.2
//! expect: run
//! stdout: 42
module Conf;
public class Item(name: string)
  public property Name: string => name;
end Item;
public class Program
  public static method Add(p: int): () -> int => lambda() => p + 1;
  public static method Main()
    var f := Add(41);
    Console.WriteLine(f());
  end Main;
end Program;
