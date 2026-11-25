//! case: s26_5_9703
//! doc: spec
//! section: 26.5
//! title: a member TextWriter does not declare is rejected on Console.Error through ?. as a call
//! rule: with a `?.` or postfix-`!` receiver read at its underlying non-nullable type
//! cites: 26.5, 7.5, stdlib 7.4
//! expect: reject
//! diagnostic: no member
//! notes: Pins #9223: the Console.Error stream is a TextWriter like Console.Out, so the same undeclared-
//!   member refusal applies through `?.`.
module Conf;

public class Program
  public static method Main()
    Console.Error?.Bogus();
    Console.WriteLine("after");
  end Main;
end Program;
