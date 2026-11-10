//! case: l9_1_0037
//! doc: stdlib
//! section: 9.1
//! title: the radix form of Parse accepts a string-typed expression operand, not only a literal
//! rule: public static pure method Parse(s: string, radix: int): int;
//! cites: 5.1
//! expect: run
//! output: 255
//!   255
module Conf;
public class Program
  public static method Main()
    var maybe: string? := "ff";
    Console.WriteLine(int.Parse(maybe ?? "0", 16));
    Console.WriteLine(int.Parse("f" + "f", 16));
  end Main;
end Program;
