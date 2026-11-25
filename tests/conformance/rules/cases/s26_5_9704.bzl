//! case: s26_5_9704
//! doc: spec
//! section: 26.5
//! title: a property TextWriter does not declare is rejected on Console.Out through ?. as a read
//! rule: with a `?.` or postfix-`!` receiver read at its underlying non-nullable type
//! cites: 26.5, 7.5, stdlib 7.4
//! expect: reject
//! diagnostic: no member
//! notes: Pins #9223: the outcome is the same for every member access on a value, a property read
//!   included, so the `?.` read of an undeclared property names no member. The `.` spelling is
//!   s26_5_9402. Deletion test: removing the statement leaves a program that compiles and prints
//!   after.
module Conf;

public class Program
  public static method Main()
    var x := Console.Out?.Bogus;
    Console.WriteLine("after");
  end Main;
end Program;
