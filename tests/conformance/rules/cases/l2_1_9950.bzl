//! case: l2_1_9950
//! doc: stdlib
//! section: 2.1
//! title: Console.Write of the string? from Console.In.ReadLine() is rejected: no nullable overload
//! rule: has no applicable candidate and is a compile error
//! cites: 2.1, 7.4
//! expect: reject
//! diagnostic: nullable
//! notes: Pins #9260 (the call's type is string?, not string). The twin l2_1_9951 differs by `??
//!   "none"`.
module Conf;
public class Program
  public static method Main()
    Console.Write(Console.In.ReadLine());
  end Main;
end Program;
