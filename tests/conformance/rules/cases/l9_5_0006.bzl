//! case: l9_5_0006
//! doc: stdlib
//! section: 9.5
//! title: char.IsDigit rejects an int argument
//! rule: char.IsDigit takes a char parameter; int to char is not in the
//!   implicit-conversion table of language spec 5.1 (char converts to int, not the
//!   reverse), so IsDigit(65) is rejected.
//! cites: 5.1
//! expect: reject
//! diagnostic: convert
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(char.IsDigit(65));
  end Main;
end Program;
