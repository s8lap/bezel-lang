//! case: l9_1_0033
//! doc: stdlib
//! section: 9.1
//! title: a ReadOnlyStringSpan operand does not match the radix form of int.TryParse
//! rule: public static method TryParse(s: string, radix: int, out result: int): bool;
//! cites: 5.1, 22.6
//! expect: reject
//! diagnostic: radix
module Conf;
public class Program
  public static method Main()
    var sp := new ReadOnlyStringSpan("255", 0, 3);
    var v: int := 0;
    Console.WriteLine(int.TryParse(sp, 16, out v));
  end Main;
end Program;
