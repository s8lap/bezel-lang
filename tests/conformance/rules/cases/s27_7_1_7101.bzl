//! case: s27_7_1_7101
//! doc: spec
//! section: 27.7.1
//! title: `Object.ReferenceEquals` over one lambda value twice is true, over two different lambdas is false
//! rule: A **lambda** (closure) value equals only itself — reference equality on the closure instance.
//! cites: 8.1, 22.12
//! expect: run
//! output: True
//!   False
module Conf;
public class Program
  public static method Main()
    var c := lambda(): int => 42;
    var d := lambda(): int => 43;
    Console.WriteLine(Object.ReferenceEquals(c, c));
    Console.WriteLine(Object.ReferenceEquals(c, d));
  end Main;
end Program;
