//! case: l9_1_0032
//! doc: stdlib
//! section: 9.1
//! title: a ReadOnlyStringSpan operand does not match the radix form of int.Parse
//! rule: public static pure method Parse(s: string, radix: int): int;
//! cites: 5.1, 22.6
//! expect: reject
//! diagnostic: radix
module Conf;
public class Program
  public static method Main()
    var sp := new ReadOnlyStringSpan("255", 0, 3);
    Console.WriteLine(int.Parse(sp, 16));
  end Main;
end Program;
