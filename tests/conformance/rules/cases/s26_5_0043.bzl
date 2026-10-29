//! case: s26_5_0043
//! doc: spec
//! section: 26.5
//! title: a user extension LastIndexOf on Array<int> does not shadow the declared Array.LastIndexOf
//! rule: instance resolution is committed and extension blocks are **never consulted** for that name on that receiver ... Adding a real method to a type silently takes priority over an extension with the same name — by design.
//! cites: 26.5
//! expect: run
//! stdout: 2
module Conf;

extension Array<int>
  public method LastIndexOf(x: int): int => 42;
end;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 1];
    Console.WriteLine(a.LastIndexOf(1));
  end Main;
end Program;
