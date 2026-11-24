//! case: s28_14_8412
//! doc: spec
//! section: 28.14
//! title: a capture-bearing closure passed at a virtual indexer inside a try block is invoked
//! rule: The caller side is correspondingly unrestricted: any closure — capture-bearing or not
//!   — may be passed to a call-bounded parameter
//! cites: 28.14, 23.4, 31.2
//! expect: run
//! stdout: 8400
//! notes: Pins #9068's try shape. The closure literal is formed and passed inside the `try`;
//!   nothing throws, so the arm never runs and each iteration adds the closure's 42: 200 *
//!   42 = 8400.
module Conf;
import System;
public class Res
  public V: int;
  public constructor() V := 42; end constructor;
end Res;
public class Fl
  public virtual property [q: () -> int]: int => q();
  public constructor() end constructor;
end Fl;
public class Program
  public static method Nop(): int return 0; end Nop;
  public static method Main(): int
    var t := 0;
    var fl := new Fl();
    for var j in 0..<200 do
      var r := new Res();
      try
        t := t + fl[lambda() => r.V];
      catch
        Exception => Program.Nop(),
      end try;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
