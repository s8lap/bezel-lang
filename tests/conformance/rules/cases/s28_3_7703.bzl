//! case: s28_3_7703
//! doc: spec
//! section: 28.3
//! title: a nested closure may throw its own owned parameter
//! rule: `throw x` is a source-visible kill
//! cites: 31.1
//! expect: run
//! stdout: c
module Conf;
import System;
public class R: Exception
  public constructor() end constructor;
end R;
public class Program
  public static method Main(): int
    var f := lambda(owned c: R): int => do
      var g := lambda(owned d: R): int => do throw d; end;
      try _ := g(new R()); catch var x: R => Console.WriteLine("c"); end;
      return 0;
    end;
    _ := f(new R());
    return 0;
  end Main;
end Program;
