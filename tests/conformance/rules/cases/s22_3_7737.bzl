//! case: s22_3_7737
//! doc: spec
//! section: 22.3
//! title: calling a closure at an owned slot with a marked owned local transfers it
//! rule: the caller writes the transfer marker at the call site: `M(move x)`
//! cites: 28.8
//! expect: run
//! output: gone
//!   1
module Conf;
import System;
public class Res
  public constructor() end constructor;
  destructor
    Console.WriteLine("gone");
  end destructor;
end Res;
public class Program
  public static method Go(): int
    var f := lambda(owned r: Res): int => do r.Dispose(); return 1; end;
    var a := new Res();
    return f(move a);
  end Go;
  public static method Main(): int
    Console.WriteLine(Program.Go());
    return 0;
  end Main;
end Program;
