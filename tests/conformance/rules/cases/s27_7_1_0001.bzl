//! case: s27_7_1_0001
//! doc: spec
//! section: 27.7.1
//! title: two separately-constructed lambdas with identical source text are not equal
//! rule: A lambda (closure) value equals only itself — reference equality on
//!   the closure instance. Two separately-constructed lambdas with identical
//!   source text are not equal, even if they capture the same variables.
//! cites: 27.7.1
//! expect: run
//! stdout: False
module Conf;

public class Program
  public static method Main()
    var a: () -> int := lambda() => 1;
    var b: () -> int := lambda() => 1;
    Console.WriteLine($"{a = b}");
  end Main;
end Program;
