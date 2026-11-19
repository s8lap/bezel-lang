//! case: s28_3_7702
//! doc: spec
//! section: 28.3
//! title: a nested closure may not throw the enclosing closure's owned parameter
//! rule: a closure cannot consume, reassign, or dispose a captured owned binding
//! cites: 31.1
//! expect: reject
//! diagnostic: captured owned binding
module Conf;
import System;
public class R: Exception
  public constructor() end constructor;
end R;
public class Program
  public static method Main(): int
    var f := lambda(owned c: R): int => do
      var g := lambda(): int => do throw c; end;
      try _ := g(); catch var x: R => Console.WriteLine("c"); end;
      return 0;
    end;
    _ := f(new R());
    return 0;
  end Main;
end Program;
