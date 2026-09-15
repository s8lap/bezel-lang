//! case: l2_1_0004
//! doc: stdlib
//! section: 2.1
//! title: Out exposes the process's standard-output stream
//! rule: Console.Out is a readonly TextWriter property; writing through it
//!   reaches the same standard output Console.WriteLine writes to.
//! expect: run
//! stdout: hello
module Conf;

public class Program
  public static method Main()
    var w := Console.Out;
    w.WriteLine("hello");
  end Main;
end Program;
