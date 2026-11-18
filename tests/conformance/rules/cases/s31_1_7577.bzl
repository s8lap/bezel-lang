//! case: s31_1_7577
//! doc: spec
//! section: 31.1
//! title: a nested lambda throws its own owned parameter and the caller catches it
//! rule: The operand is consumed as an owned value under the ownership rules below
//! cites: 31.1
//! expect: run
//! stdout: caught
module Conf;
import System;
public class R: Exception
  public constructor() end constructor;
end R;
public class Program
  public static method Main(): int
    var f := lambda(): int => do
      var c := new R(); var b := c;
      var g := lambda(owned e: R): int => do throw e; end;
      try _ := g(new R()); catch var x: R => Console.WriteLine("caught"); end;
      return 0;
    end;
    _ := f();
    return 0;
  end Main;
end Program;
