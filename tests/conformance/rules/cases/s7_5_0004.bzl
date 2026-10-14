//! case: s7_5_0004
//! doc: spec
//! section: 7.5
//! title: a null-conditional Count on a non-null array is its count
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access. The result type is always nullable.
//! cites: 7.5
//! expect: run
//! stdout: 2
//! issue: 6024
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2];
    var b: Array<int>? := a;
    Console.WriteLine($"{b?.Count}");
  end Main;
end Program;
