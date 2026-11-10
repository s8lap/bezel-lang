//! case: l9_1_0034
//! doc: stdlib
//! section: 9.1
//! title: an int operand does not match the radix form of int.Parse
//! rule: public static pure method Parse(s: string, radix: int): int;
//! cites: 5.1, 22.6
//! expect: reject
//! diagnostic: radix
module Conf;
public class Program
  public static method Main()
    Console.WriteLine(int.Parse(255, 16));
  end Main;
end Program;
