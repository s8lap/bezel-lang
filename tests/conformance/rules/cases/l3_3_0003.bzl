//! case: l3_3_0003
//! doc: stdlib
//! section: 3.3
//! title: IndexOf of an empty needle returns 0
//! rule: The empty span or string is a valid needle: IndexOf of an empty
//!   needle returns 0.
//! cites: 3.3
//! expect: run
//! stdout: 0
module Conf;

public class Program
  public static method Main()
    var span := "abc".AsSpan();
    Console.WriteLine(span.IndexOf(""));
  end Main;
end Program;
