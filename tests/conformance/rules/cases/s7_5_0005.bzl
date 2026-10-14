//! case: s7_5_0005
//! doc: spec
//! section: 7.5
//! title: a null-conditional Count on a null array is null
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access. The null result is rendered as `null` in an interpolated hole (18.5).
//! cites: 7.5, 18.5
//! expect: run
//! stdout: null
//! issue: 6024
module Conf;

public class Program
  public static method Main()
    var b: Array<int>? := null;
    Console.WriteLine($"{b?.Count}");
  end Main;
end Program;
