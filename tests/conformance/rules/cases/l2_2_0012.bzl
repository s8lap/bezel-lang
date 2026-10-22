//! case: l2_2_0012
//! doc: stdlib
//! section: 2.2
//! title: Sign of an unsigned operand above the signed maximum is 1 and of a zero one is 0
//! rule: public static method Sign<T>(x: T): int where T: INumber<T>; -1, 0, or 1:
//!   two `<` tests against.
//! cites: 2.2, 2.19
//! expect: run
//! output: 1
//!   1
//!   1
//!   0
//! issue: 6925
module Conf;

public class Program
  public static method Main()
    var u: uint := 4000000000u;
    var ul: ulong := 18000000000000000000ul;
    var b: byte := 200;
    var z: uint := 0u;
    Console.WriteLine(Math.Sign(u));
    Console.WriteLine(Math.Sign(ul));
    Console.WriteLine(Math.Sign(b));
    Console.WriteLine(Math.Sign(z));
  end Main;
end Program;
