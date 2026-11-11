//! case: l9_1_4605
//! doc: stdlib
//! section: 9.1
//! title: int.ParseOrNull has no radix form
//! rule: string input only, no radix or span duals
//! cites: 9.1, 22.6
//! expect: reject
//! diagnostic: ParseOrNull(s: string)
module Conf;

public class Program
  public static method Main()
    var a := int.ParseOrNull("ff", 16);
    Console.WriteLine($"{a}");
  end Main;
end Program;
