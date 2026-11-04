//! case: s27_6_0040
//! doc: spec
//! section: 27.6
//! title: a receiver-qualified reference over a generic extension block binds per receiver
//! rule: and the resulting function has the method's own parameters only (`var
//!   scaled: () -> int := v.Scaled;`)
//! cites: 27.6
//! expect: run
//! issue: 7488
//! output: 77
//!   77
module Conf;

extension Array<T>
  public method Length(): int => 77;
end;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var s: Array<string> := ["a"];
    var f: () -> int := a.Length;
    var g: () -> int := s.Length;
    Console.WriteLine(f());
    Console.WriteLine(g());
  end Main;
end Program;
