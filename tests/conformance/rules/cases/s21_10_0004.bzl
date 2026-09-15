//! case: s21_10_0004
//! doc: spec
//! section: 21.10
//! title: a local type must be declared before its first use
//! rule: The declaration must precede first use; local types are
//!   therefore order-significant.
//! cites: 21.10
//! expect: reject
//! diagnostic: undeclared
module Conf;

public class Program
  public static method Main()
    var t := new Tagged("x");
    record Tagged(Name: string);
    Console.WriteLine(t.Name);
  end Main;
end Program;
