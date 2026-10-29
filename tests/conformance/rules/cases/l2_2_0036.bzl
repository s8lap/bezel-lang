//! case: l2_2_0036
//! doc: stdlib
//! section: 2.2
//! title: Math.Max and Math.Min over byte are generic and need no cast
//! rule: stdlib §2.2 declares `Max<T>`/`Min<T>` with `T: IComparable<T>`; a `byte` binds T exactly (§14.5), so unlike the non-generic Abs/Floor overloads (§22.6) no ambiguity arises. Max(200, 3) is 200, Min is 3.
//! cites: 2.2, 14.5
//! expect: run
//! output: 200
//!   3
module Conf;

public class Program
  
  public static method Main()
    var b: byte := byte(200);
    Console.WriteLine(Math.Max(b, byte(3)));
    Console.WriteLine(Math.Min(b, byte(3)));
  end Main;
end Program;
