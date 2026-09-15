//! case: s27_1_0005
//! doc: spec
//! section: 27.1
//! title: named parameters in a function type are documentation, calls are positional
//! rule: Function types may include parameter names; names are documentation
//!   only and do not affect type identity — `(int, int, int) -> bool` and the
//!   named form are the same type, and calls are positional.
//! cites: 27.1
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var handler: (itemId: int, quantity: int) -> bool := lambda(id: int, qty: int) => qty > 0;
    Console.WriteLine($"{handler(1, 5)}");
  end Main;
end Program;
