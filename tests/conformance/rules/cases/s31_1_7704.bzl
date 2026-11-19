//! case: s31_1_7704
//! doc: spec
//! section: 31.1
//! title: throw of a closure's own owned local inside try/finally runs the finally then the handler
//! rule: `throw x` is a source-visible kill
//! cites: 28.3
//! expect: run
//! output: fin
//!   caught
module Conf;
import System;
public class R: Exception
  public constructor() end constructor;
end R;
public class Program
  public static method Main(): int
    var f := lambda(): int => do
      var c := new R();
      try throw c; finally Console.WriteLine("fin"); end;
    end;
    try _ := f(); catch var x: R => Console.WriteLine("caught"); end;
    return 0;
  end Main;
end Program;
