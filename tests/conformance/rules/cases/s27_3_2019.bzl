//! case: s27_3_2019
//! doc: spec
//! section: 27.3
//! title: a lambda with an expression body at a unit-returning slot is
//!   admitted and discards the result
//! rule: For procedure lambdas, the body may also be a single expression
//!   whose result is discarded
//! cites: 27.3, 21.9
//! expect: run
//! stdout: ok
module Conf;

class Item
  public method V(): int => 7;
end Item;

public class Program
  public static method Main()
    using var it := new Item();
    var f: (Item) -> () := lambda(x) => x.V();
    f(it);
    Console.WriteLine("ok");
  end Main;
end Program;
