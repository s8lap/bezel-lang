//! case: l5_0064
//! doc: stdlib
//! section: 5
//! title: Min and Max with a Guid selector work after lazy Where and Select
//! rule: public method Max<U>(selector: (T) -> U): U where U: IComparable<U>;
//! cites: 2.6, 5
//! expect: run
//! output: 00000000-0000-0000-0000-000000000005
//!   01000000-0000-0000-0000-000000000000
//!   01000000-0000-0000-0000-000000000000
//! issue: 8048
//! notes: twin of l5_0060 (pairs the accepted with its rejecting form).
module Conf;

public class Program
  public static method Main()
    using var arr := new Array<Guid>();
    arr.Add(Guid.Parse("00000000-0000-0000-0001-000000000000"));
    arr.Add(Guid.Parse("01000000-0000-0000-0000-000000000000"));
    arr.Add(Guid.Parse("00000000-0000-0000-0000-000000000005"));
    Console.WriteLine(arr.Where(lambda(x: Guid): bool => true).Min(lambda(x: Guid) => x).ToString());
    Console.WriteLine(arr.Where(lambda(x: Guid): bool => true).Max(lambda(x: Guid) => x).ToString());
    Console.WriteLine(arr.Select(lambda(x: Guid): Guid => x).Max(lambda(x: Guid) => x).ToString());
  end Main;
end Program;
