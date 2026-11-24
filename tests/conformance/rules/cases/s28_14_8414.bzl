//! case: s28_14_8414
//! doc: spec
//! section: 28.14
//! title: a virtual indexer with a string and a call-bounded function index invokes the closure
//! rule: The caller side is correspondingly unrestricted: any closure — capture-bearing or not
//!   — may be passed to a call-bounded parameter
//! cites: 28.14, 23.4
//! expect: run
//! stdout: 9290
//! notes: Pins #9068's two-argument shape. Per iteration the getter returns `q() + s.Length` =
//!   42 + (2 + digits(j)) for `ab{j}`. Sum over j = 0..199 = 200 * 42 + 200 * 2 + 490 =
//!   9290.
module Conf;
import System;
public class Res
  public V: int;
  public constructor() V := 42; end constructor;
end Res;
public class Fl
  public virtual property [s: string, q: () -> int]: int => q() + s.Length;
  public constructor() end constructor;
end Fl;
public class Program
  public static method Main(): int
    var t := 0;
    var fl := new Fl();
    for var j in 0..<200 do
      var r := new Res();
      t := t + fl[$"ab{j}", lambda() => r.V];
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
