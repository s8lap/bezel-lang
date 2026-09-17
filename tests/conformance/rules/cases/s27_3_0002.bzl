//! case: s27_3_0002
//! doc: spec
//! section: 27.3
//! title: a zero-parameter lambda spells the empty parameter list, never omits it
//! rule: A lambda expression's parenthesized parameter list is the empty pair
//!   for zero parameters; there is no paren-free form.
//! cites: 27.3
//! expect: run
//! stdout: 0
module Conf;

public class Program
  public static method Main()
    var zero: () -> int := lambda() => 0;
    Console.WriteLine($"{zero()}");
  end Main;
end Program;
