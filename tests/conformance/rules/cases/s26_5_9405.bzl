//! case: s26_5_9405
//! doc: spec
//! section: 26.5
//! title: an extension over TextWriter supplies a member the class lacks, on Console.Out and a typed local
//! rule: If the receiver's type declares no instance member of that name, the compiler searches the
//!   current module and all imported modules
//! cites: 26.5, stdlib 7.4
//! expect: run
//! output: 10
//! issue: 9193
module Conf;

import System.IO;

public extension TextWriter
  public method Shout(): int => 5;
end;

public class Program
  public static method Main()
    var w: TextWriter := Console.Out;
    var a := w.Shout();
    var b := Console.Out.Shout();
    Console.WriteLine(a + b);
  end Main;
end Program;
