//! case: s7_5_0006
//! doc: spec
//! section: 7.5
//! title: a null-conditional Length on a non-null string is its length
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access. The result type is always nullable.
//! cites: 7.5
//! expect: run
//! stdout: 3
//! issue: 6024
module Conf;

public class Program
  public static method Main()
    var s: string? := "abc";
    Console.WriteLine($"{s?.Length}");
  end Main;
end Program;
