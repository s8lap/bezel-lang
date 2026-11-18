//! case: s27_7_1_7102
//! doc: spec
//! section: 27.7.1
//! title: `Object.ReferenceEquals` over two separately-built lambdas with identical text is false
//! rule: Two separately-constructed lambdas with identical source text are *not* equal
//! cites: 8.1, 22.12
//! expect: run
//! stdout: False
module Conf;
public class Program
  public static method Main()
    var c := lambda(): int => 42;
    var d := lambda(): int => 42;
    var r := Object.ReferenceEquals(c, d);
    Console.WriteLine(r);
  end Main;
end Program;
