//! case: l5_0065
//! doc: stdlib
//! section: 5
//! title: Min and Max with a Guid selector raise InvalidOperationException on an empty source
//! rule: InvalidOperationException on an empty source
//! cites: 2.6, 5
//! expect: run
//! output: True
//!   True
//! issue: 8048
//! notes: twin of l5_0060 (pairs the accepted with its rejecting form).
module Conf;

public class Program
  public static method Main()
    using var empty := new Array<Guid>();
    var c1 := false;
    try Console.WriteLine(empty.Min(lambda(x: Guid) => x).ToString()); catch InvalidOperationException => c1 := true; end try;
    Console.WriteLine(c1);
    var c2 := false;
    try Console.WriteLine(empty.Where(lambda(x: Guid): bool => true).Max(lambda(x: Guid) => x).ToString()); catch InvalidOperationException => c2 := true; end try;
    Console.WriteLine(c2);
  end Main;
end Program;
