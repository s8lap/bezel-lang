//! case: s7_5_0007
//! doc: spec
//! section: 7.5
//! title: the result of a null-conditional Count is nullable and does not convert to int
//! rule: The result type is always nullable: `x?.M` has the nullable type of M's result, so a non-nullable int binding of `b?.Count` is a compile error.
//! cites: 7.5
//! expect: reject
//! diagnostic: nullable
//! issue: 6024
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2];
    var b: Array<int>? := a;
    var n: int := b?.Count;
    Console.WriteLine($"{n}");
  end Main;
end Program;
