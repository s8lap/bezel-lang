//! case: s7_5_0008
//! doc: spec
//! section: 7.5
//! title: the result of a null-conditional Count binds to a nullable int and holds the count
//! rule: The result type is always nullable: `x?.M` binds to a nullable int, and a non-null receiver yields the count.
//! cites: 7.5
//! expect: run
//! stdout: 2
//! issue: 6024
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2];
    var b: Array<int>? := a;
    var n: int? := b?.Count;
    Console.WriteLine($"{n}");
  end Main;
end Program;
