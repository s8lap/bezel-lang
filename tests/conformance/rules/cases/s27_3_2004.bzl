//! case: s27_3_2004
//! doc: spec
//! section: 27.3
//! title: a lambda returning a fresh Item at an owned Item slot is admitted
//!   in return position
//! rule: its inferred return type must equal the target's return slot or
//!   implicitly convert to it
//! cites: 5.1, 28.3
//! expect: run
//! stdout: 7
module Conf;

class Item
  public method V(): int => 7;
end Item;

public class Program
  public static method Make(): (int) -> owned Item => lambda(x) => new Item();

  public static method Main()
    var f := Make();
    using var i := f(1);
    Console.WriteLine(i.V());
  end Main;
end Program;
