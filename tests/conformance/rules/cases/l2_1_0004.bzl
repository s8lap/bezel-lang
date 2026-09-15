//! case: l2_1_0004
//! doc: stdlib
//! section: 2.1
//! title: Out exposes the process's standard-output stream
//! rule: Console is a static class providing process-level standard output,
//!   and Out is a readonly TextWriter property over it; a file that imports a
//!   module may call the public members of a value reachable through its
//!   surface without importing the value's own declaring module (System.IO).
//! cites: 7.4
//! expect: run
//! stdout: hello
module Conf;

public class Program
  public static method Main()
    var w := Console.Out;
    w.WriteLine("hello");
  end Main;
end Program;
