//! case: s14_2_2308
//! doc: spec
//! section: 14.2
//! title: Math.Max over string, decimal and char is accepted
//! rule: A primitive conforms to the interfaces its standard-library
//!   declaration lists
//! cites: 2.2, 11.8
//! expect: run
//! output: b
//!   2.5
//!   b
//! notes: Accepting control (green on base and tip).
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Math.Max("a", "b"));
    Console.WriteLine(Math.Max(1.5m, 2.5m));
    Console.WriteLine(Math.Max('a', 'b'));
  end Main;
end Program;
