//! case: s5_1_2016
//! doc: spec
//! section: 5.1
//! title: a lambda returning a class value at a nullable slot of the same
//!   class is admitted
//! rule: For any type `T`, `T` implicitly converts to `T?`
//! cites: 27.3, 7.1
//! expect: run
//! stdout: 7
module Conf;

class Item
  public method V(): int => 7;
end Item;

public class Program
  public static method Main()
    using var i := new Item();
    var f: (Item) -> Item? := lambda(x) => x;
    var o := f(i);
    if o <> null then Console.WriteLine(o.V()); end;
  end Main;
end Program;
