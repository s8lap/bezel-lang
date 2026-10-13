//! case: s15_1_0011
//! doc: spec
//! section: 15.1
//! title: TryPop on a Stack of an inline array is rejected at the call
//! rule: a standard-library member whose **signature** forms `T?` at a type
//!   parameter of its receiver type ... the `*OrNull` tier (`FirstOrNull`)
//!   and the `Try*(out item: T?)` family (`TryPop`, `TryReceive`) among them,
//!   is a conditionally-admitted member where that type argument is an
//!   inline array (§14.2): a compile error at the use
//! cites: 15.1, 14.2
//! expect: reject
//! diagnostic: inline array
//! issue: 5641
module Conf;

public class Program
  public static method Main()
    using var s := new Stack<[3]int>();
    var a: [3]int := [1, 2, 3];
    s.Push(a);
    var b: [3]int := [7, 8, 9];
    s.Push(b);
    var ok := s.TryPop(out var top);
    Console.WriteLine(ok);
  end Main;
end Program;
