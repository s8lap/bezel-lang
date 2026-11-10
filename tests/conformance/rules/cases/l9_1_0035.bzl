//! case: l9_1_0035
//! doc: stdlib
//! section: 9.1
//! title: a call returning a ReadOnlyStringSpan does not match the radix form of int.Parse
//! rule: public static pure method Parse(s: string, radix: int): int;
//! cites: 5.1, 22.6
//! expect: reject
//! diagnostic: radix
module Conf;
public class Program
  static method Window(): ReadOnlyStringSpan
    return new ReadOnlyStringSpan("255", 0, 3);
  end Window;

  public static method Main()
    Console.WriteLine(int.Parse(Window(), 16));
  end Main;
end Program;
