//! case: l2_3_0027
//! doc: stdlib
//! section: 2.3
//! title: Convert.ToInt32 of a long is refused: the only overload takes a string
//! rule: stdlib §2.3 declares only `ToInt32(value: string): int`; a `long` argument does not convert to `string` (§5.1), so no overload applies and the call is a compile error.
//! cites: 2.3, 5.1
//! expect: reject
//! diagnostic: no matching overload
module Conf;

public class Program
  
  public static method Main()
    var l: long := 5;
    Console.WriteLine(Convert.ToInt32(l));
  end Main;
end Program;
