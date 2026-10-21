//! case: l2_2_0010
//! doc: stdlib
//! section: 2.2
//! title: Sign of a negative decimal is -1
//! rule: "public static method Sign<T>(x: T): int where T: INumber<T>;" returns "-1, 0, or 1", and "The
//!   primitive numeric tower (`byte`…`ulong`, `float`, `double`, `decimal`) conforms to the
//!   standard-library `INumber<T>`" ([§11.8]); Sign(-0.5m) is -1.
//! expect: run
//! stdout: -1
//! issue: 6297
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Math.Sign(-0.5m));
  end Main;
end Program;
