//! case: s26_5_0066
//! doc: spec
//! section: 26.5
//! title: an extension on an implemented interface supplies the name to both the call and the bound reference
//! rule: for extension blocks on `x`'s type, base types, or implemented
//!   interfaces
//! cites: 26.5, 27.6
//! expect: run
//! issue: 7488
//! output: 55
//!   55
module Conf;

extension IEnumerable<int>
  public method Length(): int => 55;
end;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    Console.WriteLine(a.Length());
    var f: () -> int := a.Length;
    Console.WriteLine(f());
  end Main;
end Program;
